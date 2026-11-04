/*
 * Elemental Defense - platform independent application / scene layer
 *
 * This is the interface the 3DS main loop (source/main.c) and the PC preview
 * harness (tools/) both drive.  Everything declared here is plain C99 and
 * includes nothing but <stdbool.h>, <stdint.h>, game.h and gfx.h, so it can be
 * compiled and run on a normal PC against the software gfx backend.
 *
 * ---------------------------------------------------------------------------
 * Frame model
 * ---------------------------------------------------------------------------
 *   app_init(&app);                       once
 *   app_tick(&app, &input, dt);           once per frame, dt in seconds
 *   app_draw(&app);                       once per frame, draws top + bottom
 *   app_new_run(&app, seed);              optional, starts a fresh run
 *
 * The host preview harness can drive the game by filling in Input structs and
 * calling app_tick/app_draw exactly like main.c does.  All input booleans are
 * "pressed this frame" edges; the 3DS main loop synthesises D-pad repeat
 * presses so app_tick only ever has to look at edges.
 *
 * ---------------------------------------------------------------------------
 * Controls (bottom screen is the only touch surface)
 * ---------------------------------------------------------------------------
 *   Menu      : A or Start or tap START       -> new run
 *   D-pad     : move the build cursor on the battlefield (top screen)
 *   A         : select tower under cursor / build selected type on empty cell
 *   B         : clear tower selection
 *   X         : upgrade selected tower (or the tower under the cursor)
 *   Y         : sell selected tower (or the tower under the cursor)
 *   L / R     : previous / next tower type
 *   Start     : pause / resume
 *   Select    : cycle game speed 1x -> 2x -> 4x
 *   Touch     : all bottom-screen buttons (tower palette, upgrade/sell,
 *               next wave, speed, pause, retry, menu)
 *   Game over : A / tap PLAY AGAIN -> new run, B / MAIN MENU -> title screen
 *
 * Note that the top screen is not touch sensitive on the 3DS.  Placement is
 * therefore always "D-pad cursor + A"; the host preview harness may also set
 * Input.touchTop to synthesise a top-screen click for debugging screenshots.
 */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

#include "game.h"

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    bool touchDown;         /* the touchscreen is currently being held     */
    bool touchPressed;      /* the touchscreen went down this frame        */
    float touchX, touchY;   /* touch position in bottom screen pixels      */
                            /* (0..319 / 0..239); valid when touchDown     */

    /* buttons, true only on the frame they were pressed */
    bool up, down, left, right;                 /* D-pad */
    bool a, b, x, y, l, r, start, select;

    /* Host-preview only: when true the touch sample above refers to the TOP
     * screen (0..399 / 0..239) and is handled as a debug board click instead
     * of a UI button press.  Always false on real hardware. */
    bool touchTop;
} Input;

/* ------------------------------------------------------------------------ */
/* App                                                                       */
/* ------------------------------------------------------------------------ */

#define MENU_PARTICLE_COUNT 64

typedef struct {
    float x, y;         /* pixel position on the top screen */
    float vx, vy;       /* pixels per second */
    float size;         /* square size in pixels */
    float life;         /* seconds left */
    float maxLife;
    float r, g, b;
} MenuParticle;

typedef struct {
    Game game;          /* portable simulation state (see game.h) */

    float time;         /* seconds since app_init(), unscaled by game speed */
    float menuTime;     /* animation clock for menu / UI effects */
    uint32_t frames;    /* frames ticked, useful for deterministic previews */

    /* Seed used by the *next* run started from the menu (or by app_new_run).
     * main.c overwrites this with RTC/tick entropy; the preview harness can
     * leave it at the deterministic default in app_init(). */
    uint32_t seed;

    MenuParticle particles[MENU_PARTICLE_COUNT];
    uint32_t particleRng;   /* xorshift state for menu particle respawns */

    /* Wave progress bookkeeping for the bottom-screen bar.  The core does not
     * expose the wave's spawn count, so app_tick remembers the peak number of
     * enemies (alive + queued) seen during the current wave; ui.c uses it as
     * the denominator.  Reset whenever Game.wave changes. */
    int waveTrackedWave;    /* wave number waveEnemyTotal belongs to */
    int waveEnemyTotal;     /* peak alive+queued enemies for that wave  */

    /* Last Game.phase the music track was set for (-1 = not set yet), so
     * app_tick only calls audio_set_music() on actual transitions. */
    int lastMusicPhase;
} App;

/* ------------------------------------------------------------------------ */
/* API                                                                       */
/* ------------------------------------------------------------------------ */

void app_init(App *app);
void app_tick(App *app, const Input *input, float dt);
void app_draw(App *app);    /* begins a frame, draws BOTH screens, presents */

/* Start a brand new run with the given seed (also callable from the menu).
 * Resets the UI/menu animation state; the seed field is advanced afterwards
 * so consecutive runs differ. */
void app_new_run(App *app, uint32_t seed);

/* True while the simulation is frozen by the pause button.  Convenience for
 * host tooling / overlays; mirrors app->game.paused. */
bool app_is_paused(const App *app);

#endif /* APP_H */
