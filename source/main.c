/*
 * Elemental Defense - Nintendo 3DS entry point and main loop
 *
 * Owns everything hardware specific that is not graphics:
 *   - aptMainLoop() / HOME button handling
 *   - hid input polling, including D-pad auto-repeat and the touchscreen edge
 *   - frame delta time from the ARM11 tick counter, clamped so a hitch (or the
 *     process being suspended) cannot teleport enemies across the map
 *
 * All scene logic lives in app_tick() and all drawing in app_draw(), both in
 * portable files, so this file stays small.
 *
 * Controls (see source/app.h for the full scheme): touch on the bottom screen,
 * D-pad cursor + A to build on the top screen, X upgrade, Y sell, L/R tower
 * type, Start pause, Select speed.  The HOME button is the only way to leave
 * the app - Start is used for pause, so it cannot double as "quit".
 */
#include <3ds.h>

#include <stdint.h>
#include <string.h>

#include "app.h"
#include "audio.h"
#include "gfx.h"

/* The App is too large for the 3DS main thread's default stack (the Game state
 * alone is tens of kilobytes with fixed-size entity pools), so keep it static. */
static App s_app;

int main(void)
{
    static const u32 kDpadKeys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
    bool *dpadOut[4];
    float dpadHold[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    touchPosition touch;
    Input input;
    bool prevTouch = false;
    u64 lastTick;
    int i;

    gfx_init();
    audio_init();

    app_init(&s_app);
    s_app.seed = (uint32_t)(svcGetSystemTick() ^ osGetTime());
    if (s_app.seed == 0)
        s_app.seed = 0x5EED1234u;

    dpadOut[0] = &input.up;
    dpadOut[1] = &input.down;
    dpadOut[2] = &input.left;
    dpadOut[3] = &input.right;

    lastTick = svcGetSystemTick();

    while (aptMainLoop()) {
        u32 down;
        u32 held;
        u64 now;
        float dt;

        hidScanInput();
        down = hidKeysDown();
        held = hidKeysHeld();

        memset(&input, 0, sizeof(input));
        input.a = (down & KEY_A) != 0;
        input.b = (down & KEY_B) != 0;
        input.x = (down & KEY_X) != 0;
        input.y = (down & KEY_Y) != 0;
        input.l = (down & KEY_L) != 0;
        input.r = (down & KEY_R) != 0;
        input.start = (down & KEY_START) != 0;
        input.select = (down & KEY_SELECT) != 0;

        /* delta time in seconds from the 268 MHz ARM11 tick counter */
        now = svcGetSystemTick();
        dt = (float)((double)(now - lastTick) / CPU_TICKS_PER_MSEC / 1000.0);
        lastTick = now;
        if (dt < 0.0f)
            dt = 0.0f;
        if (dt > 0.1f)
            dt = 0.1f;

        /* D-pad: the app layer only consumes "pressed this frame" edges, so
         * synthesise auto-repeat here (0.32 s delay, then every 0.10 s). */
        for (i = 0; i < 4; i++) {
            if ((down & kDpadKeys[i]) != 0) {
                *dpadOut[i] = true;
                dpadHold[i] = 0.32f;
            } else if ((held & kDpadKeys[i]) != 0) {
                dpadHold[i] -= dt;
                if (dpadHold[i] <= 0.0f) {
                    *dpadOut[i] = true;
                    dpadHold[i] = 0.10f;
                }
            } else {
                dpadHold[i] = 0.0f;
            }
        }

        /* Touchscreen: hid reports DOWN on the first frame; we derive the
         * pressed edge from our own previous state so a held finger does not
         * repeatedly click buttons. */
        if ((held & KEY_TOUCH) != 0) {
            hidTouchRead(&touch);
            input.touchDown = true;
            input.touchX = (float)touch.px;
            input.touchY = (float)touch.py;
            input.touchPressed = !prevTouch;
            prevTouch = true;
        } else {
            prevTouch = false;
        }

        app_tick(&s_app, &input, dt);
        app_draw(&s_app);
        audio_update();
    }

    audio_exit();
    gfx_shutdown();
    return 0;
}
