/* trumpet_synth.c
 *
 * Monophonic pitch-tracking synthesizer for the Move Everything / Schwung framework.
 *
 * Signal path:
 *   Audio input (trumpet via Roland Silent Brass)
 *     → aubio pitch detection
 *     → Oscillator (sine / triangle / saw / square)
 *     → Airwindows Capacitor low-pass filter
 *     → ADSR envelope
 *     → Envelope follower modulation
 *     → LFO (targets: pitch, filter, amp)
 *     → Stereo output
 *
 * Plugin API: Schwung plugin_api_v2_t
 * Audio format: stereo interleaved int16, 44100 Hz, 128 frames/block
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* TODO: include aubio header once aubio is available on the target */
/* #include <aubio/aubio.h> */

/* Host API header provided by the Schwung build environment */
#include "host/plugin_api_v1.h"

#define SAMPLE_RATE  44100.0f
#define BLOCK_SIZE   128

/* -------------------------------------------------------------------------
 * Parameter state
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Input conditioning */
    float input_gain;
    float pitch_confidence;   /* minimum aubio confidence threshold */
    float pitch_smooth;       /* one-pole smoothing coefficient for detected pitch */

    /* Oscillator */
    int   osc_wave;           /* 0=sine 1=tri 2=saw 3=square */
    float osc_detune;         /* cents, -100..+100 */
    float osc_level;

    /* Airwindows Capacitor filter */
    float filter_cutoff;      /* Hz */
    float filter_reso;        /* 0..1 */

    /* ADSR */
    float env_attack;
    float env_decay;
    float env_sustain;
    float env_release;

    /* Envelope follower */
    float follow_attack;
    float follow_release;
    float follow_amount;

    /* LFO */
    float lfo_rate;
    float lfo_depth;
    int   lfo_target;         /* 0=pitch 1=filter 2=amp */

    /* Output */
    float volume;
} Params;

/* -------------------------------------------------------------------------
 * DSP state
 * ---------------------------------------------------------------------- */
typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} EnvStage;

/* Module-level host pointer — stored at move_plugin_init_v2() time.
 * Used in render_block to access the audio input buffer:
 *   (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset)
 */
static const host_api_v1_t *g_host = NULL;

typedef struct {
    /* aubio objects — TODO: allocate once aubio is integrated */
    /* aubio_pitch_t *pitch_obj; */
    /* fvec_t        *pitch_buf; */
    /* fvec_t        *pitch_out; */

    /* Pitch tracking */
    float detected_hz;        /* raw aubio output */
    float smoothed_hz;        /* after one-pole smoother */
    int   gate;               /* 1 = pitch detected above confidence */

    /* Oscillator */
    float osc_phase;          /* 0..1 */

    /* Airwindows Capacitor internal state */
    float cap_low[2];         /* lowpass state, L + R */
    float cap_band[2];        /* bandpass state, L + R */

    /* ADSR */
    EnvStage env_stage;
    float    env_value;

    /* Envelope follower */
    float follow_env;

    /* LFO */
    float lfo_phase;          /* 0..1 */

    /* Parameters (live copy, updated via set_param) */
    Params p;
} TrumpetSynth;

/* -------------------------------------------------------------------------
 * Helper: parse float/int from string safely
 * ---------------------------------------------------------------------- */
static float parse_float(const char *val, float fallback) {
    if (!val || val[0] == '\0') return fallback;
    char *end;
    float f = strtof(val, &end);
    return (end != val) ? f : fallback;
}

static int parse_wave(const char *val) {
    if (!val) return 2; /* default saw */
    if (strcmp(val, "sine")     == 0) return 0;
    if (strcmp(val, "triangle") == 0) return 1;
    if (strcmp(val, "saw")      == 0) return 2;
    if (strcmp(val, "square")   == 0) return 3;
    return 2;
}

static int parse_lfo_target(const char *val) {
    if (!val) return 0;
    if (strcmp(val, "filter") == 0) return 1;
    if (strcmp(val, "amp")    == 0) return 2;
    return 0; /* pitch */
}

/* -------------------------------------------------------------------------
 * Default parameters
 * ---------------------------------------------------------------------- */
static void params_init_defaults(Params *p) {
    p->input_gain       = 1.0f;
    p->pitch_confidence = 0.8f;
    p->pitch_smooth     = 0.1f;

    p->osc_wave         = 2;      /* saw */
    p->osc_detune       = 0.0f;
    p->osc_level        = 0.8f;

    p->filter_cutoff    = 4000.0f;
    p->filter_reso      = 0.3f;

    p->env_attack       = 0.01f;
    p->env_decay        = 0.1f;
    p->env_sustain      = 0.7f;
    p->env_release      = 0.2f;

    p->follow_attack    = 0.005f;
    p->follow_release   = 0.1f;
    p->follow_amount    = 0.5f;

    p->lfo_rate         = 4.0f;
    p->lfo_depth        = 0.0f;
    p->lfo_target       = 0;

    p->volume           = 0.8f;
}

/* -------------------------------------------------------------------------
 * Plugin lifecycle
 * ---------------------------------------------------------------------- */
static void *plugin_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;

    TrumpetSynth *s = calloc(1, sizeof(TrumpetSynth));
    if (!s) return NULL;

    params_init_defaults(&s->p);

    s->detected_hz = 440.0f;
    s->smoothed_hz = 440.0f;
    s->gate        = 0;
    s->osc_phase   = 0.0f;
    s->lfo_phase   = 0.0f;
    s->env_stage   = ENV_IDLE;
    s->env_value   = 0.0f;
    s->follow_env  = 0.0f;

    /* TODO: initialise aubio pitch detector
     *   s->pitch_buf = new_fvec(BLOCK_SIZE);
     *   s->pitch_out = new_fvec(1);
     *   s->pitch_obj = new_aubio_pitch("yin", BLOCK_SIZE, BLOCK_SIZE,
     *                                  (uint_t)SAMPLE_RATE);
     *   aubio_pitch_set_unit(s->pitch_obj, "Hz");
     *   aubio_pitch_set_tolerance(s->pitch_obj, s->p.pitch_confidence);
     */

    return s;
}

static void plugin_destroy(void *instance) {
    if (!instance) return;
    TrumpetSynth *s = (TrumpetSynth *)instance;

    /* TODO: free aubio objects
     *   del_aubio_pitch(s->pitch_obj);
     *   del_fvec(s->pitch_buf);
     *   del_fvec(s->pitch_out);
     */

    free(s);
}

/* -------------------------------------------------------------------------
 * Parameters
 * ---------------------------------------------------------------------- */
static void plugin_set_param(void *instance, const char *key, const char *val) {
    TrumpetSynth *s = (TrumpetSynth *)instance;
    Params *p = &s->p;

    if      (strcmp(key, "input_gain")       == 0) p->input_gain       = parse_float(val, p->input_gain);
    else if (strcmp(key, "pitch_confidence") == 0) p->pitch_confidence = parse_float(val, p->pitch_confidence);
    else if (strcmp(key, "pitch_smooth")     == 0) p->pitch_smooth     = parse_float(val, p->pitch_smooth);
    else if (strcmp(key, "osc_wave")         == 0) p->osc_wave         = parse_wave(val);
    else if (strcmp(key, "osc_detune")       == 0) p->osc_detune       = parse_float(val, p->osc_detune);
    else if (strcmp(key, "osc_level")        == 0) p->osc_level        = parse_float(val, p->osc_level);
    else if (strcmp(key, "filter_cutoff")    == 0) p->filter_cutoff    = parse_float(val, p->filter_cutoff);
    else if (strcmp(key, "filter_reso")      == 0) p->filter_reso      = parse_float(val, p->filter_reso);
    else if (strcmp(key, "env_attack")       == 0) p->env_attack       = parse_float(val, p->env_attack);
    else if (strcmp(key, "env_decay")        == 0) p->env_decay        = parse_float(val, p->env_decay);
    else if (strcmp(key, "env_sustain")      == 0) p->env_sustain      = parse_float(val, p->env_sustain);
    else if (strcmp(key, "env_release")      == 0) p->env_release      = parse_float(val, p->env_release);
    else if (strcmp(key, "follow_attack")    == 0) p->follow_attack    = parse_float(val, p->follow_attack);
    else if (strcmp(key, "follow_release")   == 0) p->follow_release   = parse_float(val, p->follow_release);
    else if (strcmp(key, "follow_amount")    == 0) p->follow_amount    = parse_float(val, p->follow_amount);
    else if (strcmp(key, "lfo_rate")         == 0) p->lfo_rate         = parse_float(val, p->lfo_rate);
    else if (strcmp(key, "lfo_depth")        == 0) p->lfo_depth        = parse_float(val, p->lfo_depth);
    else if (strcmp(key, "lfo_target")       == 0) p->lfo_target       = parse_lfo_target(val);
    else if (strcmp(key, "volume")           == 0) p->volume           = parse_float(val, p->volume);
}

static int plugin_get_param(void *instance, const char *key, char *buf, int buf_len) {
    TrumpetSynth *s = (TrumpetSynth *)instance;
    Params *p = &s->p;

    static const char *wave_names[]   = { "sine", "triangle", "saw", "square" };
    static const char *target_names[] = { "pitch", "filter", "amp" };

    if      (strcmp(key, "input_gain")       == 0) return snprintf(buf, buf_len, "%.4f", p->input_gain);
    else if (strcmp(key, "pitch_confidence") == 0) return snprintf(buf, buf_len, "%.4f", p->pitch_confidence);
    else if (strcmp(key, "pitch_smooth")     == 0) return snprintf(buf, buf_len, "%.4f", p->pitch_smooth);
    else if (strcmp(key, "osc_wave")         == 0) return snprintf(buf, buf_len, "%s",   wave_names[p->osc_wave]);
    else if (strcmp(key, "osc_detune")       == 0) return snprintf(buf, buf_len, "%.2f", p->osc_detune);
    else if (strcmp(key, "osc_level")        == 0) return snprintf(buf, buf_len, "%.4f", p->osc_level);
    else if (strcmp(key, "filter_cutoff")    == 0) return snprintf(buf, buf_len, "%.2f", p->filter_cutoff);
    else if (strcmp(key, "filter_reso")      == 0) return snprintf(buf, buf_len, "%.4f", p->filter_reso);
    else if (strcmp(key, "env_attack")       == 0) return snprintf(buf, buf_len, "%.4f", p->env_attack);
    else if (strcmp(key, "env_decay")        == 0) return snprintf(buf, buf_len, "%.4f", p->env_decay);
    else if (strcmp(key, "env_sustain")      == 0) return snprintf(buf, buf_len, "%.4f", p->env_sustain);
    else if (strcmp(key, "env_release")      == 0) return snprintf(buf, buf_len, "%.4f", p->env_release);
    else if (strcmp(key, "follow_attack")    == 0) return snprintf(buf, buf_len, "%.4f", p->follow_attack);
    else if (strcmp(key, "follow_release")   == 0) return snprintf(buf, buf_len, "%.4f", p->follow_release);
    else if (strcmp(key, "follow_amount")    == 0) return snprintf(buf, buf_len, "%.4f", p->follow_amount);
    else if (strcmp(key, "lfo_rate")         == 0) return snprintf(buf, buf_len, "%.4f", p->lfo_rate);
    else if (strcmp(key, "lfo_depth")        == 0) return snprintf(buf, buf_len, "%.4f", p->lfo_depth);
    else if (strcmp(key, "lfo_target")       == 0) return snprintf(buf, buf_len, "%s",   target_names[p->lfo_target]);
    else if (strcmp(key, "volume")           == 0) return snprintf(buf, buf_len, "%.4f", p->volume);
    else if (strcmp(key, "detected_hz")      == 0) return snprintf(buf, buf_len, "%.2f", s->smoothed_hz);
    else if (strcmp(key, "gate")             == 0) return snprintf(buf, buf_len, "%d",   s->gate);

    return -1;
}

/* -------------------------------------------------------------------------
 * MIDI (not primary trigger mechanism, but accepted for manual override)
 * ---------------------------------------------------------------------- */
static void plugin_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    if (len < 3) return;
    TrumpetSynth *s = (TrumpetSynth *)instance;

    uint8_t status = msg[0] & 0xF0;
    if (status == 0x90 && msg[2] > 0) {
        /* Note On: override tracked pitch and trigger envelope */
        s->smoothed_hz = 440.0f * powf(2.0f, (msg[1] - 69) / 12.0f);
        s->gate        = 1;
        s->env_stage   = ENV_ATTACK;
    } else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) {
        s->gate      = 0;
        s->env_stage = ENV_RELEASE;
    }
}

/* -------------------------------------------------------------------------
 * DSP helpers
 * ---------------------------------------------------------------------- */

/* One-pole envelope follower, returns level 0..1 */
static inline float env_follower_tick(float *env, float in,
                                      float attack_coef,
                                      float release_coef) {
    float abs_in = fabsf(in);
    if (abs_in > *env)
        *env += attack_coef  * (abs_in - *env);
    else
        *env += release_coef * (abs_in - *env);
    return *env;
}

/* ADSR tick — returns envelope amplitude 0..1 */
static inline float adsr_tick(TrumpetSynth *s) {
    Params *p = &s->p;
    float dt = 1.0f / SAMPLE_RATE;

    switch (s->env_stage) {
        case ENV_ATTACK:
            s->env_value += dt / p->env_attack;
            if (s->env_value >= 1.0f) {
                s->env_value = 1.0f;
                s->env_stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY:
            s->env_value -= dt / p->env_decay * (1.0f - p->env_sustain);
            if (s->env_value <= p->env_sustain) {
                s->env_value = p->env_sustain;
                s->env_stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            s->env_value = p->env_sustain;
            break;
        case ENV_RELEASE:
            s->env_value -= dt / p->env_release * s->env_value;
            if (s->env_value <= 0.0f) {
                s->env_value = 0.0f;
                s->env_stage = ENV_IDLE;
            }
            break;
        case ENV_IDLE:
        default:
            s->env_value = 0.0f;
            break;
    }
    return s->env_value;
}

/* Simple bandlimited saw (Blep-free placeholder — TODO: PolyBlep) */
static inline float osc_tick(TrumpetSynth *s, float freq_hz) {
    float phase = s->osc_phase;
    float sample;

    switch (s->p.osc_wave) {
        case 0: /* sine */
            sample = sinf(2.0f * (float)M_PI * phase);
            break;
        case 1: /* triangle */
            sample = (phase < 0.5f)
                     ? (4.0f * phase - 1.0f)
                     : (3.0f - 4.0f * phase);
            break;
        case 2: /* saw (naive) */
            sample = 2.0f * phase - 1.0f;
            break;
        case 3: /* square */
            sample = (phase < 0.5f) ? 1.0f : -1.0f;
            break;
        default:
            sample = 0.0f;
    }

    s->osc_phase = phase + freq_hz / SAMPLE_RATE;
    if (s->osc_phase >= 1.0f) s->osc_phase -= 1.0f;

    return sample;
}

/* Airwindows Capacitor (low-pass only) — placeholder coefficients.
 * TODO: replace with full Airwindows Capacitor2 implementation. */
static inline float capacitor_tick(TrumpetSynth *s, float in, int ch) {
    Params *p = &s->p;
    float cutoff = p->filter_cutoff / (SAMPLE_RATE * 0.5f);
    if (cutoff > 1.0f) cutoff = 1.0f;

    float iir_amount = cutoff * (1.0f - p->filter_reso * 0.5f);
    s->cap_low[ch]  += iir_amount * (in            - s->cap_low[ch]);
    s->cap_band[ch] += iir_amount * (s->cap_low[ch] - s->cap_band[ch]);

    return s->cap_low[ch];
}

/* -------------------------------------------------------------------------
 * Render block
 * ---------------------------------------------------------------------- */
static void plugin_render_block(void *instance, int16_t *out_lr, int frames) {
    TrumpetSynth *s  = (TrumpetSynth *)instance;
    Params        *p = &s->p;

    /* --- Audio input buffer (stereo interleaved int16 from host shared memory) --- */
    const int16_t *audio_in = NULL;
    if (g_host && g_host->mapped_memory)
        audio_in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    /* --- Pitch detection (TODO: wire in aubio) ---
     *
     * Copy the mono-downmix of audio_in into the aubio input vector, then
     * call aubio_pitch_do().  audio_in is stereo interleaved so use [i*2].
     *
     *   for (int i = 0; i < frames; i++)
     *       s->pitch_buf->data[i] = audio_in
     *           ? (smpl_t)(audio_in[i*2] * p->input_gain / 32768.0f)
     *           : 0.0f;
     *
     *   aubio_pitch_do(s->pitch_obj, s->pitch_buf, s->pitch_out);
     *   float conf = aubio_pitch_get_confidence(s->pitch_obj);
     *   float hz   = s->pitch_out->data[0];
     *
     *   if (conf >= p->pitch_confidence && hz > 20.0f && hz < 4000.0f) {
     *       s->detected_hz = hz;
     *       if (!s->gate) { s->gate = 1; s->env_stage = ENV_ATTACK; }
     *   } else {
     *       if (s->gate) { s->gate = 0; s->env_stage = ENV_RELEASE; }
     *   }
     */

    /* One-pole pitch smoother coefficient */
    float smooth_coef = 1.0f - expf(-1.0f / (SAMPLE_RATE * fmaxf(p->pitch_smooth, 1e-4f)));

    /* Pre-compute time-to-coefficient conversions for envelope follower */
    float flw_att  = 1.0f - expf(-1.0f / (SAMPLE_RATE * fmaxf(p->follow_attack,  1e-4f)));
    float flw_rel  = 1.0f - expf(-1.0f / (SAMPLE_RATE * fmaxf(p->follow_release, 1e-4f)));

    for (int i = 0; i < frames; i++) {
        /* --- Smooth detected pitch --- */
        s->smoothed_hz += smooth_coef * (s->detected_hz - s->smoothed_hz);

        /* --- LFO --- */
        float lfo_val = sinf(2.0f * (float)M_PI * s->lfo_phase);
        s->lfo_phase += p->lfo_rate / SAMPLE_RATE;
        if (s->lfo_phase >= 1.0f) s->lfo_phase -= 1.0f;

        /* --- Compute oscillator frequency with detune and optional LFO --- */
        float freq = s->smoothed_hz
                     * powf(2.0f, p->osc_detune / 1200.0f)
                     * ((p->lfo_target == 0)
                        ? (1.0f + lfo_val * p->lfo_depth * 0.1f)
                        : 1.0f);

        /* --- Oscillator --- */
        float osc = osc_tick(s, freq) * p->osc_level;

        /* --- Filter cutoff modulated by LFO if target == filter --- */
        if (p->lfo_target == 1) {
            /* modulation applied inside capacitor via temporary param override
             * is deferred; direct approach: scale cutoff here */
            float orig_cutoff = p->filter_cutoff;
            p->filter_cutoff  = orig_cutoff * (1.0f + lfo_val * p->lfo_depth);
            if (p->filter_cutoff < 20.0f)    p->filter_cutoff = 20.0f;
            if (p->filter_cutoff > 20000.0f) p->filter_cutoff = 20000.0f;
            float filtered_l = capacitor_tick(s, osc, 0);
            float filtered_r = capacitor_tick(s, osc, 1);
            p->filter_cutoff  = orig_cutoff;
            osc = (filtered_l + filtered_r) * 0.5f;
        } else {
            float filtered_l = capacitor_tick(s, osc, 0);
            float filtered_r = capacitor_tick(s, osc, 1);
            osc = (filtered_l + filtered_r) * 0.5f;
        }

        /* --- ADSR envelope --- */
        float env = adsr_tick(s);

        /* --- Envelope follower (tracks input loudness, drives amplitude) ---
         * Use the left channel of audio_in when available; fall back to osc. */
        float in_sample = audio_in ? (audio_in[i * 2] * p->input_gain / 32768.0f) : osc;
        float follow = env_follower_tick(&s->follow_env, in_sample, flw_att, flw_rel);

        /* Mix ADSR and follower */
        float combined_env = env * (1.0f - p->follow_amount)
                           + follow * p->follow_amount;

        /* --- Amplitude LFO --- */
        float amp = combined_env;
        if (p->lfo_target == 2) {
            amp *= (1.0f + lfo_val * p->lfo_depth * 0.5f);
            if (amp < 0.0f) amp = 0.0f;
        }

        /* --- Output --- */
        float sample = osc * amp * p->volume;
        int16_t out  = (int16_t)(sample * 32767.0f);

        out_lr[i * 2 + 0] = out; /* L */
        out_lr[i * 2 + 1] = out; /* R */
    }
}

/* -------------------------------------------------------------------------
 * Error reporting
 * ---------------------------------------------------------------------- */
static int plugin_get_error(void *instance, char *buf, int buf_len) {
    (void)instance;
    (void)buf;
    (void)buf_len;
    return 0; /* no error */
}

/* -------------------------------------------------------------------------
 * Plugin entry point — must match the symbol Schwung looks up via dlsym()
 * ---------------------------------------------------------------------- */
static plugin_api_v2_t g_plugin = {
    .api_version      = 2,
    .create_instance  = plugin_create,
    .destroy_instance = plugin_destroy,
    .on_midi          = plugin_on_midi,
    .set_param        = plugin_set_param,
    .get_param        = plugin_get_param,
    .get_error        = plugin_get_error,
    .render_block     = plugin_render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_plugin;
}
