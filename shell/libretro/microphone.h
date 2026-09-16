#pragma once

/* The Dreamcast microphone's frontend half.
 *
 * Everything here is called only from the thread that calls retro_run, which is
 * where every libretro microphone call must happen. The emulation thread
 * reaches this through StartAudioRecording / RecordAudio / StopAudioRecording
 * (declared in core/audio/audiostream.h), which move atomics and drain a
 * lock-free queue -- they never call the frontend, take a lock, or wait.
 */

void mic_probe();     /* retro_init: ask for the interface, size the queue */
void mic_pump();      /* retro_run: open, read, decimate, enqueue */
void mic_flush();     /* retro_reset / retro_unserialize: drop buffered audio */
void mic_shutdown();  /* retro_unload_game / retro_deinit: force the handle shut */
