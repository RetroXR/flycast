#pragma once

/* The libretro microphone interface, backported.
 *
 * Unlike ControllerAudioInterface.h beside it, none of this is ours: every
 * declaration below is copied from upstream libretro.h. The copy of
 * libretro-common vendored in core/deps stops at environment call 74 and
 * predates the microphone, so a core built against it cannot see the interface
 * at all.
 *
 * Guarded so the file becomes empty the day core/deps/libretro-common is
 * updated -- nothing is redefined and no call site has to change.
 */

#include <stddef.h>
#include <stdint.h>

#include <libretro.h>

#ifndef RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE

#define RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE (75 | RETRO_ENVIRONMENT_EXPERIMENTAL)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct retro_microphone retro_microphone_t;

typedef struct retro_microphone_params
{
   unsigned rate;
} retro_microphone_params_t;

typedef retro_microphone_t *(RETRO_CALLCONV *retro_open_mic_t)(const retro_microphone_params_t *params);
typedef void (RETRO_CALLCONV *retro_close_mic_t)(retro_microphone_t *microphone);
typedef bool (RETRO_CALLCONV *retro_get_mic_params_t)(const retro_microphone_t *microphone, retro_microphone_params_t *params);
typedef bool (RETRO_CALLCONV *retro_set_mic_state_t)(retro_microphone_t *microphone, bool state);
typedef bool (RETRO_CALLCONV *retro_get_mic_state_t)(const retro_microphone_t *microphone);
typedef int (RETRO_CALLCONV *retro_read_mic_t)(retro_microphone_t *microphone, int16_t *samples, size_t num_samples);

#define RETRO_MICROPHONE_INTERFACE_VERSION 1

struct retro_microphone_interface
{
   /* Set by the core to the version it wants; a frontend that answers writes
    * back the version it filled in. A zero here is refused. */
   unsigned interface_version;

   retro_open_mic_t open_mic;
   retro_close_mic_t close_mic;
   retro_get_mic_params_t get_params;
   retro_set_mic_state_t set_mic_state;
   retro_get_mic_state_t get_mic_state;
   retro_read_mic_t read_mic;
};

#ifdef __cplusplus
}
#endif

#endif /* RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE */
