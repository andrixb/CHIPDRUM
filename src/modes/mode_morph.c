// =============================================================================
// MORPH - live-reel Morphagene-style mode (low-risk v1)
// =============================================================================
// Panel (no buttons.c change: we reuse the existing callbacks):
//   EXTRA2 short (ACT_RESTART -> restart())  = REC punch in/out
//   EXTRA2 hold  (ACT_RANDOMIZE -> randomize()) = random splice next gene
//   EXTRA1 short = manual clock tick (existing): manual gene retrig
//   UP/DOWN = slot, LEFT/RIGHT = adjust (existing)
// CV1 (macro, already smoothed in main.c) = MORPH crossfade H1<->H2 live.
// Slider = tempo / ext-clock divide-multiply (existing clockgen, untouched).
#include <math.h>
#include <stdio.h>

#include "../gfx.h"
#include "../mode.h"
#include "../reel.h"
#include "../rng.h"
#include "../sampler.h"
#include "mode_util.h"

static int16_t p_splice = 0; // 0..7
static int16_t p_size = 240; // gene size, ms 40..800
static int16_t p_speed = 0;  // semitones -12..+12 (neg = reverse)
static int16_t p_sos = 100; // overdub SOS %
static int16_t p_fx = 0;     // 0 off, 1 crush, 2 tilt
static int16_t p_fxamt = 0;  // 0..100

static void fmt_ms(char *out, int16_t v) { sprintf(out, "%dms", v); }
static void fmt_semi(char *out, int16_t v) { sprintf(out, "%+d", v); }
static void fmt_pct(char *out, int16_t v) { sprintf(out, "%d%%", v); }
static void fmt_fx(char *out, int16_t v) {
    static const char *const N[3] = {"OFF", "CRUSH", "TILT"};
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    sprintf(out, "%s", N[v]);
}

static const param_t PARAMS[] = {
    {"SPLICE", &p_splice, 0, 7, 1, 0},
    {"SIZE", &p_size, 40, 800, 20, fmt_ms},
    {"SPEED", &p_speed, -12, 12, 1, fmt_semi},
    {"SOS", &p_sos, 0, 100, 5, fmt_pct},
    {"FX", &p_fx, 0, 2, 1, fmt_fx},
    {"FXAMT", &p_fxamt, 0, 100, 5, fmt_pct},
};

static int32_t speed_to_step(int16_t st, bool *rev) {
    int mag = (st < 0) ? -st : st;
    float rate = powf(2.0f, mag / 12.0f);
    int32_t step = (int32_t)(rate * 65536.0f);
    if (step < 4096) step = 4096;
    if (step > (16 << 16)) step = (16 << 16);
    *rev = (st < 0);
    return *rev ? -step : step;
}

static void apply_gene(void) {
    uint32_t s0 = reel_splice_start(p_splice);
    uint32_t s1 = reel_splice_start((p_splice + 1) % 8);
    uint32_t want = (uint32_t)p_size * 25u; // ms -> samples @25 kHz
    uint32_t maxl = reel_splice_len(0);
    if (want > maxl) want = maxl;
    if (want < 64) want = 64;
    bool rev;
    int32_t step = speed_to_step(p_speed, &rev);
    // Morph crossfade is applied live in update(); park both heads here.
    reel_head_setup(0, s0, want, step, 256);
    reel_head_setup(1, s1, want, step, 0);
    // H2: half-speed shadow of the same splice, quiet bed (Morphagene-style).
    int32_t half = step / 2;
    if (half == 0) half = (step >= 0) ? 1 : -1;
    reel_head_setup(2, s0, want, half, 128);
    reel_set_sos_pct(p_sos);
    reel_set_fx(p_fx, p_fxamt);
}

static void reset(void) {
    for (int v = 0; v < 3; v++) {
        sampler_enable(v, true);
        sampler_set_synth(v, false);
    }
    reel_reset();
    reel_enable(true);
    reel_set_rec(false);
    apply_gene();
}

static void on_tick(uint64_t now) {
    (void)now;
    // Clock-quantized gene retrig: park heads at splice edges each 16th.
    // Cheap: only re-park positions, keep gains (update() owns gains).
    reel_head_retrigger(0, reel_splice_start(p_splice));
    reel_head_retrigger(1, reel_splice_start((p_splice + 1) % 8));
    reel_head_retrigger(2, reel_splice_start(p_splice));
}

static void update(float dt_s, uint64_t now) {
    (void)dt_s;
    (void)now;
    // Live MORPH from the already-smoothed macro CV (no ADC change).
    float m = g.cv1;
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    reel_head_gain(0, (uint16_t)(256.0f * (1.0f - m)));
    reel_head_gain(1, (uint16_t)(256.0f * m));
    // Params edited via LEFT/RIGHT need no tick to take effect except geometry.
    reel_set_sos_pct(p_sos);
    reel_set_fx(p_fx, p_fxamt);
}

static void restart(void) {
    // EXTRA2 short press: REC punch in/out. No buttons.c change.
    reel_set_rec(!reel_recording());
}

static void randomize(int16_t *x, int16_t *y) {
    // EXTRA2 2 s hold: jump to a random splice next gene.
    p_splice = (int16_t)rng_below(8);
    if (x) *x = p_splice;
    if (y) *y = p_splice;
    apply_gene();
}

static void draw(void) {
    // Waveform overview, decimated to 128 px (reads only; tearing cosmetic).
    for (int x = 0; x < 128; x += 2) {
        uint32_t idx = ((uint32_t)x * reel_length()) / 128u;
        uint8_t s = reel_peek(idx);
        int h = ((int)s - 128) * 20 / 128; // +/-10 px
        int y0 = 32 - h;
        int y1 = 32 + (h < 0 ? -h : 0);
        if (y0 < 12) y0 = 12;
        if (y1 > 50) y1 = 50;
        gfx_pixel(x, 32 + ((int)s - 128) * 20 / 128, 1);
        (void)y0;
        (void)y1;
    }
    // Splice ticks.
    for (int i = 0; i < 8; i++) {
        int x = (int)(((uint64_t)reel_splice_start(i) * 128) / reel_length());
        gfx_pixel(x, 12, 1);
        gfx_pixel(x, 13, 1);
    }
    // Write head + 3 read heads.
    int wx = (int)(((uint64_t)reel_write_pos() * 128) / reel_length());
    gfx_pixel(wx, 50, 1);
    for (int h = 0; h < 3; h++) {
        int hx = (int)(((uint64_t)reel_head_pos(h) * 128) / reel_length());
        gfx_pixel(hx, 20 + h * 6, 1);
    }
    char buf[20];
    if (reel_recording())
        gfx_text(100, 14, "REC", 1);
    sprintf(buf, "SP%d %dms", p_splice, p_size);
    gfx_text(0, 42, buf, 1);
}

static void header(char *out) {
    if (reel_recording())
        sprintf(out, "REC");
    else
        sprintf(out, "SP%d", p_splice);
}

const mode_t MODE_MORPH = {
    .name = "MORPH",
    .reset = reset,
    .on_tick = on_tick,
    .update = update,
    .draw = draw,
    .header = header,
    .restart = restart,
    .randomize = randomize,
    .params = PARAMS,
    .n_params = 6,
    .hide_globals = true, // ROOT/OCT/GATE mean nothing to a reel; skip them
};
