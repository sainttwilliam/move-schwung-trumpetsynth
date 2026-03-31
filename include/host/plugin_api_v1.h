/* plugin_api_v1.h
 * Vendored from https://github.com/charlesvestal/move-everything
 *
 * Host API and plugin API structs for Schwung (Move Everything) sound
 * generator plugins.  Keep in sync with the upstream header when upgrading
 * the Schwung host.
 */
#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Host → plugin modulation callbacks
 * ------------------------------------------------------------------------ */
typedef void (*move_mod_emit_value_fn)(void *ctx, const char *source, float value);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source);

/* ---------------------------------------------------------------------------
 * host_api_v1_t
 *
 * Passed to move_plugin_init_v2().  Store the pointer if you need to access
 * the audio input buffer (mapped_memory + audio_in_offset) during render.
 * ------------------------------------------------------------------------ */
typedef struct host_api_v1 {
    uint32_t api_version;
    int      sample_rate;       /* always 44100 */
    int      frames_per_block;  /* always 128   */

    /* Shared memory region.  Audio I/O lives here:
     *   audio input:  (int16_t *)(mapped_memory + audio_in_offset)
     *   audio output: written via out_interleaved_lr in render_block
     */
    uint8_t *mapped_memory;
    int      audio_out_offset;
    int      audio_in_offset;

    /* Logging */
    void (*log)(const char *msg);

    /* MIDI send */
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);

    /* Clock */
    int   (*get_clock_status)(void);
    float (*get_bpm)(void);

    /* Modulation */
    move_mod_emit_value_fn   mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
} host_api_v1_t;

/* ---------------------------------------------------------------------------
 * plugin_api_v2_t
 *
 * Returned by move_plugin_init_v2().  api_version must be 2.
 * ------------------------------------------------------------------------ */
typedef struct plugin_api_v2 {
    uint32_t api_version;   /* set to 2 */

    /* Instance lifecycle */
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void  (*destroy_instance)(void *instance);

    /* MIDI — source 0 = internal Move pads, source 1 = external USB MIDI */
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);

    /* Parameter I/O — keys and values are null-terminated strings */
    void (*set_param)(void *instance, const char *key, const char *val);
    int  (*get_param)(void *instance, const char *key, char *buf, int buf_len);

    /* Error reporting — return 0 if no error, else write message and return len */
    int  (*get_error)(void *instance, char *buf, int buf_len);

    /* Audio render — write frames stereo-interleaved int16 samples to out */
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* Entry point — symbol looked up via dlsym() by the Schwung host */
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
