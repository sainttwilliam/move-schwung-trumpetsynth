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
#define GATE_ATTACK_RATE     (1.0f / (0.001f * SAMPLE_RATE))  /* 1ms open ramp         */
#define GATE_RELEASE_SAMPLES ((int)(0.008f * SAMPLE_RATE))     /* 8ms release countdown */
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

    float  raw_hz_buf[3];     /* circular buffer of last 3 processed hz */
    int    raw_hz_idx;        /* next write position                    */
    int    raw_hz_count;      /* number of valid entries (max 3)        */

    float     osc_phase;
    float     gate_gain;          /* smoothed gate: ramps 0→1 on open, 1→0 on close  */
    int       onset_pending;      /* set on gate open; cleared after first valid pitch */
    float     last_rms;           /* RMS of previous block — used for pitch freeze     */
    float     frozen_hz;          /* smoothed_hz captured at gate close                */
    int       gate_closed_samples; /* frames elapsed since gate closed                */
    int       gate_release_count;  /* countdown samples remaining in release ramp      */
    int       attack_blind_blocks; /* blocks to suppress pitch after gate open         */
    int       slur_change_count;   /* consecutive hops with >20% pitch jump            */
    int       pitch_stable_blocks; /* hops since last detected_hz commit               */
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
    p->osc_wave         = 2;
    p->osc_detune       = 0.0f;
    p->osc_level        = 0.8f;
    p->volume           = 0.8f;
    p->filter_cutoff    = 20000.0f;
    p->filter_resonance = 0.3f;
}

/* -------------------------------------------------------------------------
 * Persistence filter: requires 2-out-of-3 readings within 15% of each other.
 * Frequencies are treated as independent — no octave grouping. 349 Hz and
 * 698 Hz are different pitches and will not match. This is safe because the
 * custom autocorrelation + harmonic sieve already rejects sub-octave errors;
 * the 2:1 grouping previously caused F4/F5 slurs to lock to the lower octave.
 * Returns prev_hz unchanged if no pair agrees within 15%.
 * ---------------------------------------------------------------------- */
static float persistence_filter(const float *buf, int n, float prev_hz) {
    int   best_count = 0;
    float best_hz    = 0.0f;
    for (int i = 0; i < n; i++) {
        if (buf[i] <= 0.0f) continue;
        int   count   = 0;
        float grp_rep = buf[i];
        for (int j = 0; j < n; j++) {
            if (buf[j] <= 0.0f) continue;
            float lo    = buf[i] < buf[j] ? buf[i] : buf[j];
            float hi    = buf[i] > buf[j] ? buf[i] : buf[j];
            float ratio = hi / lo;
            int agree = (ratio <= 1.15f) ||
                        (lo < 200.0f && hi > 200.0f);
            if (agree) {
                count++;
                if (buf[j] > grp_rep) grp_rep = buf[j];
            }
        }
        if (count > best_count) { best_count = count; best_hz = grp_rep; }
    }
    return (best_count >= 2) ? best_hz : prev_hz;
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

            /* Only update pitch when: gate is open, signal is present (≥ 50% of
             * gate threshold), and attack blind period has elapsed.
             * Gate close freezes detected_hz immediately — no pitch drift on release. */
            if (hz > 0.0f && s->gate
                          && s->last_rms >= p->gate_threshold * 0.5f
                          && s->attack_blind_blocks == 0) {

                /* Step 1: double up to 6 times to bring above 180 Hz */
                for (int oct = 0; oct < 6 && hz < 180.0f; oct++) hz *= 2.0f;
                /* Step 2: halve once if above 700 Hz */
                if (hz > 700.0f) hz *= 0.5f;
                /* Step 3: flywheel — reject if candidate is ~2x from smoothed_hz
                 * (ratio within 15% of 2.0). Active only for the first 5 hops after
                 * a pitch commit; after 5 hops of stable persistence agreement the
                 * persistence filter is the final authority and flywheel is bypassed. */
                if (hz > 0.0f && s->smoothed_hz > 0.0f && s->pitch_stable_blocks < 5) {
                    float lo = hz < s->smoothed_hz ? hz : s->smoothed_hz;
                    float hi = hz > s->smoothed_hz ? hz : s->smoothed_hz;
                    float ratio = hi / lo;
                    if (ratio > 1.70f && ratio < 2.30f)
                        hz = 0.0f;
                }
                /* Step 4: accept only 155–760 Hz, else hold previous */
                if (hz < 155.0f || hz > 760.0f) hz = 0.0f;
            }

            if (hz > 0.0f && s->gate && s->attack_blind_blocks == 0) {
                /* Store in 3-slot circular buffer */
                s->raw_hz_buf[s->raw_hz_idx] = hz;
                s->raw_hz_idx = (s->raw_hz_idx + 1) % 3;
                if (s->raw_hz_count < 3) s->raw_hz_count++;

                if (s->onset_pending) {
                    /* First valid reading after a note attack — commit immediately */
                    s->detected_hz        = hz;
                    s->smoothed_hz        = hz;
                    s->onset_pending      = 0;
                    s->slur_change_count  = 0;
                    s->pitch_stable_blocks = 0;
                } else {
                    /* Slur detection: >20% deviation for 3 consecutive hops while
                     * gate stays open → snap to new pitch, reset persistence. */
                    if (s->smoothed_hz > 0.0f) {
                        float lo = hz < s->smoothed_hz ? hz : s->smoothed_hz;
                        float hi = hz > s->smoothed_hz ? hz : s->smoothed_hz;
                        if (lo > 0.0f && (hi / lo) > 1.20f) {
                            s->slur_change_count++;
                            if (s->slur_change_count >= 3) {
                                s->raw_hz_buf[0] = hz;
                                s->raw_hz_buf[1] = 0.0f;
                                s->raw_hz_buf[2] = 0.0f;
                                s->raw_hz_idx    = 1;
                                s->raw_hz_count  = 1;
                                s->detected_hz    = hz;
                                s->smoothed_hz    = hz;
                                s->frozen_hz      = 0.0f;
                                s->slur_change_count  = 0;
                                s->pitch_stable_blocks = 0;
                            }
                        } else {
                            s->slur_change_count = 0;
                        }
                    }
                    /* Persistence filter: 2-of-3 readings within 15% required */
                    float candidate = persistence_filter(
                        s->raw_hz_buf, s->raw_hz_count, s->detected_hz);
                    if (candidate != s->detected_hz) {
                        s->detected_hz         = candidate;
                        s->pitch_stable_blocks  = 0;
                    } else {
                        if (s->pitch_stable_blocks < 5) s->pitch_stable_blocks++;
                    }
                }
            }
            /* else: hold detected_hz unchanged (no peak, gate closed, or blind) */
        }
    }
    float rms = sqrtf(sum_sq / frames);
    s->last_rms = rms;

    /* Decrement attack blind period counter once per block */
    if (s->attack_blind_blocks > 0) s->attack_blind_blocks--;

    /* Amplitude gate: open at threshold, 30ms minimum hold, close at 30% of threshold */
    if (!s->gate && rms >= p->gate_threshold) {
        /* Quick re-articulation (< 50ms gap): restore frozen pitch so the oscillator
         * plays the correct pitch immediately while waiting for first onset detection.
         * Longer gaps: let onset_pending commit the first autocorr reading instead. */
        if (s->gate_closed_samples < (int)(SAMPLE_RATE * 0.050f) && s->frozen_hz > 0.0f) {
            s->smoothed_hz = s->frozen_hz;
            s->detected_hz = s->frozen_hz;
        }
        s->gate = 1;
        s->gate_hold_remaining  = (int)(SAMPLE_RATE * 0.030f);
        s->gate_closed_samples  = 0;
        s->raw_hz_count         = 0;
        s->raw_hz_idx           = 0;
        s->onset_pending        = 1;
        s->attack_blind_blocks  = 3;
        s->slur_change_count    = 0;
    } else if (s->gate) {
        if (s->gate_hold_remaining > 0)
            s->gate_hold_remaining -= frames;
        else if (rms < p->gate_threshold * 0.3f) {
            s->gate = 0;
            s->frozen_hz           = s->smoothed_hz;
            s->gate_closed_samples = 0;
        }
    } else {
        s->gate_closed_samples += frames;
    }

    /* Pitch smoother coefficient */
    const float k = 1.0f - expf(-1.0f / (SAMPLE_RATE * fmaxf(p->pitch_smooth, 1e-4f)));

    /* Synthesise — oscillator always runs to avoid phase discontinuities */
    for (int i = 0; i < frames; i++) {
        /* Smoother only runs while gate is open and past the onset commit */
        if (s->gate && !s->onset_pending)
            s->smoothed_hz += k * (s->detected_hz - s->smoothed_hz);

        /* Gate gain: 1ms linear attack; 8ms exact countdown release.
         * Ramps immediately on gate open regardless of attack blind period —
         * volume responds instantly while pitch detection is still suppressed. */
        if (s->gate) {
            s->gate_gain += GATE_ATTACK_RATE;
            if (s->gate_gain > 1.0f) s->gate_gain = 1.0f;
            s->gate_release_count = GATE_RELEASE_SAMPLES;
        } else if (s->gate_release_count > 0) {
            s->gate_release_count--;
            s->gate_gain = (float)s->gate_release_count / (float)GATE_RELEASE_SAMPLES;
        } else {
            s->gate_gain = 0.0f;
        }

        float freq     = s->smoothed_hz * powf(2.0f, p->osc_detune / 1200.0f);
        float osc      = osc_tick(s, freq) * p->osc_level * p->volume;
        float filtered = capacitor_process(&s->filter, osc, p->filter_resonance);

        /* tanh soft limiter before int16 conversion — prevents clipping from
         * filter resonance overshoot while preserving transient character */
        float out = tanhf(filtered * 1.2f) * 0.85f;
        out *= s->gate_gain;

        int16_t sample = (int16_t)(out * 32767.0f);
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
