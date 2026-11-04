/*
 * Elemental Defense - audio
 *
 * Pure-C, platform independent interface.  The implementation in
 * source/audio.c is the portable fallback: it records the game event bits so
 * the host preview harness (and tests) can verify which sounds *should* play,
 * but it touches no audio hardware.  On the 3DS build, source/audio_3ds.c
 * implements the same functions with libctru's ndsp mixer playing the raw PCM
 * data linked in from data/.
 *
 * Keeping this header free of <3ds.h> lets the PC preview harness link against
 * it unchanged.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* Which looping music track should be playing (the 3DS backend crossfades by
 * swapping the looped wave buffers; the portable stub just remembers it). */
typedef enum {
    AUDIO_MUSIC_NONE = 0,
    AUDIO_MUSIC_MENU = 1,
    AUDIO_MUSIC_GAME = 2
} AudioMusic;

void audio_init(void);
void audio_exit(void);

/* Dispatches the GAME_EVT_* bits the core accumulated this frame.  The app
 * layer calls this once per frame and then clears Game.events. */
void audio_play_event(uint32_t game_events);

/* Called once per frame from the platform main loop.  On the 3DS this keeps
 * the streamed music ring buffers topped up; the portable fallback does
 * nothing. */
void audio_update(void);

/* Idempotent: switching to the track that is already playing is a no-op.
 * AUDIO_MUSIC_NONE stops whatever is playing. */
void audio_set_music(AudioMusic track);
AudioMusic audio_current_music(void);

/* Debug / host-preview helpers: the OR of every event bit sent this run and
 * the number of audio_play_event() calls.  Both are harmless no-ops on 3DS. */
uint32_t audio_event_history(void);
uint32_t audio_event_count(void);

void audio_set_muted(bool muted);
bool audio_is_muted(void);

#endif /* AUDIO_H */
