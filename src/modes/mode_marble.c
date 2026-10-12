// =============================================================================
// MARBLE - probabilistic CV + gate brain (Mutable Instruments Marbles spirit)
// =============================================================================
// Two stepped-CV streams on V1/V2 and a gate stream on V3, all driven by the
// same 32-step Truchets morphing banks the drum modes use: a node's map level
// (0..255) becomes the probability that the stream *fires* on that step.
// A firing stream steps a random-walk value ladder; non-firing streams hold.
//   CV1     live CHAOS: pulls the deterministic probabilities into pure dice
//   BANK/X/Y  the familiar 5x5 node morph (short-press EXTRA2 hold re-rolls)
//   RES     quantization of the CV ladders (2..16 steps across 0-3.3 V)
//   ROT     rotate the value set (which rung is "home")
//   JUMP    ladder stride when a stream fires (1 = scan, 8 = leaping chaos)
//   SMOOTH  0 = sample & hold (Marbles mode 1), 100 = glide (mode 2)
//   GWID    gate width on V3, % of the tick
// Outputs are steady DAC levels behind the same 1k+10nF RC the drum jacks
// want, so they are real 0-3.3 V CV. Clock out keeps pulsing one per tick.
#include <stdio.h>

#include "../clockgen.h"
#include "../config.h"
#include "../gfx.h"
#include "../marble.h"
#include "../mode.h"
#include "../rng.h"
#include "../sampler.h"
#include "../state.h"
#include "../truchet_patterns.h"

static int16_t p_bank = TRUCHET_DEFAULT_BANK;
static int16_t p_x = TRUCHET_DEFAULT_X;
static int16_t p_y = TRUCHET_DEFAULT_Y;
static int16_t p_res = 8;
static int16_t p_rot = 0;
static int16_t p_jump = 2;
static int16_t p_smooth = 0;
static int16_t p_gwid = 50;

static void fmt_bank(char *out, int16_t v) {
    sprintf(out, "%s", truchet_bank_name(v));
}
static void fmt_pct(char *out, int16_t v) { sprintf(out, "%d%%", v); }

static const param_t PARAMS[] = {
    {"BANK", &p_bank, 0, TRUCHET_BANKS - 1, 1, fmt_bank},
    {"X", &p_x, 0, 100, 5, 0},
    {"Y", &p_y, 0, 100, 5, 0},
    {"RES", &p_res, 2, 16, 1, 0},
    {"ROT", &p_rot, 0, 15, 1, 0},
    {"JUMP", &p_jump, 1, 8, 1, 0},
    {"SMOOTH", &p_smooth, 0, 100, 5, fmt_pct},
    {"GWID", &p_gwid, 10, 90, 5, fmt_pct},
};

// Stream ladders and their smoothed DAC targets, 0..255 domain.
static int val[2];   // ladder positions (rungs)
static int tgt[2];   // target level after RES/ROT mapping
static int cur[2];   // level currently emitted
static uint64_t gate_off_at;
static bool gate_on;
static uint8_t lit[2]; // display glow decay
static uint8_t glow_gate;

static int chaos(void) {
    int c = (int)(g.cv1 * 100.0f + 0.5f);
    if (c < 0) c = 0;
    if (c > 100) c = 100;
    return c;
}

// Fire probability for one stream: node level 0..255 as p, then blend with
// pure chaos like Marbles' CHAOS knob does. At full chaos the deterministic
// map is replaced by a coin flip (threshold 128 +/- spread).
static bool stream_fire(int instrument, int step, int c) {
    int level = truchet_level(p_bank, p_x, p_y, instrument, step);
    int p = level * (100 - c) / 100 + 128 * c / 100;
    if (c > 0) p += (int)rng_below((uint32_t)(2 * c + 1)) - c;
    if (p < 0) p = 0;
    if (p > 255) p = 255;
    return (int)rng_below(256) < p;
}

static void step_ladder(int s) {
    int j = p_jump;
    val[s] = (val[s] + 1 + (j > 1 ? (int)rng_below((uint32_t)j) : 0)) % p_res;
    tgt[s] = marble_cv_u8(p_res, (val[s] + p_rot) % p_res);
}

static void reset(void) {
    for (int v = 0; v < NUM_VOICES; v++) {
        sampler_enable(v, true);
        sampler_hold(v, 128);
    }
    val[0] = (int)rng_below((uint32_t)p_res);
    val[1] = (int)rng_below((uint32_t)p_res);
    step_ladder(0);
    step_ladder(1);
    cur[0] = tgt[0];
    cur[1] = tgt[1];
    gate_on = false;
    gate_off_at = 0;
    sampler_hold(0, cur[0]);
    sampler_hold(1, cur[1]);
    sampler_hold(2, 0);
}

static void on_tick(uint64_t now) {
    int step = (int)(g.step % TRUCHET_STEPS);
    int c = chaos();
    for (int s = 0; s < 2; s++) {
        if (stream_fire(s, step, c)) {
            step_ladder(s);
            lit[s] = 3;
        }
    }
    if (stream_fire(2, step, c)) {
        uint32_t period_us = clock_period_us();
        uint64_t w = period_us * (uint64_t)p_gwid / 100u;
        if (w < 1000) w = 1000;
        gate_on = true;
        gate_off_at = now + w;
        sampler_hold(2, 255);
        glow_gate = 3;
    }
}

static void update(float dt_s, uint64_t now) {
    // glide (Marbles mode 2) or hard jump (mode 1)
    int k = p_smooth * 40; // 0..4000
    if (k > 0) {
        float f = dt_s * (float)k;
        for (int s = 0; s < 2; s++) {
            int d = tgt[s] - cur[s];
            int move = (int)(f * (float)d);
            if (move == 0 && d != 0) move = d > 0 ? 1 : -1;
            if (move > d) move = d;
            if (move < d) move = d;
            cur[s] += move;
        }
    } else {
        cur[0] = tgt[0];
        cur[1] = tgt[1];
    }
    sampler_hold(0, cur[0]);
    sampler_hold(1, cur[1]);
    if (gate_on && now >= gate_off_at) {
        gate_on = false;
        sampler_hold(2, 0);
    }
}

static void restart(void) {
    // Bar restart: ladders re-enter at home, values kept otherwise.
    val[0] = 0;
    val[1] = 0;
    step_ladder(0);
    step_ladder(1);
}

static void randomize(int16_t *x, int16_t *y) {
    p_x = (int16_t)(rng_below(21) * 5);
    p_y = (int16_t)(rng_below(21) * 5);
    val[0] = (int)rng_below((uint32_t)p_res);
    val[1] = (int)rng_below((uint32_t)p_res);
    if (x) *x = p_x;
    if (y) *y = p_y;
}

static void draw(void) {
    // 5x5 node map with the live X/Y dot.
    for (int gx = 0; gx < 5; gx++)
        for (int gy = 0; gy < 5; gy++)
            gfx_pixel(4 + gx * 3, 14 + gy * 3, 1);
    gfx_fill_rect(4 + (p_x * 4 / 100) * 3 - 1, 14 + (p_y * 4 / 100) * 3 - 1,
                  3, 3, 1);

    // CV1 / CV2 bars, glowing right after a fire.
    for (int s = 0; s < 2; s++) {
        int w = cur[s] * 60 / 255;
        gfx_rect(24, 14 + s * 10, 61, 6, 1);
        gfx_fill_rect(25, 15 + s * 10, w, 4, 1);
        gfx_text(14, 14 + s * 10, s ? "B" : "A", 1);
        if (lit[s]) lit[s]--;
    }
    // Gate indicator.
    gfx_rect(92, 14, 10, 16, 1);
    if (gate_on || glow_gate) {
        gfx_fill_rect(93, 15, 8, 14, 1);
        if (glow_gate) glow_gate--;
    }

    char buf[24];
    sprintf(buf, "%s X%d Y%d", truchet_bank_name(p_bank), p_x / 5, p_y / 5);
    gfx_text(0, 40, buf, 1);
    sprintf(buf, "A%d/%d B%d  ch%d%%", (tgt[0] * (p_res - 1) + 127) / 255 + 1,
            p_res, (tgt[1] * (p_res - 1) + 127) / 255 + 1, chaos());
    gfx_text(0, 50, buf, 1);
}

static void header(char *out) { sprintf(out, "ch%d", chaos()); }

const mode_t MODE_MARBLE = {
    .name = "MARBLE",
    .reset = reset,
    .on_tick = on_tick,
    .update = update,
    .draw = draw,
    .header = header,
    .restart = restart,
    .randomize = randomize,
    .params = PARAMS,
    .n_params = 8,
    .hide_globals = true,
};
