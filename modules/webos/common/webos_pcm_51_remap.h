#pragma once

#include <stdint.h>

/**
 * Moonlight / Opus decoder PCM (SDL / Vorbis):
 *   0 FL(E), 1 FR(D), 2 C, 3 LFE(S), 4 RL(PE), 5 RR(PD)
 *
 * Device order (LG soundbar / Starfish 6ch), LFE last — validated on-device:
 *   0 E, 1 PD, 2 D, 3 PE, 4 C, 5 S
 *
 * Unity gain on all channels (PE ×2.5 reduced quality without a usable level fix).
 *
 * Do NOT also send host surroundParams "642014523".
 */
static inline void SS4S_WebOS_RemapPcm51ToDevice(const int16_t *in, int16_t *out, int frames) {
    for (int f = 0; f < frames; f++) {
        const int16_t *s = in + f * 6;
        int16_t *d = out + f * 6;
        d[0] = s[0]; /* E  (FL) */
        d[1] = s[5]; /* PD (RR) */
        d[2] = s[1]; /* D  (FR) */
        d[3] = s[4]; /* PE (RL) */
        d[4] = s[2]; /* C  (FC) */
        d[5] = s[3]; /* S  (LFE) */
    }
}
