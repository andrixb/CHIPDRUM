// =============================================================================
// REEL - live-recorded loop buffer + 3 gene playback heads
// =============================================================================
#include "reel.h"

#include <string.h>

#include "config.h"

static uint8_t s_reel[REEL_LEN]; // .bss, silence = 128

static bool s_enabled = false;
static bool s_rec = false;
static uint32_t s_wptr = 0;
static int s_sos_q8 = 256;       // 0..256
static int32_t s_dc = 2048;      // slow input-bias tracker, 12-bit domain
static int s_fx = REEL_FX_OFF;
static int s_fx_amt = 0;
static int32_t s_lp[REEL_NHEADS] = {0, 0, 0}; // per-head tilt state (signed)
static uint16_t s_hold_ctr[REEL_NHEADS] = {0, 0, 0};

typedef struct {
    uint32_t pos;   // 16.16 position (integer part = sample index space)
    int32_t step;   // 16.16 signed increment
    uint32_t start; // loop window, sample indices
    uint32_t len;
    uint16_t gain;  // 0..256
    bool active;
} head_t;

static head_t s_head[REEL_NHEADS];

int32_t reel_mix_signed(int32_t old_s, int32_t in_s, int sos_q8) {
    // sos_q8 = weight of the NEW input, 0..256.
    int32_t m = old_s * (256 - sos_q8) + in_s * sos_q8;
    m >>= 8;
    if (m > 127) m = 127;
    if (m < -128) m = -128;
    return m;
}

uint8_t reel_interp_u8(uint8_t a, uint8_t b, uint16_t frac_q16) {
    int32_t d = (int32_t)b - (int32_t)a;
    int32_t v = (int32_t)a + ((d * (int32_t)frac_q16) >> 16);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

void reel_reset(void) {
    memset(s_reel, 128, sizeof(s_reel));
    s_wptr = 0;
    s_rec = false;
    s_sos_q8 = 256;
    s_dc = 2048;
    s_fx = REEL_FX_OFF;
    s_fx_amt = 0;
    for (int i = 0; i < REEL_NHEADS; i++) {
        s_lp[i] = 0;
        s_hold_ctr[i] = 0;
        s_head[i].start = 0;
        s_head[i].len = REEL_LEN;
        s_head[i].pos = 0;
        s_head[i].step = 65536;
        s_head[i].gain = (i == 0) ? 256 : 0;
        s_head[i].active = true;
    }
}

void reel_enable(bool on) { s_enabled = on; }
bool reel_active(void) { return s_enabled; }

void reel_set_rec(bool on) { s_rec = on; }
bool reel_recording(void) { return s_rec; }

void reel_set_sos_pct(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_sos_q8 = (pct * 256 + 50) / 100;
}

void reel_set_fx(int fx, int amt_pct) {
    if (fx < 0) fx = 0;
    if (fx > 2) fx = 2;
    if (amt_pct < 0) amt_pct = 0;
    if (amt_pct > 100) amt_pct = 100;
    s_fx = fx;
    s_fx_amt = amt_pct;
}

void reel_capture_sample(uint16_t adc12) {
    if (!s_enabled || !s_rec) return;
    if (adc12 > 4095) adc12 = 4095;
    // Track input bias slowly (~40 ms time constant at 25 kHz).
    s_dc += ((int32_t)adc12 - s_dc) >> 10;
    int32_t c = (int32_t)adc12 - s_dc; // signed signal around 0
    // Fixed input gain x0.25 (>>2): +/-512 LSB maps to +/-128. Clamp.
    int32_t in_s = c >> 2;
    if (in_s > 127) in_s = 127;
    if (in_s < -128) in_s = -128;
    // Overdub mix against stored sample.
    uint32_t w = s_wptr;
    if (w >= REEL_LEN) w = 0;
    int32_t old_s = (int32_t)s_reel[w] - 128;
    int32_t m = reel_mix_signed(old_s, in_s, s_sos_q8);
    s_reel[w] = (uint8_t)(m + 128);
    s_wptr++;
    if (s_wptr >= REEL_LEN) s_wptr = 0;
}

void reel_head_setup(int head, uint32_t start, uint32_t len,
                     int32_t step_q16, uint16_t gain_q8) {
    if (head < 0 || head >= REEL_NHEADS) return;
    if (len < 64) len = 64;
    if (start >= REEL_LEN) start = 0;
    if (start + len > REEL_LEN) len = REEL_LEN - start;
    if (gain_q8 > 256) gain_q8 = 256;
    head_t *h = &s_head[head];
    h->start = start;
    h->len = len;
    h->step = step_q16;
    h->gain = gain_q8;
    h->active = true;
    // Park at window start (forward) or end (reverse) so the next ISR tick
    // plays from the splice edge, not mid-gene.
    h->pos = (step_q16 >= 0) ? (start << 16) : ((start + len - 1) << 16);
    s_lp[head] = 0;
    s_hold_ctr[head] = 0;
}

void reel_head_gain(int head, uint16_t gain_q8) {
    if (head < 0 || head >= REEL_NHEADS) return;
    if (gain_q8 > 256) gain_q8 = 256;
    s_head[head].gain = gain_q8;
}

void reel_head_retrigger(int head, uint32_t start) {
    if (head < 0 || head >= REEL_NHEADS) return;
    head_t *h = &s_head[head];
    if (start >= REEL_LEN) start = 0;
    if (start + h->len > REEL_LEN) start = REEL_LEN - h->len;
    h->pos = (h->step >= 0) ? (start << 16) : ((start + h->len - 1) << 16);
}

uint16_t reel_render_level(int head) {
    head_t *h = &s_head[head];
    // Window wrap (cheap branch version; len >= 64 always).
    uint32_t idx = h->pos >> 16;
    if (idx < h->start || idx >= h->start + h->len) {
        // Wrapped past an edge: fold back into the window.
        uint32_t rel;
        if (h->step >= 0) {
            rel = (idx >= h->start) ? (idx - h->start) % h->len : 0;
        } else {
            rel = h->len - 1;
        }
        idx = h->start + rel;
        h->pos = idx << 16;
    }
    uint32_t idx2 = idx + 1;
    if (idx2 >= h->start + h->len) idx2 = h->start;
    uint16_t frac = (uint16_t)(h->pos & 0xFFFFu);
    uint8_t s = reel_interp_u8(s_reel[idx], s_reel[idx2], frac);
    h->pos = (uint32_t)((int32_t)h->pos + h->step);

    int32_t c = (int32_t)s - 128;
    c = (c * (int32_t)h->gain) >> 8;

    // Feedforward FX only (no feedback delay in v1: unconditionally stable).
    if (s_fx == REEL_FX_CRUSH && s_fx_amt > 0) {
        // amt 0..100 -> keep top 8..2 bits.
        int bits = 8 - (s_fx_amt * 6 + 50) / 100;
        if (bits < 2) bits = 2;
        if (bits < 8) {
            int32_t mask = ~((1 << (8 - bits)) - 1);
            // Work in unsigned domain for the bit mask, then recenter.
            int32_t u = c + 128;
            u &= mask;
            c = u - 128;
        }
        // Decimator hold: repeat sample N times (1..6).
        int hold = 1 + (s_fx_amt * 5) / 100;
        static int32_t held[REEL_NHEADS] = {0, 0, 0};
        if ((s_hold_ctr[head]++ % (uint16_t)hold) == 0) held[head] = c;
        c = held[head];
    } else if (s_fx == REEL_FX_TILT && s_fx_amt > 0) {
        // amt 0..50 = lowpass darker, 51..100 = highpass brighter (fixed tilt).
        // One 1-pole section: y += a*(x-y), a from amt. Integer, stable.
        int32_t a = (s_fx_amt <= 50) ? (s_fx_amt * 256 / 50)      // 0..256 LP
                                      : ((100 - s_fx_amt) * 256 / 50); // HP-ish
        if (a < 8) a = 8;
        if (a > 256) a = 256;
        s_lp[head] += ((c - s_lp[head]) * a) >> 8;
        if (s_fx_amt <= 50) {
            c = s_lp[head];
        } else {
            c = c + (c - s_lp[head]) / 2; // presence lift, still bounded
            if (c > 127) c = 127;
            if (c < -128) c = -128;
        }
    }

    uint8_t out = (uint8_t)(c + 128);
    return (uint16_t)(((uint32_t)out * DAC_STEPS) >> 8);
}

uint32_t reel_length(void) { return REEL_LEN; }
uint8_t reel_peek(uint32_t idx) { return (idx < REEL_LEN) ? s_reel[idx] : 128; }
uint32_t reel_write_pos(void) { return s_wptr; }
uint32_t reel_head_pos(int head) {
    if (head < 0 || head >= REEL_NHEADS) return 0;
    return s_head[head].pos >> 16;
}
uint32_t reel_splice_start(int i) {
    if (i < 0) i = 0;
    if (i >= REEL_NSPLICE) i = REEL_NSPLICE - 1;
    return ((uint32_t)i * REEL_LEN) / REEL_NSPLICE;
}
uint32_t reel_splice_len(int i) {
    (void)i;
    return REEL_LEN / REEL_NSPLICE;
}
