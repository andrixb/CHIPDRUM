// Pure helpers for MARBLE, exposed for host tests.
#pragma once
#include <stdint.h>

// Map a value-set index to an 8-bit DAC level over RES quantization steps.
// idx is wrapped into [0, RES); RES >= 2.
static inline uint8_t marble_cv_u8(int res, int idx) {
    if (res < 2) res = 2;
    idx %= res;
    if (idx < 0) idx += res;
    return (uint8_t)((idx * 255 + (res - 1) / 2) / (res - 1));
}
