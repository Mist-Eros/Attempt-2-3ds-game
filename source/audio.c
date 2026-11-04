/*
 * Elemental Defense - audio stub
 *
 * Deliberately contains no platform code.  Sound is a later milestone: this
 * file exists so the app layer can already dispatch GAME_EVT_* feedback and so
 * the host preview build links without an audio backend.  A real ndsp mixer
 * would slot in here behind exactly the same four functions.
 *
 * Sound mapping the mixer will eventually use (from include/game.h):
 *   GAME_EVT_BUILD      -> thunk, low
 *   GAME_EVT_SELL       -> rising chirp
 *   GAME_EVT_UPGRADE    -> sparkle
 *   GAME_EVT_LASER      -> per-element zap (rate limited)
 *   GAME_EVT_ENEMY_DIE  -> pop
 *   GAME_EVT_WAVE_START -> fanfare
 *   GAME_EVT_GAMEOVER   -> descending tone
 *   GAME_EVT_VICTORY    -> victory jingle
 *   GAME_EVT_LEAK       -> low alarm
 */
#include <stdbool.h>
#include <stdint.h>

#include "audio.h"

static bool s_initialized = false;
static bool s_muted = false;
static uint32_t s_history = 0;
static uint32_t s_calls = 0;

void audio_init(void)
{
    s_initialized = true;
    s_muted = false;
    s_history = 0;
    s_calls = 0;
}

void audio_exit(void)
{
    s_initialized = false;
}

void audio_play_event(uint32_t game_events)
{
    if (!s_initialized || s_muted || game_events == 0)
        return;

    /* Later: for each bit in game_events pick a loaded wav and queue a voice
     * with ndspChnWaveBufAdd(), with a small cooldown for GAME_EVT_LASER so
     * a wave of 20 towers does not turn into a wall of noise. */
    s_history |= game_events;
    s_calls++;
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
