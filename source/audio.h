/*
 * Elemental Defense - audio
 *
 * Pure-C, platform independent interface.  Right now the implementation in
 * source/audio.c is a structured no-op: it records the game event bits so a
 * later milestone (or host test) can verify which sounds *should* play, but no
 * audio hardware is touched.  Keeping this file free of <3ds.h> lets the PC
 * preview harness link against it unchanged.
 *
 * When ndsp support lands it is added inside source/audio.c (or a future
 * source/plat_3ds_audio.c) - the game and UI code will not need to change.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void audio_init(void);
void audio_exit(void);

/* Dispatches the GAME_EVT_* bits the core accumulated this frame.  The app
 * layer calls this once per frame and then clears Game.events. */
void audio_play_event(uint32_t game_events);

/* Debug / host-preview helpers: the OR of every event bit sent this run and
 * the number of audio_play_event() calls.  Both are harmless no-ops on 3DS. */
uint32_t audio_event_history(void);
uint32_t audio_event_count(void);

/* Muting is a stub for now; it is already wired through app_tick so adding
 * real voices later only touches audio.c. */
void audio_set_muted(bool muted);
bool audio_is_muted(void);

#endif /* AUDIO_H */
