/*
 * Elemental Defense - bottom screen (touch) UI
 *
 * The bottom screen is the only touch surface on a 3DS, so every interactive
 * control lives here.  ui.c owns the layout and drawing; ui_hit_test() maps a
 * touch position to a UiEvent that app.c then turns into a game action.
 *
 * All coordinates are bottom-screen pixels: 320x240, origin top-left.
 * The rectangles are exposed (ui_*_rect) so the PC preview harness can click
 * precise buttons without duplicating the layout.
 *
 * Screen layouts (320x240):
 *   PHASE_MENU : title, three element icons, big START button
 *   BUILD/WAVE : 32 px header (gold | lives | wave | speed badge | pause badge)
 *                52 px tower palette, 66 px info/upgrade panel,
 *                77 px NEXT WAVE + incoming preview (build)
 *                or wave progress + stats            (wave)
 *   paused     : dimmed play UI + RESUME / MAIN MENU
 *   result     : PLAY AGAIN / MAIN MENU
 *
 * The header speed/pause badges are the single touch control for those two
 * actions in every play phase; the wave panel shows progress and stats only.
 *
 * Maze building: the info panel labels the cursor cell "ROAD" when it lies on
 * the live BFS path and says whether building there is legal; the core owns
 * the actual rerouting and the top screen redraws the road from g->path.
 */
#ifndef UI_H
#define UI_H

#include "app.h"
#include "gfx.h"

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_START,         /* menu: big START button                    */
    UI_EVENT_SELECT_TYPE,   /* index = 0..2, tower palette               */
    UI_EVENT_UPGRADE,       /* upgrade selected / cursor tower           */
    UI_EVENT_SELL,          /* sell selected / cursor tower              */
    UI_EVENT_NEXT_WAVE,     /* build phase: start the next wave          */
    UI_EVENT_CYCLE_SPEED,   /* cycle 1x / 2x / 4x                        */
    UI_EVENT_TOGGLE_PAUSE,  /* pause / resume                            */
    UI_EVENT_RESUME,        /* paused overlay: resume                    */
    UI_EVENT_RETRY,         /* result screen: start a new run            */
    UI_EVENT_MENU           /* result / pause / paused: go to title      */
} UiEventType;

typedef struct {
    UiEventType type;
    int index;              /* palette index for UI_EVENT_SELECT_TYPE   */
} UiEvent;

typedef struct {
    float x, y, w, h;
} UiRect;

/* ------------------------------------------------------------------------ */
/* API                                                                       */
/* ------------------------------------------------------------------------ */

/* Renders the whole bottom screen for the current phase (including paused and
 * result variants).  app_draw calls this after the top screen. */
void ui_draw(const App *app);

/* Hit-tests a bottom-screen touch.  Returns true and fills *ev when a button
 * was hit; returns false when the touch landed on empty space.  The event is
 * *not* applied here, app.c owns the game actions. */
bool ui_hit_test(const App *app, float x, float y, UiEvent *ev);

bool ui_rect_contains(UiRect r, float x, float y);

/* ---- explicit hit boxes, shared with the preview harness ---- */
UiRect ui_start_rect(void);             /* PHASE_MENU: START               */
UiRect ui_tower_button_rect(int type);  /* 0..2                            */
UiRect ui_upgrade_rect(void);           /* selected / cursor tower panel   */
UiRect ui_sell_rect(void);
UiRect ui_next_wave_rect(void);         /* PHASE_BUILD                     */
UiRect ui_speed_rect(void);             /* header speed badge (all play)   */
UiRect ui_pause_rect(void);             /* header pause badge              */
UiRect ui_wave_speed_rect(void);        /* alias of ui_speed_rect          */
UiRect ui_wave_pause_rect(void);        /* alias of ui_pause_rect          */
UiRect ui_resume_rect(void);            /* paused overlay                  */
UiRect ui_pause_menu_rect(void);        /* paused overlay: main menu       */
UiRect ui_retry_rect(void);             /* result screen: play again       */
UiRect ui_result_menu_rect(void);       /* result screen: main menu        */

#endif /* UI_H */
