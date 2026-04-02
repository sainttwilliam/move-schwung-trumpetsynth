/* capacitor.h — Airwindows Capacitor lowpass filter, ported to C
 *
 * Original algorithm by Chris Johnson (Airwindows), MIT licence.
 * https://github.com/airwindows/airwindows
 *
 * The Capacitor uses a "gearbox" scheme: rather than applying the same
 * 3-pole cascade every sample, it cycles through 6 different pole-pair
 * combinations (sharing pole A as the first stage each time, then rotating
 * through B/C and D/E/F for the second and third stages). This spreads the
 * phase artefacts and produces a softer, more analog-like response than a
 * fixed 3-pole IIR.
 *
 * Resonance feedback (not in the original): the first pole state (iirLP_A)
 * is fed back into the input each sample, creating a peak at the cutoff
 * frequency. A resonance value of 1.0 approaches self-oscillation.
 */
#ifndef CAPACITOR_H
#define CAPACITOR_H

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double iirLP_A, iirLP_B, iirLP_C, iirLP_D, iirLP_E, iirLP_F;
    int    count;
    double lpAmount;  /* smoothed one-pole coefficient                  */
    double lpChase;   /* target coefficient (set from cutoff Hz)        */
} Capacitor;

static inline void capacitor_init(Capacitor *c) {
    memset(c, 0, sizeof(*c));
}

/* Compute and store the target coefficient from a cutoff in Hz.
 * Call once at init and again whenever the cutoff parameter changes. */
static inline void capacitor_set_cutoff(Capacitor *c, float hz, float sr) {
    double lp = 1.0 - exp(-2.0 * M_PI * (double)hz / (double)sr);
    c->lpChase = lp < 0.0 ? 0.0 : lp > 1.0 ? 1.0 : lp;
}

/* Process one mono sample.
 * resonance: 0.0 = no resonance, 1.0 = near self-oscillation. */
static inline float capacitor_process(Capacitor *c, float input, float resonance) {
    /* Resonance: feed first-pole output back into the input.
     * Scale capped at 0.9 so invLP + lp*fb < 1 across all cutoff values. */
    double x = (double)input + c->iirLP_A * (double)resonance * 0.9;

    /* Per-sample coefficient smoothing (Airwindows speed-adaptive scheme) */
    double speed = 300.0 / (fabs(c->lpAmount - c->lpChase) + 1.0);
    c->lpAmount = ((c->lpAmount * speed) + c->lpChase) / (speed + 1.0);
    double inv = 1.0 - c->lpAmount;

    /* Gearbox: 6 rotating pole-pair combinations */
    if (++c->count > 5) c->count = 0;
    switch (c->count) {
        case 0:
            c->iirLP_A = (c->iirLP_A * inv) + (x * c->lpAmount); x = c->iirLP_A;
            c->iirLP_B = (c->iirLP_B * inv) + (x * c->lpAmount); x = c->iirLP_B;
            c->iirLP_D = (c->iirLP_D * inv) + (x * c->lpAmount); x = c->iirLP_D;
            break;
        case 1:
            c->iirLP_A = (c->iirLP_A * inv) + (x * c->lpAmount); x = c->iirLP_A;
            c->iirLP_C = (c->iirLP_C * inv) + (x * c->lpAmount); x = c->iirLP_C;
            c->iirLP_E = (c->iirLP_E * inv) + (x * c->lpAmount); x = c->iirLP_E;
            break;
        case 2:
            c->iirLP_A = (c->iirLP_A * inv) + (x * c->lpAmount); x = c->iirLP_A;
            c->iirLP_B = (c->iirLP_B * inv) + (x * c->lpAmount); x = c->iirLP_B;
            c->iirLP_F = (c->iirLP_F * inv) + (x * c->lpAmount); x = c->iirLP_F;
            break;
        case 3:
            c->iirLP_A = (c->iirLP_A * inv) + (x * c->lpAmount); x = c->iirLP_A;
            c->iirLP_C = (c->iirLP_C * inv) + (x * c->lpAmount); x = c->iirLP_C;
            c->iirLP_D = (c->iirLP_D * inv) + (x * c->lpAmount); x = c->iirLP_D;
            break;
        case 4:
            c->iirLP_A = (c->iirLP_A * inv) + (x * c->lpAmount); x = c->iirLP_A;
            c->iirLP_B = (c->iirLP_B * inv) + (x * c->lpAmount); x = c->iirLP_B;
            c->iirLP_E = (c->iirLP_E * inv) + (x * c->lpAmount); x = c->iirLP_E;
            break;
        case 5:
            c->iirLP_A = (c->iirLP_A * inv) + (x * c->lpAmount); x = c->iirLP_A;
            c->iirLP_C = (c->iirLP_C * inv) + (x * c->lpAmount); x = c->iirLP_C;
            c->iirLP_F = (c->iirLP_F * inv) + (x * c->lpAmount); x = c->iirLP_F;
            break;
    }

    return (float)x;
}

#endif /* CAPACITOR_H */
