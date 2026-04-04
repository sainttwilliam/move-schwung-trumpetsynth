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
#include "dattorro_filter.h"

#define SAMPLE_RATE  44100.0f
#define BLOCK_SIZE   128
#define PITCH_WIN    2048   /* analysis window (~46ms at 44100 Hz)                       */
#define PITCH_HOP    512    /* new samples per pitch detection call                       */
#define GATE_ATTACK_RATE     (1.0f / (0.001f * SAMPLE_RATE))  /* 1ms open ramp         */
#define GATE_RELEASE_SAMPLES ((int)(0.008f * SAMPLE_RATE))     /* 8ms release countdown */
#define AC_MIN_LAG   28     /* 44100 / 1575 Hz ≈ 28.0 — first lag above 1575 Hz          */
#define AC_MAX_LAG   294    /* 44100 /  150 Hz = 294.0 — last lag above 150 Hz           */

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
    float vol_tracking;      /* 0=flat, 1=RMS-tracks input dynamics    */
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
    float     last_raw_hz;         /* raw autocorr result before any correction        */
    int       log_committed;       /* 1 if detected_hz changed this block              */
    float     vol_rms_smooth;      /* 15ms smoothed RMS for volume tracking            */
    DattorroFilter filter;

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
    p->gate_threshold   = 0.015f; /* RMS ~-36 dBFS — tuned for Roland Silent Brass levels */
    p->osc_wave         = 2;
    p->osc_detune       = 0.0f;
    p->osc_level        = 0.8f;
    p->volume           = 0.8f;
    p->filter_cutoff    = 20000.0f;
    p->filter_resonance = 0.3f;
    p->vol_tracking     = 0.8f;
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
            int agree = (ratio <= 1.15f);
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
    dattorro_init(&s->filter);
    dattorro_set_params(&s->filter, s->p.filter_cutoff, s->p.filter_resonance, SAMPLE_RATE);

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
        dattorro_set_params(&s->filter, p->filter_cutoff, p->filter_resonance, SAMPLE_RATE);
    }
    else if (strcmp(key, "filter_resonance") == 0) {
        p->filter_resonance = parse_float(val, p->filter_resonance);
        dattorro_set_params(&s->filter, p->filter_cutoff, p->filter_resonance, SAMPLE_RATE);
    }
    else if (strcmp(key, "vol_tracking")     == 0) p->vol_tracking     = parse_float(val, p->vol_tracking);
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
    else if (strcmp(key, "vol_tracking")     == 0) return snprintf(buf, buf_len, "%.4f", p->vol_tracking);
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
static float autocorr_pitch(const smpl_t *buf, int win, float rms) {
    float r0 = 0.0f;
    for (int i = 0; i < win; i++) r0 += buf[i] * buf[i];
    if (r0 < 1e-6f) return 0.0f;

    /* Autocorrelation for lags AC_MIN_LAG-1 through AC_MAX_LAG.
     * One lag below AC_MIN_LAG is computed so the local-maximum check
     * at the first candidate lag has a valid left neighbour. */
    float r[AC_MAX_LAG + 1];
    memset(r, 0, sizeof(r));
    for (int lag = AC_MIN_LAG - 1; lag <= AC_MAX_LAG; lag++) {
        float sum = 0.0f;
        int   n   = win - lag;
        for (int i = 0; i < n; i++) sum += buf[i] * buf[i + lag];
        r[lag] = sum / r0;
    }

    /* Signal level through Roland Silent Brass is consistently 0.02–0.05 RMS —
     * at these levels the ratio between fundamental and harmonic peaks is less
     * pronounced, so use a strict sieve: a sub-peak needs only 50% of the
     * candidate's strength to trigger rejection. */
    float sieve_thresh = 0.5f;
    (void)rms; /* sieve_thresh is now fixed; rms kept for gate guard above */

    float best_val = 0.1f;  /* minimum normalised autocorrelation to accept */
    int   best_lag = 0;

    for (int lag = AC_MIN_LAG; lag < AC_MAX_LAG; lag++) {
        /* Local maximum */
        if (r[lag] <= r[lag - 1] || r[lag] <= r[lag + 1]) continue;

        /* Harmonic sieve: prefer fundamentals over harmonics.
         * If a longer lag (lower frequency) — lag*2, lag*3, or lag*4 —
         * has a strong peak within the search range, this candidate is
         * a harmonic of that lower note. Skip it so the fundamental wins.
         * Example: lag 62 (F5=711Hz) is skipped when lag 124 (F4=356Hz)
         * is strong, allowing lag 124 to win as the true fundamental. */
        int rejected = 0;
        for (int mul = 2; mul <= 4 && !rejected; mul++) {
            int longer = lag * mul;
            if (longer <= AC_MAX_LAG && r[longer] > sieve_thresh * r[lag])
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

    s->log_committed = 0;

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

            float hz = autocorr_pitch(s->pitch_buf->data, PITCH_WIN, s->last_rms);
            s->last_raw_hz = hz;

            /* Only update pitch when: gate is open, signal is present (≥ 50% of
             * gate threshold), and attack blind period has elapsed.
             * Gate close freezes detected_hz immediately — no pitch drift on release. */
            if (hz > 0.0f && s->gate
                          && s->last_rms >= p->gate_threshold * 0.5f
                          && s->attack_blind_blocks == 0) {

                /* Store RAW autocorrelation value in the persistence buffer.
                 * Octave correction is applied AFTER persistence commits —
                 * this keeps the 3-slot buffer free of artificially doubled
                 * values that would otherwise corrupt the 2-of-3 matching. */
                s->raw_hz_buf[s->raw_hz_idx] = hz;
                s->raw_hz_idx = (s->raw_hz_idx + 1) % 3;
                if (s->raw_hz_count < 3) s->raw_hz_count++;

                /* Compute octave-corrected value for this single hop so that
                 * slur detection and onset commits compare on the same scale
                 * as smoothed_hz (which is always a corrected, committed value). */
                /* Octave correction: only double values genuinely below the valid
                 * range (<155Hz). Values already in 155–760Hz are used directly —
                 * the sieve has already selected the correct fundamental. */
                float hz_corr = hz;
                while (hz_corr > 0.0f && hz_corr < 155.0f) hz_corr *= 2.0f;
                if (hz_corr > 760.0f) hz_corr *= 0.5f;
                int hz_corr_valid = (hz_corr >= 155.0f && hz_corr <= 760.0f);

                if (s->onset_pending) {
                    /* First valid reading after a note attack — commit immediately. */
                    if (hz_corr_valid) {
                        s->detected_hz       = hz_corr;
                        s->smoothed_hz       = hz_corr;
                        s->onset_pending     = 0;
                        s->slur_change_count = 0;
                        s->log_committed     = 1;
                    }
                } else {
                    /* Slur detection: compare corrected per-hop value against
                     * smoothed_hz (which is always corrected). >20% deviation
                     * for 3 consecutive hops → snap to new pitch, reset buffer. */
                    if (s->smoothed_hz > 0.0f && hz_corr_valid) {
                        float lo = hz_corr < s->smoothed_hz ? hz_corr : s->smoothed_hz;
                        float hi = hz_corr > s->smoothed_hz ? hz_corr : s->smoothed_hz;
                        if (lo > 0.0f && (hi / lo) > 1.20f) {
                            s->slur_change_count++;
                            if (s->slur_change_count >= 3) {
                                /* Reset buffer to just this raw reading */
                                s->raw_hz_buf[0] = hz;
                                s->raw_hz_buf[1] = 0.0f;
                                s->raw_hz_buf[2] = 0.0f;
                                s->raw_hz_idx    = 1;
                                s->raw_hz_count  = 1;
                                s->detected_hz       = hz_corr;
                                s->smoothed_hz       = hz_corr;
                                s->frozen_hz         = 0.0f;
                                s->slur_change_count = 0;
                                s->log_committed     = 1;
                            }
                        } else {
                            s->slur_change_count = 0;
                        }
                    }

                    /* Persistence filter: 2-of-3 raw readings must agree within 15%. */
                    float raw_candidate = persistence_filter(
                        s->raw_hz_buf, s->raw_hz_count, s->detected_hz);

                    /* Octave correction on the committed raw value. Only double
                     * values below 155Hz — if the sieve returned a value already
                     * in the valid range, use it directly without doubling. */
                    float candidate = s->detected_hz;
                    if (raw_candidate > 0.0f) {
                        float c = raw_candidate;
                        while (c > 0.0f && c < 155.0f) c *= 2.0f;
                        if (c > 760.0f) c *= 0.5f;
                        if (c >= 155.0f && c <= 760.0f) candidate = c;
                    }

                    /* Commit if corrected candidate differs from current pitch by > 1%. */
                    if (s->detected_hz == 0.0f ||
                            fabsf(candidate - s->detected_hz) / s->detected_hz > 0.01f) {
                        s->detected_hz   = candidate;
                        s->smoothed_hz   = candidate;
                        s->log_committed = 1;
                    }
                }
            }
            /* else: hold detected_hz unchanged (no peak, gate closed, or blind) */
        }
    }
    float rms = sqrtf(sum_sq / frames);
    s->last_rms = rms;

    /* Volume tracking: smooth RMS at 15ms, apply power 1.5 curve.
     * dyn_vol blends flat volume (vol_tracking=0) with RMS-tracked level
     * (vol_tracking=1) so soft playing is quieter and loud is full volume.
     * Reference level 0.10 RMS = forte — normalised to [0, 1]. */
    {
        float k_vol = 1.0f - expf(-(float)frames / (0.015f * SAMPLE_RATE));
        s->vol_rms_smooth += k_vol * (rms - s->vol_rms_smooth);
    }
    float vol_norm = s->vol_rms_smooth / 0.10f;
    if (vol_norm > 1.0f) vol_norm = 1.0f;
    float dyn_vol = p->volume * (1.0f - p->vol_tracking
                                + p->vol_tracking * powf(vol_norm, 1.5f));

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

    /* Debug log — every block while gate is open */
    if (s->gate) {
        FILE *dbg = fopen("/data/UserData/tsyn_debug.log", "a");
        if (dbg) {
            fprintf(dbg,
                "[tsyn] raw=%.1f buf=[%.1f,%.1f,%.1f] smooth=%.1f det=%.1f"
                " rms=%.4f commit=%d blind=%d\n",
                s->last_raw_hz,
                s->raw_hz_buf[0], s->raw_hz_buf[1], s->raw_hz_buf[2],
                s->smoothed_hz, s->detected_hz,
                rms, s->log_committed, s->attack_blind_blocks);
            fclose(dbg);
        }
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
        float osc      = osc_tick(s, freq) * p->osc_level * dyn_vol;
        float filtered = dattorro_process(&s->filter, osc);

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
