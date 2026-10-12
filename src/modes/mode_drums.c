// =============================================================================
// DRUMS - sample playback drum machine
// V1: kick - V2: snare - V3: hat, one drum per output jack.
// Both drum modes read the same Truchets banks. BANK selects OG Grids,
// Electronic or Breakbeat; X/Y morph smoothly through each 5x5 map.
// CV1 remains a live FILL/DENSITY control: noon is a stable core groove,
// below noon thins lower-priority hits and above noon adds probabilistic
// bank-informed fills, increasingly favouring the end of the 32-step phrase.
// All sequencing lives in drum_seq.c - this file is kit selection, trigger
// dispatch and display.
// =============================================================================
#include <stdio.h>

#include "../config.h"
#include "../drum_seq.h"
#include "../gfx.h"
#include "../mode.h"
#include "../samples.h"
#include "../samples_bank1.h"
#include "../samples_bank2.h"
#include "../sampler.h"
#include "../truchet_patterns.h"
#include "mode_util.h"

static int16_t p_bank = TRUCHET_DEFAULT_BANK;
static int16_t p_kit = 0;
static int16_t p_x = TRUCHET_DEFAULT_X;
static int16_t p_y = TRUCHET_DEFAULT_Y;
static int16_t p_swing = 50; // 50% = straight timing
static int16_t p_tune = 0;   // kick pitch in semitones

#define NUM_KITS 3

static void fmt_semi(char *out, int16_t v) { sprintf(out, "%+d", v); }
static void fmt_pct(char *out, int16_t v) { sprintf(out, "%d%%", v); }
static void fmt_kit(char *out, int16_t v) {
    static const char *const NAMES[NUM_KITS] = {"909", "BANK 1", "BANK 2"};
    if (v < 0) v = 0;
    if (v >= NUM_KITS) v = NUM_KITS - 1;
    sprintf(out, "%s", NAMES[v]);
}
static void fmt_bank(char *out, int16_t v) {
    sprintf(out, "%s", truchet_bank_name(v));
}

static const param_t PARAMS[] = {
    {"BANK", &p_bank, 0, TRUCHET_BANKS - 1, 1, fmt_bank},
    {"KIT", &p_kit, 0, NUM_KITS - 1, 1, fmt_kit},
    {"X", &p_x, 0, 100, 5, 0},
    {"Y", &p_y, 0, 100, 5, 0},
    {"SWING", &p_swing, 50, 75, 1, fmt_pct},
    {"TUNE", &p_tune, -12, 12, 1, fmt_semi},
};

_Static_assert(BANK1_SAMPLE_RATE_HZ == AUDIO_SR,
               "sample bank 1 has the wrong sample rate");
_Static_assert(BANK2_SAMPLE_RATE_HZ == AUDIO_SR,
               "sample bank 2 has the wrong sample rate");

typedef struct {
    const uint8_t *pcm[3];
    uint32_t len[3];
} sample_kit_t;

static const sample_kit_t KITS[NUM_KITS] = {
    {{KICK_PCM, SNARE_PCM, HAT_PCM}, {KICK_LEN, SNARE_LEN, HAT_LEN}},
    {{BANK1_KICK_PCM, BANK1_SNARE_PCM, BANK1_HAT_PCM},
     {BANK1_KICK_LEN, BANK1_SNARE_LEN, BANK1_HAT_LEN}},
    {{BANK2_KICK_PCM, BANK2_SNARE_PCM, BANK2_HAT_PCM},
     {BANK2_KICK_LEN, BANK2_SNARE_LEN, BANK2_HAT_LEN}},
};

static drum_seq_t seq;

static void fire_hits(void *ctx, uint32_t hits) {
    (void)ctx;
    int kit = p_kit;
    if (kit < 0) kit = 0;
    if (kit >= NUM_KITS) kit = NUM_KITS - 1; // param clamps; belt and braces
    const sample_kit_t *k = &KITS[kit];
    for (int d = 0; d < 3; d++) {
        if (!((hits >> d) & 1u)) continue;
        uint16_t velocity = d == 2 ? drum_seq_hat_velocity_q8() : 256;
        sampler_trigger(d, k->pcm[d], k->len[d], d == 0 ? p_tune : 0,
                        velocity);
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
        sampler_set_synth(v, false);
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
    const int n = TRUCHET_STEPS;
    int w = 112 / n;
    if (w > 8) w = 8;
    if (w < 2) w = 2;

    for (int d = 0; d < 3; d++) {
        // Rows start below the UI header (which owns y 0-10) and are packed
        // tightly enough to leave a line for the FILL readout underneath.
        int y = 13 + d * 11;
        // label, inverted while the drum is sounding
        if (seq.lit[d] > 40) {
            gfx_fill_rect(0, y - 1, 9, 10, 1);
            gfx_text(2, y, LABEL[d], 1);
            // punch the letter back out of the block
            for (int px = 2; px < 8; px++)
                for (int py = y; py < y + 8; py++)
                    if ((px + py) & 1) gfx_pixel(px, py, 0);
        } else {
            gfx_text(2, y, LABEL[d], 1);
        }

        for (int i = 0; i < n; i++) {
            int x = 12 + i * w;
            if ((seq.planned[d] >> i) & 1u)
                gfx_fill_rect(x, y, w - 1, 7, 1);
            else
                gfx_hline(x, y + 6, w - 1, 1);
        }
        // playhead
        int px = 12 + seq.pos * w;
        gfx_rect(px - 1, y - 1, w + 1, 9, 1);
    }
}

// FILL is a live performance control with no other readout, so it rides in
// the header. BANK/X/Y are available in the parameter line.
static void header(char *out) {
    sprintf(out, "f%d%%", drum_seq_fill_percent());
}

const mode_t MODE_DRUMS = {
    .name = "DRUMS",
    .reset = reset,
    .on_tick = on_tick,
    .update = update,
    .draw = draw,
    .header = header,
    .restart = restart,
    .randomize = randomize_xy,
    .params = PARAMS,
    .n_params = 6,
    .hide_globals = true,
};
