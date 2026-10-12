// =============================================================================
// SYNDRUM - synthesized drums, 909-flavoured with an industrial snare
// V1: kick - V2: snare - V3: hat, one drum per output jack.
// Same Truchets sequencing as DRUMS (shared drum_seq.c) so the two can be
// A/B'd on identical patterns; the difference is entirely in the sound
// engine. Nothing is sampled here - every hit is generated live, so TUNE and
// DECAY reach places a fixed sample cannot.
// =============================================================================
#include <stdio.h>

#include "../drum_seq.h"
#include "../drumsynth.h"
#include "../gfx.h"
#include "../mode.h"
#include "../sampler.h"
#include "../truchet_patterns.h"
#include "mode_util.h"

static int16_t p_bank = TRUCHET_DEFAULT_BANK;
static int16_t p_x = TRUCHET_DEFAULT_X;
static int16_t p_y = TRUCHET_DEFAULT_Y;
static int16_t p_swing = 50;
static int16_t p_ktune = 0, p_kdecay = 55;
static int16_t p_stune = 0, p_sdecay = 45;
static int16_t p_htune = 0, p_hdecay = 30;

static void fmt_semi(char *out, int16_t v) { sprintf(out, "%+d", v); }
static void fmt_pct(char *out, int16_t v) { sprintf(out, "%d%%", v); }
static void fmt_bank(char *out, int16_t v) {
    sprintf(out, "%s", truchet_bank_name(v));
}

static const param_t PARAMS[] = {
    {"BANK", &p_bank, 0, TRUCHET_BANKS - 1, 1, fmt_bank},
    {"X", &p_x, 0, 100, 5, 0},
    {"Y", &p_y, 0, 100, 5, 0},
    {"SWING", &p_swing, 50, 75, 1, fmt_pct},
    {"K TUNE", &p_ktune, -24, 24, 1, fmt_semi},
    {"K DECAY", &p_kdecay, 0, 100, 5, fmt_pct},
    {"S TUNE", &p_stune, -24, 24, 1, fmt_semi},
    {"S DECAY", &p_sdecay, 0, 100, 5, fmt_pct},
    {"H TUNE", &p_htune, -24, 24, 1, fmt_semi},
    {"H DECAY", &p_hdecay, 0, 100, 5, fmt_pct},
};

static drum_seq_t seq;

static void fire_hits(void *ctx, uint32_t hits) {
    (void)ctx;
    for (int d = 0; d < 3; d++) {
        if (!((hits >> d) & 1u)) continue;

        uint16_t velocity = 256;
        switch (d) {
        case 0:
            drumsynth_trigger(0, DRUM_KICK, p_ktune, p_kdecay);
            break;
        case 1:
            drumsynth_trigger(1, DRUM_SNARE, p_stune, p_sdecay);
            break;
        default:
            velocity = drum_seq_hat_velocity_q8();
            drumsynth_trigger_velocity(2, DRUM_HAT, p_htune, p_hdecay,
                                       velocity);
            break;
        }
        seq.lit[d] = (uint8_t)(velocity - 1u);
    }
}

static void restart(void) {
    drum_seq_start(&seq, &p_bank, &p_x, &p_y, &p_swing);
}

static void reset(void) {
    restart();
    for (int v = 0; v < 3; v++) {
        sampler_enable(v, true);
        sampler_set_synth(v, true);
        seq.lit[v] = 0;
    }
}

static void on_tick(uint64_t now) {
    drum_seq_on_tick(&seq, now, fire_hits, 0);
}

static void randomize_xy(int16_t *x, int16_t *y) {
    drum_seq_randomize(&seq, x, y);
}

static void update(float dt, uint64_t now) {
    drum_seq_update(&seq, dt, now, fire_hits, 0);
}

static void draw(void) {
    static const char *const LABEL[3] = {"K", "S", "H"};
    const int16_t *dec[3] = {&p_kdecay, &p_sdecay, &p_hdecay};
    const int n = TRUCHET_STEPS;
    // Match DRUMS' step size: the grid runs to x=108, leaving just enough for
    // a compact decay bar. The per-drum TUNE value lives in the parameter
    // line rather than eating a third of the screen width.
    int w = 96 / (n > 0 ? n : 16);
    if (w > 7) w = 7;
    if (w < 2) w = 2;

    for (int d = 0; d < 3; d++) {
        int y = 13 + d * 11; // clear of the UI header at y 0-10
        if (seq.lit[d] > 40)
            gfx_fill_rect(0, y - 1, 9, 10, 1);
        gfx_text(2, y, LABEL[d], 1);

        for (int i = 0; i < n; i++) {
            int x = 12 + i * w;
            if ((seq.planned[d] >> i) & 1u)
                gfx_fill_rect(x, y, w - 1, 7, 1);
            else
                gfx_hline(x, y + 6, w - 1, 1);
        }
        gfx_rect(12 + seq.pos * w - 1, y - 1, w + 1, 9, 1);

        // decay as a small bar, so the grid keeps the width instead. Sits
        // after the fixed 32-step grid and clamped to stay on screen.
        int bx = 12 + n * w + 2;
        if (bx > 112) bx = 112;
        gfx_rect(bx, y, 14, 7, 1);
        int fillw = (*dec[d] * 12) / 100;
        if (fillw > 0) gfx_fill_rect(bx + 1, y + 1, fillw, 5, 1);
    }
}

static void header(char *out) {
    sprintf(out, "f%d%%", drum_seq_fill_percent());
}

const mode_t MODE_SYNDRUM = {
    .name = "SYNDRUM",
    .reset = reset,
    .on_tick = on_tick,
    .update = update,
    .draw = draw,
    .params = PARAMS,
    .header = header,
    .restart = restart,
    .randomize = randomize_xy,
    .n_params = 10,
    .hide_globals = true,
};
