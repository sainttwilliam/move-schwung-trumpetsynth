/* trumpet_synth.c
 *
 * Minimal pitch-tracking synthesizer.
 * Signal path: audio in → autocorrelation pitch detection → oscillator → output
 *
 * Plugin API: audio_fx_api_v2_t (Schwung / Move Everything)
 * Audio format: stereo interleaved int16, 44100 Hz, 128 frames/block
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <aubio/aubio.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "capacitor.h"

#define SAMPLE_RATE  44100.0f
#define BLOCK_SIZE   128
#define PITCH_WIN    2048   /* analysis window (~46ms at 44100 Hz)                       */
#define PITCH_HOP    512    /* new samples per pitch detection call                       */
#define GATE_ATTACK_RATE  (1.0f / (0.005f * SAMPLE_RATE))  /* 5ms open ramp  */
#define GATE_RELEASE_RATE (1.0f / (0.005f * SAMPLE_RATE))  /* 5ms close ramp */
#define AC_MIN_LAG   32     /* 44100 / 1378 Hz ≈ 32.0 — first lag above 1378 Hz          */
#define AC_MAX_LAG   294    /* 44100 /  150 Hz = 294.0 — last lag above 150 Hz           */
#define AC_SIEVE_MIN  8     /* AC_MIN_LAG / 4 — lowest lag computed (for sieve lookups)  */

/* -------------------------------------------------------------------------
 * Parameters
 * ---------------------------------------------------------------------- */
typedef struct {
    float input_gain;
    float pitch_confidence;  /* aubio YIN confidence threshold (0..1) */
    float pitch_smooth;      /* one-pole smoother time constant (s)   */
    float gate_threshold;    /* RMS level below which oscillator mutes */
    int   osc_wave;          /* 0=sine 1=triangle 2=saw 3=square      */
    float osc_detune;        /* cents, -100..+100                      */
    float osc_level;
    float volume;
    float filter_cutoff;     /* lowpass cutoff Hz, 20..20000           */
    float filter_resonance;  /* feedback resonance, 0..1               */
} Params;

/* -------------------------------------------------------------------------
 * Instance state
 * ---------------------------------------------------------------------- */
static const host_api_v1_t *g_host = NULL;

typedef struct {
    fvec_t        *pitch_buf;  /* PITCH_WIN-sample rolling analysis window (via aubio alloc) */

    float detected_hz;
    float smoothed_hz;
    int   gate;
    int   gate_hold_remaining; /* samples left in minimum hold period   */
    int   block_count;         /* increments every PITCH_HOP samples    */

    smpl_t hop_buf[PITCH_HOP]; /* accumulator: fills at BLOCK_SIZE rate */
    int    hop_fill;           /* samples currently in hop_buf          */

    float  raw_hz_buf[5];     /* circular buffer of last 5 processed hz */
    int    raw_hz_idx;        /* next write position                    */
    int    raw_hz_count;      /* number of valid entries (max 5)        */

    float     osc_phase;
    float     gate_gain;         /* smoothed gate: ramps 0→1 on open, 1→0 on close  */
    int       onset_pending;     /* set on gate open; cleared after first valid pitch */
    float     last_rms;          /* RMS of previous block — used for pitch freeze     */
    float     frozen_hz;         /* smoothed_hz captured at gate close                */
    int       gate_closed_samples; /* frames elapsed since gate closed               */
    Capacitor filter;

    Params p;
} TrumpetSynth;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static float parse_float(const char *val, float fallback) {
    if (!val || val[0] == '\0') return fallback;
    char *end;
    float f = strtof(val, &end);
    return (end != val) ? f : fallback;
}

static int parse_wave(const char *val) {
    if (!val)                        return 2;
    if (strcmp(val, "sine")     == 0) return 0;
    if (strcmp(val, "triangle") == 0) return 1;
    if (strcmp(val, "saw")      == 0) return 2;
    if (strcmp(val, "square")   == 0) return 3;
    return 2;
}

static void params_init_defaults(Params *p) {
    p->input_gain       = 1.0f;
    p->pitch_confidence = 0.0f;
    p->pitch_smooth     = 0.005f;
    p->gate_threshold   = 0.007f; /* RMS ~-43 dBFS */
    p->osc_wave         = 0;
    p->osc_detune       = 0.0f;
    p->osc_level        = 0.8f;
    p->volume           = 0.8f;
    p->filter_cutoff    = 2000.0f;
    p->filter_resonance = 0.3f;
}

/* -------------------------------------------------------------------------
 * Persistence filter: groups readings within a 2:1 ratio as the same note
 * (covers octave alternation). If 3+ readings agree, returns the highest
 * value in that group — biasing toward the upper octave, which is correct
 * for trumpet. Returns prev_hz unchanged if fewer than 3 agree.
 * ---------------------------------------------------------------------- */
static float persistence_filter(const float *buf, int n, float prev_hz) {
    int   best_count = 0;
    float best_max   = 0.0f;
    for (int i = 0; i < n; i++) {
        int   count = 0;
        float grp_max = 0.0f;
        for (int j = 0; j < n; j++) {
            float ratio = buf[i] > buf[j] ? buf[i] / buf[j] : buf[j] / buf[i];
            if (ratio <= 2.0f) {
                count++;
                if (buf[j] > grp_max) grp_max = buf[j];
            }
        }
        if (count > best_count) { best_count = count; best_max = grp_max; }
    }
    return (best_count >= 3) ? best_max : prev_hz;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */
static void *plugin_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;

    TrumpetSynth *s = calloc(1, sizeof(TrumpetSynth));
    if (!s) return NULL;

    params_init_defaults(&s->p);
    s->detected_hz = 440.0f;
    s->smoothed_hz = 440.0f;

    s->pitch_buf = new_fvec(PITCH_WIN);
    if (!s->pitch_buf) {
        free(s);
        return NULL;
    }
    capacitor_init(&s->filter);
    capacitor_set_cutoff(&s->filter, s->p.filter_cutoff, SAMPLE_RATE);

    return s;
}

static void plugin_destroy(void *instance) {
    if (!instance) return;
    TrumpetSynth *s = (TrumpetSynth *)instance;
    del_fvec(s->pitch_buf);
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
    else if (strcmp(key, "gate_threshold")   == 0) p->gate_threshold   = parse_float(val, p->gate_threshold);
    else if (strcmp(key, "osc_wave")         == 0) p->osc_wave         = parse_wave(val);
    else if (strcmp(key, "osc_detune")       == 0) p->osc_detune       = parse_float(val, p->osc_detune);
    else if (strcmp(key, "osc_level")        == 0) p->osc_level        = parse_float(val, p->osc_level);
    else if (strcmp(key, "volume")           == 0) p->volume           = parse_float(val, p->volume);
    else if (strcmp(key, "filter_cutoff")    == 0) {
        p->filter_cutoff = parse_float(val, p->filter_cutoff);
        capacitor_set_cutoff(&s->filter, p->filter_cutoff, SAMPLE_RATE);
    }
    else if (strcmp(key, "filter_resonance") == 0) p->filter_resonance = parse_float(val, p->filter_resonance);
}

static int plugin_get_param(void *instance, const char *key, char *buf, int buf_len) {
    TrumpetSynth *s = (TrumpetSynth *)instance;
    Params *p = &s->p;
    static const char *wave_names[] = { "sine", "triangle", "saw", "square" };

    if      (strcmp(key, "input_gain")       == 0) return snprintf(buf, buf_len, "%.4f", p->input_gain);
    else if (strcmp(key, "pitch_confidence") == 0) return snprintf(buf, buf_len, "%.4f", p->pitch_confidence);
    else if (strcmp(key, "pitch_smooth")     == 0) return snprintf(buf, buf_len, "%.4f", p->pitch_smooth);
    else if (strcmp(key, "gate_threshold")   == 0) return snprintf(buf, buf_len, "%.4f", p->gate_threshold);
    else if (strcmp(key, "osc_wave")         == 0) return snprintf(buf, buf_len, "%s",   wave_names[p->osc_wave]);
    else if (strcmp(key, "osc_detune")       == 0) return snprintf(buf, buf_len, "%.2f", p->osc_detune);
    else if (strcmp(key, "osc_level")        == 0) return snprintf(buf, buf_len, "%.4f", p->osc_level);
    else if (strcmp(key, "volume")           == 0) return snprintf(buf, buf_len, "%.4f", p->volume);
    else if (strcmp(key, "filter_cutoff")    == 0) return snprintf(buf, buf_len, "%.2f", p->filter_cutoff);
    else if (strcmp(key, "filter_resonance") == 0) return snprintf(buf, buf_len, "%.4f", p->filter_resonance);
    else if (strcmp(key, "detected_hz")      == 0) return snprintf(buf, buf_len, "%.2f", s->smoothed_hz);
    else if (strcmp(key, "gate")             == 0) return snprintf(buf, buf_len, "%d",   s->gate);

    return -1;
}

/* -------------------------------------------------------------------------
 * MIDI — override pitch for testing without a trumpet
 * ---------------------------------------------------------------------- */
static void plugin_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    if (len < 3) return;
    TrumpetSynth *s = (TrumpetSynth *)instance;
    uint8_t status = msg[0] & 0xF0;
    if (status == 0x90 && msg[2] > 0) {
        s->smoothed_hz = 440.0f * powf(2.0f, (msg[1] - 69) / 12.0f);
        s->gate = 1;
    } else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) {
        s->gate = 0;
    }
}

/* -------------------------------------------------------------------------
 * Autocorrelation pitch detector with harmonic sieve.
 *
 * Computes normalised autocorrelation over the trumpet fundamental range
 * (150–1378 Hz, lags 32–294). For each local peak, the harmonic sieve
 * checks whether a divisor lag (half, third, or quarter period) has a
 * stronger peak: if so, this lag is a sub-harmonic and is rejected.
 * Returns the detected frequency in Hz, or 0.0f on silence/no peak.
 * ---------------------------------------------------------------------- */
static float autocorr_pitch(const smpl_t *buf, int win) {
    float r0 = 0.0f;
    for (int i = 0; i < win; i++) r0 += buf[i] * buf[i];
    if (r0 < 1e-6f) return 0.0f;

    /* Autocorrelation for lags needed by both the search range and the sieve */
    float r[AC_MAX_LAG + 1];
    memset(r, 0, sizeof(r));
    for (int lag = AC_SIEVE_MIN; lag <= AC_MAX_LAG; lag++) {
        float sum = 0.0f;
        int   n   = win - lag;
        for (int i = 0; i < n; i++) sum += buf[i] * buf[i + lag];
        r[lag] = sum / r0;
    }

    float best_val = 0.1f;  /* minimum normalised autocorrelation to accept */
    int   best_lag = 0;

    for (int lag = AC_MIN_LAG; lag < AC_MAX_LAG; lag++) {
        /* Local maximum */
        if (r[lag] <= r[lag - 1] || r[lag] <= r[lag + 1]) continue;

        /* Harmonic sieve: reject if any divisor lag has a stronger peak AND
         * that divisor lag is itself within the valid search window [AC_MIN_LAG,
         * AC_MAX_LAG]. Guarding with AC_MIN_LAG (not AC_SIEVE_MIN) prevents
         * instrument harmonic energy above the search range (e.g. r[31] for a
         * 700 Hz note) from incorrectly triggering rejection of true fundamentals
         * in the 650–980 Hz range. */
        int rejected = 0;
        for (int div = 2; div <= 4 && !rejected; div++) {
            int sub = lag / div;
            if (sub >= AC_MIN_LAG && r[sub] > 0.6f * r[lag])
                rejected = 1;
        }
        if (rejected) continue;

        if (r[lag] > best_val) {
            best_val = r[lag];
            best_lag = lag;
        }
    }

    return (best_lag > 0) ? SAMPLE_RATE / (float)best_lag : 0.0f;
}

/* -------------------------------------------------------------------------
 * Oscillator
 * ---------------------------------------------------------------------- */
static inline float osc_tick(TrumpetSynth *s, float freq_hz) {
    float phase = s->osc_phase;
    float sample;

    switch (s->p.osc_wave) {
        case 0: sample = sinf(2.0f * (float)M_PI * phase);                           break;
        case 1: sample = (phase < 0.5f) ? (4.0f*phase - 1.0f) : (3.0f - 4.0f*phase); break;
        case 2: sample = 2.0f * phase - 1.0f;                                        break;
        case 3: sample = (phase < 0.5f) ? 1.0f : -1.0f;                              break;
        default: sample = 0.0f;
    }

    s->osc_phase = phase + freq_hz / SAMPLE_RATE;
    if (s->osc_phase >= 1.0f) s->osc_phase -= 1.0f;

    return sample;
}

/* -------------------------------------------------------------------------
 * Process block
 * ---------------------------------------------------------------------- */
static void plugin_process_block(void *instance, int16_t *audio_inout, int frames) {
    TrumpetSynth *s = (TrumpetSynth *)instance;
    Params       *p = &s->p;

    /* Convert input to mono float; accumulate into hop buffer and compute RMS */
    float sum_sq = 0.0f;
    for (int i = 0; i < frames; i++) {
        float raw = audio_inout[i * 2] * p->input_gain / 32768.0f;
        if (raw >  1.0f) raw =  1.0f;
        if (raw < -1.0f) raw = -1.0f;
        sum_sq += raw * raw;
        s->hop_buf[s->hop_fill++] = (smpl_t)raw;

        /* Run pitch detection once per hop (every PITCH_HOP samples) */
        if (s->hop_fill == PITCH_HOP) {
            /* Shift rolling analysis window left, append new hop at end */
            memmove(s->pitch_buf->data,
                    s->pitch_buf->data + PITCH_HOP,
                    (PITCH_WIN - PITCH_HOP) * sizeof(smpl_t));
            memcpy(s->pitch_buf->data + (PITCH_WIN - PITCH_HOP),
                   s->hop_buf, PITCH_HOP * sizeof(smpl_t));
            s->hop_fill = 0;
            s->block_count++;

            float hz = autocorr_pitch(s->pitch_buf->data, PITCH_WIN);

            /* Only update pitch when signal is clearly present (≥ 50% of gate
             * threshold). Below that the detector loses lock and would produce
             * octave jumps as the note decays — hold last stable value instead. */
            if (hz > 0.0f && s->last_rms >= p->gate_threshold * 0.5f) {
                /* Store in circular buffer */
                s->raw_hz_buf[s->raw_hz_idx] = hz;
                s->raw_hz_idx = (s->raw_hz_idx + 1) % 5;
                if (s->raw_hz_count < 5) s->raw_hz_count++;

                if (s->onset_pending) {
                    /* First valid reading after a note attack — commit immediately */
                    s->detected_hz   = hz;
                    s->smoothed_hz   = hz;  /* also snap smoother to avoid glide-in */
                    s->onset_pending = 0;
                } else {
                    s->detected_hz = persistence_filter(
                        s->raw_hz_buf, s->raw_hz_count, s->detected_hz);
                }
            }
            /* else hz=0.0: autocorr found no valid peak — hold detected_hz unchanged */

            if (s->block_count % 10 == 0) {
                FILE *f = fopen("/data/UserData/tsyn_debug.log", "a");
                if (f) {
                    fprintf(f, "[tsyn] hz=%.1f gate=%d rms=%.4f\n",
                            hz, s->gate, sqrtf(sum_sq / (i + 1)));
                    fclose(f);
                }
            }
        }
    }
    float rms = sqrtf(sum_sq / frames);
    s->last_rms = rms;

    /* Amplitude gate: open at threshold, 20ms minimum hold, close at 30% of threshold */
    if (!s->gate && rms >= p->gate_threshold) {
        /* If the gate reopens within 200ms, snap the pitch smoother to the frozen
         * value from the previous note so tonguing doesn't scoop up from a drifted hz. */
        if (s->gate_closed_samples < (int)(SAMPLE_RATE * 0.200f) && s->frozen_hz > 0.0f) {
            s->smoothed_hz = s->frozen_hz;
            s->detected_hz = s->frozen_hz;
        }
        s->gate = 1;
        s->gate_hold_remaining   = (int)(SAMPLE_RATE * 0.020f);
        s->gate_closed_samples   = 0;
        /* Reset persistence buffer so next valid reading commits immediately */
        s->raw_hz_count  = 0;
        s->raw_hz_idx    = 0;
        s->onset_pending = 1;
    } else if (s->gate) {
        if (s->gate_hold_remaining > 0)
            s->gate_hold_remaining -= frames;
        else if (rms < p->gate_threshold * 0.3f) {
            s->gate = 0;
            s->frozen_hz          = s->smoothed_hz;  /* lock last stable pitch */
            s->gate_closed_samples = 0;
        }
    } else {
        /* Gate remains closed — count elapsed samples for the 200ms window */
        s->gate_closed_samples += frames;
    }

    /* Pitch smoother coefficient */
    const float k = 1.0f - expf(-1.0f / (SAMPLE_RATE * fmaxf(p->pitch_smooth, 1e-4f)));

    /* Synthesise — oscillator always runs to avoid phase discontinuities */
    for (int i = 0; i < frames; i++) {
        /* Only track pitch while gate is open — freeze frequency on release
         * so the smoother doesn't drift (scoop) during the gain ramp-down. */
        if (s->gate)
            s->smoothed_hz += k * (s->detected_hz - s->smoothed_hz);
        /* Ramp gate_gain toward target to eliminate onset/release clicks */
        if (s->gate) {
            s->gate_gain += GATE_ATTACK_RATE;
            if (s->gate_gain > 1.0f) s->gate_gain = 1.0f;
        } else {
            s->gate_gain -= GATE_RELEASE_RATE;
            if (s->gate_gain < 0.0f) s->gate_gain = 0.0f;
        }

        float freq     = s->smoothed_hz * powf(2.0f, p->osc_detune / 1200.0f);
        float osc      = osc_tick(s, freq) * p->osc_level * p->volume;
        float filtered = capacitor_process(&s->filter, osc, p->filter_resonance);
        int16_t sample = (int16_t)(filtered * s->gate_gain * 32767.0f);
        audio_inout[i * 2 + 0] = sample;
        audio_inout[i * 2 + 1] = sample;
    }
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
static audio_fx_api_v2_t g_plugin = {
    .api_version      = AUDIO_FX_API_VERSION_2,
    .create_instance  = plugin_create,
    .destroy_instance = plugin_destroy,
    .process_block    = plugin_process_block,
    .set_param        = plugin_set_param,
    .get_param        = plugin_get_param,
    .on_midi          = plugin_on_midi,
};

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_plugin;
}
