/* dattorro_filter.h — Airwindows Dattorro State Variable Filter, ported to C
 *
 * Original algorithm by Chris Johnson (Airwindows), MIT licence.
 * https://github.com/airwindows/airwindows
 *
 * The Dattorro is a 2-integrator State Variable Filter with a mild nonlinearity
 * on the output: the bandpass integrator is passed through sin(band*0.5) before
 * being subtracted from the lowpass output. This produces a smooth, slightly
 * warm character rather than a clinical brickwall response.
 *
 * Per-sample equations (from airwindows Dattorro.cpp):
 *   low  += freq * band
 *   band += freq * (reso*input - low - reso*band)
 *   out   = (low - sin(band*0.5)) * gain
 *
 * freq = 2 * sin(π * cutoff_hz / sr)   — standard SVF coefficient, clamped ≤ 1.0
 * reso = pow(1 - resonance, 2)          — damping (low reso = resonant), floor 0.001
 * gain = 1 / sqrt(reso)                 — compensates level at resonance
 */
#ifndef DATTORRO_FILTER_H
#define DATTORRO_FILTER_H

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double low;   /* lowpass integrator state                  */
    double band;  /* bandpass integrator state                 */
    double freq;  /* SVF frequency coefficient (0..1)          */
    double reso;  /* damping term — low value = high resonance */
    double gain;  /* output gain compensation (1/sqrt(reso))   */
} DattorroFilter;

static inline void dattorro_init(DattorroFilter *f) {
    memset(f, 0, sizeof(*f));
    f->freq = 0.5;
    f->reso = 1.0;
    f->gain = 1.0;
}

/* Call whenever filter_cutoff or filter_resonance changes.
 * cutoff_hz : target lowpass frequency in Hz
 * resonance  : 0.0 = no resonance (fully damped), 1.0 = near self-oscillation
 * sr         : sample rate in Hz */
static inline void dattorro_set_params(DattorroFilter *f,
                                       float cutoff_hz,
                                       float resonance,
                                       float sr) {
    double fc = (double)cutoff_hz;
    if (fc < 20.0)        fc = 20.0;
    if (fc > sr * 0.45)   fc = sr * 0.45;  /* stay well below Nyquist for stability */

    f->freq = 2.0 * sin(M_PI * fc / (double)sr);
    if (f->freq > 1.0) f->freq = 1.0;

    /* reso = pow(1 - resonance, 2): at resonance=0, reso=1 (fully damped);
     * at resonance=1, reso→0 (undamped). Floor at 0.001. */
    f->reso = pow(1.0 - (double)resonance, 2.0);
    if (f->reso < 0.001) f->reso = 0.001;

    f->gain = 1.0 / sqrt(f->reso);
}

/* Process one mono sample through the Dattorro SVF lowpass. */
static inline float dattorro_process(DattorroFilter *f, float input) {
    double x = (double)input;

    f->low  += f->freq * f->band;
    f->band += f->freq * (f->reso * x - f->low - f->reso * f->band);

    /* Airwindows nonlinearity: sin(band*0.5) softens the resonance peak */
    double out = (f->low - sin(f->band * 0.5)) * f->gain;

    /* Soft clip to keep output bounded */
    if      (out >  1.0) out =  1.0;
    else if (out < -1.0) out = -1.0;

    return (float)out;
}

#endif /* DATTORRO_FILTER_H */
