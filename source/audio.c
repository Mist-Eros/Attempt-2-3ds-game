/*
 * Elemental Defense - portable audio fallback
 *
 * Deliberately contains no platform code: this is what the host preview
 * harness and the unit-test builds link against.  It logs the GAME_EVT_* bits
 * the app dispatches and remembers the requested music track, so the preview
 * tooling can verify the wiring without audio hardware.
 *
 * The real mixer lives in source/audio_3ds.c (libctru ndsp) and is only
 * compiled into the 3DS build.
 *
 * Sound mapping (kept in sync with audio_3ds.c):
 *   GAME_EVT_BUILD      -> build thunk
 *   GAME_EVT_SELL       -> sell chirp
 *   GAME_EVT_UPGRADE    -> one of six upgrade sparkles
 *   GAME_EVT_LASER      -> per-shot zap (rate limited)
 *   GAME_EVT_ENEMY_DIE  -> pop
 *   GAME_EVT_WAVE_START -> wave fanfare
 *   GAME_EVT_GAMEOVER   -> descending game-over sting
 *   GAME_EVT_VICTORY    -> victory sting
 *   GAME_EVT_LEAK       -> leak alarm
 */
#include <stdbool.h>
#include <stdint.h>

#include "audio.h"

/*
 * The 3DS build compiles source/audio_3ds.c instead of this fallback (look for
 * -D__3DS__ in the Makefile).  Keeping the two implementations in separate
 * files means the host preview harness never needs to link libctru.
 */
#ifndef __3DS__

static bool s_initialized = false;
static bool s_muted = false;
static uint32_t s_history = 0;
static uint32_t s_calls = 0;
static AudioMusic s_music = AUDIO_MUSIC_NONE;

void audio_init(void)
{
    s_initialized = true;
    s_muted = false;
    s_history = 0;
    s_calls = 0;
    s_music = AUDIO_MUSIC_NONE;
}

void audio_exit(void)
{
    s_initialized = false;
    s_music = AUDIO_MUSIC_NONE;
}

void audio_play_event(uint32_t game_events)
{
    if (!s_initialized || s_muted || game_events == 0)
        return;

    s_history |= game_events;
    s_calls++;
}

void audio_set_music(AudioMusic track)
{
    if (!s_initialized)
        return;
    if (track != AUDIO_MUSIC_MENU && track != AUDIO_MUSIC_GAME)
        track = AUDIO_MUSIC_NONE;
    s_music = track;
}

AudioMusic audio_current_music(void)
{
    return s_music;
}

uint32_t audio_event_history(void)
{
    return s_history;
}

uint32_t audio_event_count(void)
{
    return s_calls;
}

void audio_set_muted(bool muted)
{
    s_muted = muted;
}

bool audio_is_muted(void)
{
    return s_muted;
}

#endif /* !__3DS__ */
