/* trumpet_synth.c
 *
 * Monophonic pitch-tracking synthesizer for the Move Everything / Schwung framework.
 *
 * Signal path:
 *   Audio input (trumpet via Roland Silent Brass)
 *     → Hard limiter (clip ±1.0)
 *     → Soft compressor (peak-following, feed-forward)
 *     → aubio pitch detection
 *     → Oscillator (sine / triangle / saw / square)
 *     → ADSR envelope
 *     → Airwindows Capacitor low-pass filter
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
    float pitch_confidence;   /* minimum aubio confidence threshold (0..1) */
    float pitch_smooth;       /* one-pole pitch smoother time constant (s)  */

    /* Input protection: hard limiter (fixed) → soft compressor */
    float comp_threshold;     /* linear amplitude at which compression begins (0..1) */
    float comp_ratio;         /* compression ratio (1 = no compression, 20 = hard limit) */
    float comp_makeup;        /* post-compression makeup gain (linear, 0..4) */

    /* Oscillator */
    int   osc_wave;           /* 0=sine 1=tri 2=saw 3=square */
    float osc_detune;         /* cents, -100..+100 */
    float osc_level;

    /* ADSR */
    float env_attack;
    float env_decay;
    float env_sustain;
    float env_release;

    /* Airwindows Capacitor filter */
    float filter_cutoff;      /* Hz */
    float filter_reso;        /* 0..1 */

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

    /* Input protection */
    float comp_env;           /* compressor peak envelope state */

    /* Pitch tracking */
    float detected_hz;        /* raw aubio output */
    float smoothed_hz;        /* after one-pole smoother */
    int   gate;               /* 1 = pitch detected above confidence */

    /* Oscillator */
    float osc_phase;          /* 0..1 */

    /* ADSR */
    EnvStage env_stage;
    float    env_value;

    /* Airwindows Capacitor internal state */
    float cap_low[2];         /* lowpass state, L + R */
    float cap_band[2];        /* bandpass state, L + R */

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

/* -------------------------------------------------------------------------
 * Default parameters
 * ---------------------------------------------------------------------- */
static void params_init_defaults(Params *p) {
    p->input_gain       = 1.0f;
    p->pitch_confidence = 0.8f;
    p->pitch_smooth     = 0.05f;

    p->comp_threshold   = 0.8f;   /* compress above 80% full scale */
    p->comp_ratio       = 4.0f;   /* 4:1 */
    p->comp_makeup      = 1.0f;

    p->osc_wave         = 2;      /* saw */
    p->osc_detune       = 0.0f;
    p->osc_level        = 0.8f;

    p->env_attack       = 0.01f;
    p->env_decay        = 0.1f;
    p->env_sustain      = 0.7f;
    p->env_release      = 0.2f;

    p->filter_cutoff    = 4000.0f;
    p->filter_reso      = 0.3f;

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

    s->comp_env    = 0.0f;
    s->detected_hz = 440.0f;
    s->smoothed_hz = 440.0f;
    s->gate        = 0;
    s->osc_phase   = 0.0f;
    s->env_stage   = ENV_IDLE;
    s->env_value   = 0.0f;

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
    else if (strcmp(key, "comp_threshold")   == 0) p->comp_threshold   = parse_float(val, p->comp_threshold);
    else if (strcmp(key, "comp_ratio")       == 0) p->comp_ratio       = parse_float(val, p->comp_ratio);
    else if (strcmp(key, "comp_makeup")      == 0) p->comp_makeup      = parse_float(val, p->comp_makeup);
    else if (strcmp(key, "osc_wave")         == 0) p->osc_wave         = parse_wave(val);
    else if (strcmp(key, "osc_detune")       == 0) p->osc_detune       = parse_float(val, p->osc_detune);
    else if (strcmp(key, "osc_level")        == 0) p->osc_level        = parse_float(val, p->osc_level);
    else if (strcmp(key, "env_attack")       == 0) p->env_attack       = parse_float(val, p->env_attack);
    else if (strcmp(key, "env_decay")        == 0) p->env_decay        = parse_float(val, p->env_decay);
    else if (strcmp(key, "env_sustain")      == 0) p->env_sustain      = parse_float(val, p->env_sustain);
    else if (strcmp(key, "env_release")      == 0) p->env_release      = parse_float(val, p->env_release);
    else if (strcmp(key, "filter_cutoff")    == 0) p->filter_cutoff    = parse_float(val, p->filter_cutoff);
    else if (strcmp(key, "filter_reso")      == 0) p->filter_reso      = parse_float(val, p->filter_reso);
    else if (strcmp(key, "volume")           == 0) p->volume           = parse_float(val, p->volume);
}

static int plugin_get_param(void *instance, const char *key, char *buf, int buf_len) {
    TrumpetSynth *s = (TrumpetSynth *)instance;
    Params *p = &s->p;

    static const char *wave_names[] = { "sine", "triangle", "saw", "square" };

    if      (strcmp(key, "input_gain")       == 0) return snprintf(buf, buf_len, "%.4f", p->input_gain);
    else if (strcmp(key, "pitch_confidence") == 0) return snprintf(buf, buf_len, "%.4f", p->pitch_confidence);
    else if (strcmp(key, "pitch_smooth")     == 0) return snprintf(buf, buf_len, "%.4f", p->pitch_smooth);
    else if (strcmp(key, "comp_threshold")   == 0) return snprintf(buf, buf_len, "%.4f", p->comp_threshold);
    else if (strcmp(key, "comp_ratio")       == 0) return snprintf(buf, buf_len, "%.2f", p->comp_ratio);
    else if (strcmp(key, "comp_makeup")      == 0) return snprintf(buf, buf_len, "%.4f", p->comp_makeup);
    else if (strcmp(key, "osc_wave")         == 0) return snprintf(buf, buf_len, "%s",   wave_names[p->osc_wave]);
    else if (strcmp(key, "osc_detune")       == 0) return snprintf(buf, buf_len, "%.2f", p->osc_detune);
    else if (strcmp(key, "osc_level")        == 0) return snprintf(buf, buf_len, "%.4f", p->osc_level);
    else if (strcmp(key, "env_attack")       == 0) return snprintf(buf, buf_len, "%.4f", p->env_attack);
    else if (strcmp(key, "env_decay")        == 0) return snprintf(buf, buf_len, "%.4f", p->env_decay);
    else if (strcmp(key, "env_sustain")      == 0) return snprintf(buf, buf_len, "%.4f", p->env_sustain);
    else if (strcmp(key, "env_release")      == 0) return snprintf(buf, buf_len, "%.4f", p->env_release);
    else if (strcmp(key, "filter_cutoff")    == 0) return snprintf(buf, buf_len, "%.2f", p->filter_cutoff);
    else if (strcmp(key, "filter_reso")      == 0) return snprintf(buf, buf_len, "%.4f", p->filter_reso);
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

/* Hard limiter — clips to ±1.0 */
static inline float hard_limit(float x) {
    if (x >  1.0f) return  1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
}

/* Soft feed-forward compressor — peak envelope detector, sample-by-sample.
 * atk_tc / rel_tc are one-pole coefficients (pre-computed once per block).
 * Returns the gain-reduced, makeup-amplified sample. */
static inline float compress_sample(float *env, float in,
                                    float threshold, float ratio, float makeup,
                                    float atk_tc, float rel_tc) {
    float level = fabsf(in);
    if (level > *env)
        *env += atk_tc * (level - *env);
    else
        *env += rel_tc * (level - *env);

    float gain = 1.0f;
    if (*env > threshold && threshold > 0.0f) {
        float reduced = threshold + (*env - threshold) / ratio;
        gain = reduced / *env;
    }
    return in * gain * makeup;
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

/* Oscillator — naive waveforms (TODO: PolyBlep for saw/square) */
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
        case 2: /* saw */
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

/* Airwindows Capacitor (low-pass) — placeholder IIR.
 * TODO: replace with full Airwindows Capacitor2 implementation. */
static inline float capacitor_tick(TrumpetSynth *s, float in, int ch) {
    Params *p = &s->p;
    float cutoff = p->filter_cutoff / (SAMPLE_RATE * 0.5f);
    if (cutoff > 1.0f) cutoff = 1.0f;

    float iir = cutoff * (1.0f - p->filter_reso * 0.5f);
    s->cap_low[ch]  += iir * (in             - s->cap_low[ch]);
    s->cap_band[ch] += iir * (s->cap_low[ch] - s->cap_band[ch]);

    return s->cap_low[ch];
}

/* -------------------------------------------------------------------------
 * Render block
 * ---------------------------------------------------------------------- */
static void plugin_render_block(void *instance, int16_t *out_lr, int frames) {
    TrumpetSynth *s = (TrumpetSynth *)instance;
    Params       *p = &s->p;

    /* Audio input buffer from host shared memory (stereo interleaved int16) */
    const int16_t *audio_in = NULL;
    if (g_host && g_host->mapped_memory)
        audio_in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    /* Compressor time constants — 1 ms attack, 100 ms release.
     * These are constant expressions; -O3 evaluates them at compile time. */
    const float comp_atk = 1.0f - expf(-1.0f / (SAMPLE_RATE * 0.001f));
    const float comp_rel = 1.0f - expf(-1.0f / (SAMPLE_RATE * 0.100f));

    /* --- Input protection + pitch detection (TODO: wire in aubio) -------
     *
     * Run the full input chain once per block before synthesis, feeding the
     * compressor-conditioned mono signal into the aubio pitch buffer:
     *
     *   for (int i = 0; i < frames; i++) {
     *       float raw   = audio_in
     *                   ? (audio_in[i * 2] * p->input_gain / 32768.0f)
     *                   : 0.0f;
     *       float cond  = compress_sample(&s->comp_env,
     *                                     hard_limit(raw),
     *                                     p->comp_threshold, p->comp_ratio,
     *                                     p->comp_makeup,
     *                                     comp_atk, comp_rel);
     *       s->pitch_buf->data[i] = (smpl_t)cond;
     *   }
     *   aubio_pitch_do(s->pitch_obj, s->pitch_buf, s->pitch_out);
     *   float conf = aubio_pitch_get_confidence(s->pitch_obj);
     *   float hz   = s->pitch_out->data[0];
     *   if (conf >= p->pitch_confidence && hz > 20.0f && hz < 4000.0f) {
     *       s->detected_hz = hz;
     *       if (!s->gate) { s->gate = 1; s->env_stage = ENV_ATTACK; }
     *   } else {
     *       if (s->gate) { s->gate = 0; s->env_stage = ENV_RELEASE; }
     *   }
     * ------------------------------------------------------------------- */

    /* Pitch smoother coefficient */
    const float smooth_coef = 1.0f - expf(-1.0f / (SAMPLE_RATE * fmaxf(p->pitch_smooth, 1e-4f)));

    for (int i = 0; i < frames; i++) {
        /* Smooth detected pitch toward target */
        s->smoothed_hz += smooth_coef * (s->detected_hz - s->smoothed_hz);

        /* Oscillator frequency with detune */
        float freq = s->smoothed_hz * powf(2.0f, p->osc_detune / 1200.0f);

        /* Oscillator */
        float osc = osc_tick(s, freq) * p->osc_level;

        /* ADSR envelope */
        float env = adsr_tick(s);

        /* Airwindows Capacitor filter — same signal to both channels */
        float out_l = capacitor_tick(s, osc * env, 0);
        float out_r = capacitor_tick(s, osc * env, 1);

        /* Output */
        out_lr[i * 2 + 0] = (int16_t)(out_l * p->volume * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(out_r * p->volume * 32767.0f);
    }

    /* Advance compressor envelope state in sync with audio input even while
     * aubio is not yet wired, to keep comp_env decay running correctly. */
    if (audio_in) {
        for (int i = 0; i < frames; i++) {
            float raw = audio_in[i * 2] * p->input_gain / 32768.0f;
            compress_sample(&s->comp_env, hard_limit(raw),
                            p->comp_threshold, p->comp_ratio, p->comp_makeup,
                            comp_atk, comp_rel);
        }
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
