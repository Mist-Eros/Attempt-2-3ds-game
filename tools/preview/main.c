/*
 * main.c - scripted PC preview driver for Elemental Defense (3DS)
 *
 * Runs the real platform-independent game code (source/app.c, render.c, ui.c,
 * core .c files) against the software gfx.h backend in plat_host.c, feeding it
 * a deterministic scripted Input sequence instead of a 3DS controller.  Every
 * captured frame is dumped as two PNGs (top 400x240, bottom 320x240) so the
 * layout can be inspected without hardware.
 *
 * Usage (see tools/preview/Makefile):
 *   preview.exe [--scenario NAME] [--frames N] [--capture-every K]
 *               [--out DIR] [--seed S]
 *
 * Scenarios: menu, build, wave, selected, gameover, victory, all (default all)
 *
 * The driver never pokes at rendering code; it only talks to app.h plus the
 * read-only ui_*_rect() hit boxes that ui.h exposes for exactly this purpose.
 * For the result screens it forces Game.phase directly (there is no scripted
 * way to lose 20 lives in 30 seconds), which is what those states are: pure
 * overlays over the last battlefield frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

#include "app.h"
#include "audio.h"
#include "game.h"
#include "gfx.h"
#include "render.h"
#include "ui.h"
#include "host.h"

#define PREVIEW_DT       (1.0f / 60.0f)
#define DEFAULT_FRAMES   1800   /* 30 s of game time per scenario */
#define DEFAULT_SEED     1337u

/* ------------------------------------------------------------------ */
/* driver state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    App app;
    const char *scenario;
    const char *outdir;
    int max_frames;
    int capture_every;
    int frame;          /* frames ticked in the current scenario        */
    int capture_seq;    /* global capture serial, used in the file name */
    int captures;
    int failures;
    int duplicates;     /* captures identical to the previous one       */
    uint32_t last_top_sig;
    uint32_t last_bottom_sig;
} Driver;

static Input blank_input(void)
{
    Input in;
    memset(&in, 0, sizeof in);
    return in;
}

/* ------------------------------------------------------------------ */
/* output directory                                                    */
/* ------------------------------------------------------------------ */

static void ensure_dir(const char *path)
{
    char buf[1024];
    size_t i;
    size_t n;

    if (!path || !path[0])
        return;
    n = strlen(path);
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    memcpy(buf, path, n);
    buf[n] = '\0';

    for (i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            (void)HOST_MKDIR(buf);
            buf[i] = saved;
        }
    }
    (void)HOST_MKDIR(buf);
}

/* ------------------------------------------------------------------ */
/* one frame                                                           */
/* ------------------------------------------------------------------ */

static void run_frame(Driver *d, const Input *input, bool force_capture)
{
    Input zero;
    bool capture;
    int seq;

    if (!d || d->frame >= d->max_frames)
        return;
    if (!input) {
        memset(&zero, 0, sizeof zero);
        input = &zero;
    }

    capture = force_capture;
    if (!capture && d->capture_every > 0 && (d->frame % d->capture_every) == 0)
        capture = true;

    seq = d->capture_seq;
    if (capture)
        host_capture_request(seq);

    app_tick(&d->app, input, PREVIEW_DT);
    app_draw(&d->app);

    if (capture) {
        uint32_t top_sig;
        uint32_t bot_sig;
        int top_colors;
        int bot_colors;
        bool duplicate;

        host_capture_finish(seq);
        if (host_last_captured_frame() != seq) {
            fprintf(stderr,
                    "[capture] FAILED scenario=%s frame=%d (PNG write error)\n",
                    d->scenario, d->frame);
            d->failures++;
            d->capture_seq++;
            d->frame++;
            return;
        }

        top_sig = (uint32_t)host_screen_signature(SCREEN_TOP);
        bot_sig = (uint32_t)host_screen_signature(SCREEN_BOTTOM);
        top_colors = host_screen_distinct_colors(SCREEN_TOP);
        bot_colors = host_screen_distinct_colors(SCREEN_BOTTOM);
        duplicate = (d->captures > 0 && top_sig == d->last_top_sig &&
                     bot_sig == d->last_bottom_sig);
        if (duplicate)
            d->duplicates++;
        d->last_top_sig = top_sig;
        d->last_bottom_sig = bot_sig;

        printf("[capture] %-9s frame=%-4d %s/frame_%04d_top.png %s/frame_%04d_bottom.png"
               "  top_colors=%-5d bottom_colors=%-5d%s\n",
               d->scenario, d->frame, d->outdir, seq, d->outdir, seq,
               top_colors, bot_colors, duplicate ? "  (identical to previous)" : "");
        d->captures++;
        d->capture_seq++;
    }

    d->frame++;
}

static void idle(Driver *d, int frames)
{
    int i;
    for (i = 0; i < frames; i++)
        run_frame(d, NULL, false);
}

/* Press one button for exactly one frame (edge input). */
#define PRESS(d, field, capture_after)                       \
    do {                                                     \
        Input press_in_ = blank_input();                     \
        press_in_.field = true;                              \
        run_frame((d), &press_in_, (capture_after));         \
    } while (0)

/* Tap one bottom-screen UI rect (touchscreen "pressed this frame"). */
static bool tap_rect(Driver *d, UiRect r, bool capture_after)
{
    Input in;

    if (r.w <= 0.0f || r.h <= 0.0f) {
        fprintf(stderr, "[tap] scenario=%s frame=%d: invalid rect %.0f,%.0f %.0fx%.0f\n",
                d->scenario, d->frame, r.x, r.y, r.w, r.h);
        return false;
    }

    in = blank_input();
    in.touchDown = true;
    in.touchPressed = true;
    in.touchX = r.x + r.w * 0.5f;
    in.touchY = r.y + r.h * 0.5f;
    in.touchTop = false;
    run_frame(d, &in, capture_after);
    return true;
}

/* Move the D-pad cursor by dcol/drow grid cells (one press per cell). */
static void move_cursor(Driver *d, int dcol, int drow)
{
    int i;

    for (i = 0; i < dcol; i++)
        PRESS(d, right, false);
    for (i = 0; i < -dcol; i++)
        PRESS(d, left, false);
    for (i = 0; i < drow; i++)
        PRESS(d, down, false);
    for (i = 0; i < -drow; i++)
        PRESS(d, up, false);
}

/* Start a run from the title screen (A does it; app.c owns the transition). */
static void start_run_from_menu(Driver *d)
{
    idle(d, 2);
    PRESS(d, a, false);
    idle(d, 2);
}

/* ------------------------------------------------------------------ */
/* scenarios                                                           */
/* ------------------------------------------------------------------ */

/* Title screen over time (particles animate). */
static void scenario_menu(Driver *d)
{
    run_frame(d, NULL, true);   /* frame 0  : title screen */
    idle(d, 59);
    run_frame(d, NULL, true);   /* frame 60 : +1 s */
    idle(d, 59);
    run_frame(d, NULL, true);   /* frame 120: +2 s */
}

/* Menu -> build phase, place six towers of all three types, show pause. */
static void scenario_build(Driver *d)
{
    start_run_from_menu(d);
    run_frame(d, NULL, true);           /* empty build board + palette */

    move_cursor(d, 4, 0);               /* (4,0) */
    PRESS(d, a, false);                 /* FIRE   at (4,0) */
    PRESS(d, r, false);                 /* -> WATER */
    move_cursor(d, 0, 2);               /* (4,2) */
    PRESS(d, a, false);                 /* WATER  at (4,2) */
    PRESS(d, r, false);                 /* -> FOREST */
    move_cursor(d, 7, 0);               /* (11,2) */
    PRESS(d, a, false);                 /* FOREST at (11,2) */
    idle(d, 2);
    run_frame(d, NULL, true);           /* 3 towers, last one selected */

    PRESS(d, r, false);                 /* -> FIRE */
    move_cursor(d, 8, 0);               /* (19,2) */
    PRESS(d, a, false);                 /* FIRE   at (19,2) */
    PRESS(d, l, false);                 /* -> FOREST */
    move_cursor(d, 0, 4);               /* (19,6) */
    PRESS(d, a, false);                 /* FOREST at (19,6) */
    PRESS(d, r, false);                 /* -> FIRE */
    move_cursor(d, 0, 4);               /* (19,10) */
    PRESS(d, a, false);                 /* FIRE   at (19,10) */
    idle(d, 2);
    run_frame(d, NULL, true);           /* 6 towers, selected + range circle */

    move_cursor(d, -7, -10);            /* (12,0), clears the selection */
    idle(d, 4);
    run_frame(d, NULL, true);           /* plain build HUD, cursor only */

    PRESS(d, start, false);             /* pause */
    idle(d, 2);
    run_frame(d, NULL, true);           /* pause overlay */
    PRESS(d, start, false);             /* resume */
    idle(d, 2);
}

/* Build a few towers, send wave 1, capture activity (enemies + beams).
 * Towers go on board row 2 (just below the top path leg, still in range) so
 * they are visible during PHASE_BUILD - row 0 is covered by the build-phase
 * hint strip in render.c (reported to engine-3ds). */
static void scenario_wave(Driver *d)
{
    int i;
    int b;

    start_run_from_menu(d);

    move_cursor(d, 3, 2);               /* (3,2) */
    PRESS(d, a, false);                 /* FIRE */
    PRESS(d, r, false);
    move_cursor(d, 5, 0);               /* (8,2) */
    PRESS(d, a, false);                 /* WATER */
    PRESS(d, r, false);
    move_cursor(d, 7, 0);               /* (15,2) */
    PRESS(d, a, false);                 /* FOREST */
    PRESS(d, r, false);
    move_cursor(d, 7, 0);               /* (22,2) */
    PRESS(d, a, false);                 /* FIRE */
    idle(d, 2);
    run_frame(d, NULL, true);           /* build phase, 4 towers ready */

    /* Start wave 1 through the real bottom-screen NEXT button hit box. */
    if (!tap_rect(d, ui_next_wave_rect(), true)) {
        fprintf(stderr, "[wave] ui_next_wave_rect() is empty; cannot start wave\n");
        return;
    }

    /* Run the wave.  Capture while beams are on screen so the screenshots
     * actually show combat, plus a slow heartbeat of enemy-only frames.  Stop
     * as soon as the wave resolves (phase drops back to PHASE_BUILD) so the
     * capture set stays small and every file is a distinct game state. */
    for (i = 0; i < 1500 && d->frame < d->max_frames; i++) {
        bool capture = false;

        if (d->app.game.phase != PHASE_WAVE)
            break;
        for (b = 0; b < MAX_BEAMS; b++) {
            if (d->app.game.beams[b].active) {
                if ((d->frame % 12) == 0)
                    capture = true;
                break;
            }
        }
        if (!capture && d->app.game.enemiesAlive > 0 && (d->frame % 90) == 0)
            capture = true;
        run_frame(d, NULL, capture);
    }
    idle(d, 2);
    run_frame(d, NULL, true);           /* post-wave state */
}

/* Place a tower, show the selected/upgrade panel, upgrade it, then sell it.
 * The tower sits on row 2 so the top screen shows it (and its range circle)
 * without the build-phase hint strip covering it. */
static void scenario_selected(Driver *d)
{
    start_run_from_menu(d);

    move_cursor(d, 4, 2);               /* (4,2) */
    PRESS(d, a, false);                 /* place FIRE (auto-selected) */
    idle(d, 2);
    run_frame(d, NULL, true);           /* selected panel + range circle */

    tap_rect(d, ui_upgrade_rect(), false);
    idle(d, 2);
    run_frame(d, NULL, true);           /* after upgrade: LV.2 panel */

    tap_rect(d, ui_sell_rect(), false);
    idle(d, 2);
    run_frame(d, NULL, true);           /* after sell: empty cell panel */
}

/* Force the game-over overlay (no way to lose 20 lives in a 30 s script). */
static void scenario_gameover(Driver *d)
{
    start_run_from_menu(d);

    move_cursor(d, 4, 0);
    PRESS(d, a, false);                 /* FIRE at (4,0) */
    PRESS(d, r, false);
    move_cursor(d, 5, 0);               /* (9,0) */
    PRESS(d, a, false);                 /* WATER at (9,0) */
    idle(d, 3);

    d->app.game.phase = PHASE_GAMEOVER;
    d->app.game.lives = 0;
    d->app.game.wave = 4;
    d->app.game.kills = 17;
    d->app.game.enemiesAlive = 0;
    idle(d, 2);
    run_frame(d, NULL, true);           /* game-over overlay */
}

/* Force the victory overlay (wave == totalWaves). */
static void scenario_victory(Driver *d)
{
    start_run_from_menu(d);

    move_cursor(d, 4, 0);
    PRESS(d, a, false);                 /* FIRE at (4,0) */
    PRESS(d, r, false);
    move_cursor(d, 5, 0);               /* (9,0) */
    PRESS(d, a, false);                 /* WATER at (9,0) */
    idle(d, 3);

    d->app.game.phase = PHASE_VICTORY;
    d->app.game.wave = d->app.game.totalWaves;
    d->app.game.kills = 143;
    d->app.game.lives = 12;
    d->app.game.enemiesAlive = 0;
    idle(d, 2);
    run_frame(d, NULL, true);           /* victory overlay */
}

/* ------------------------------------------------------------------ */
/* scenario dispatch                                                   */
/* ------------------------------------------------------------------ */

typedef void (*ScenarioFn)(Driver *d);

typedef struct {
    const char *name;
    ScenarioFn fn;
} ScenarioEntry;

static const ScenarioEntry kScenarios[] = {
    { "menu",     scenario_menu     },
    { "build",    scenario_build    },
    { "wave",     scenario_wave     },
    { "selected", scenario_selected },
    { "gameover", scenario_gameover },
    { "victory",  scenario_victory  }
};

#define SCENARIO_COUNT ((int)(sizeof kScenarios / sizeof kScenarios[0]))

/* Shared capture serial: PNG file names stay unique across "all" scenarios. */
static int g_capture_seq = 0;

static int run_scenario(const char *name, const char *outdir, int seed,
                        int capture_every, int max_frames)
{
    Driver d;
    int i;
    int found = 0;
    int result = 0;

    memset(&d, 0, sizeof d);
    d.scenario = name;
    d.outdir = outdir;
    d.max_frames = max_frames;
    d.capture_every = capture_every;
    d.capture_seq = g_capture_seq;
    d.last_top_sig = 0xFFFFFFFFu;
    d.last_bottom_sig = 0xFFFFFFFFu;

    gfx_init();
    host_capture_configure(outdir, 1);
    host_capture_set_defer(0);   /* app_draw() ends with exactly one gfx_present() */

    app_init(&d.app);
    d.app.seed = (uint32_t)seed;
    audio_init();

    for (i = 0; i < SCENARIO_COUNT; i++) {
        if (strcmp(kScenarios[i].name, name) == 0) {
            found = 1;
            printf("\n=== scenario: %s (seed=%u, max_frames=%d) ===\n",
                   name, (unsigned)d.app.seed, max_frames);
            kScenarios[i].fn(&d);
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "unknown scenario '%s' (try --help)\n", name);
        return -1;
    }

    audio_exit();
    gfx_shutdown();

    printf("--- scenario %s: %d capture(s), %d failure(s), %d identical-to-previous,",
           name, d.captures, d.failures, d.duplicates);
    printf(" %d frame(s) simulated\n", d.frame);
    if (d.captures == 0) {
        fprintf(stderr, "!!! scenario %s produced no screenshots\n", name);
        result = 1;
    }
    if (d.failures > 0)
        result = 1;

    g_capture_seq = d.capture_seq;
    return result;
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    printf("Elemental Defense - PC preview screenshot harness\n"
           "\n"
           "Usage: %s [options]\n"
           "\n"
           "  --scenario NAME   menu | build | wave | selected | gameover |\n"
           "                    victory | all            (default: all)\n"
           "  --frames N        frame cap per scenario        (default: %d)\n"
           "  --capture-every K also capture every K-th frame (default: 0 = off)\n"
           "  --out DIR         screenshot directory          (default: out)\n"
           "  --seed S          run seed                      (default: %u)\n"
           "  --help            this text\n"
           "\n"
           "Output: DIR/frame_<N>_top.png (400x240) and DIR/frame_<N>_bottom.png\n"
           "(320x240), N counting captures across the whole run.\n",
           argv0, DEFAULT_FRAMES, (unsigned)DEFAULT_SEED);
}

int main(int argc, char **argv)
{
    const char *scenario = "all";
    const char *outdir = "out";
    int frames = DEFAULT_FRAMES;
    int capture_every = 0;
    unsigned seed = DEFAULT_SEED;
    int i;
    int exit_code = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--capture-every") == 0 && i + 1 < argc) {
            capture_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option '%s' (try --help)\n", argv[i]);
            return 2;
        }
    }

    if (frames <= 0)
        frames = DEFAULT_FRAMES;
    if (capture_every < 0)
        capture_every = 0;

    ensure_dir(outdir);

    printf("Elemental Defense preview harness\n");
    printf("  outdir='%s' scenario='%s' seed=%u frames=%d capture_every=%d\n",
           outdir, scenario, seed, frames, capture_every);

    if (strcmp(scenario, "all") == 0) {
        int s;
        for (s = 0; s < SCENARIO_COUNT && exit_code == 0; s++) {
            if (run_scenario(kScenarios[s].name, outdir, (int)seed,
                             capture_every, frames) != 0)
                exit_code = 1;
        }
    } else {
        if (run_scenario(scenario, outdir, (int)seed, capture_every, frames) < 0)
            return 2;
    }

    printf("\npreview harness finished: %s\n", exit_code == 0 ? "OK" : "FAILURES");
    return exit_code;
}
