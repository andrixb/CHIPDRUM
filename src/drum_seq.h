// =============================================================================
// drum_seq - the shared Truchets sequencing brain of DRUMS and SYNDRUM
// =============================================================================
// Both drum modes plan 32 steps ahead (so the OLED shows what WILL play, not
// what played), apply the FILL deadband, queue X/Y randomizations at bar
// boundaries, delay offbeats for swing, and fade the per-drum display glow.
// Only the sound differs: DRUMS triggers samples, SYNDRUM runs the synth;
// the mode passes a fire() callback for its own voice path.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct drum_seq drum_seq_t;
typedef void (*drum_fire_fn)(void *ctx, uint32_t hits);

struct drum_seq {
    // Live parameters, owned by the mode; the sequencer points at them so
    // LEFT/RIGHT edits take effect without any handover.
    int16_t *bank, *x, *y, *swing;

    // Mode state, read by the mode's draw():
    uint32_t planned[3]; // bit i = hit planned at step i, one mask per drum
    int pos;             // next step to play (0..31)
    uint8_t lit[3];      // display glow per drum, decayed in update()
    int16_t planned_bank, planned_x, planned_y; // plan provenance

    // Internal:
    int planned_fill;
    bool xy_queued;
    int16_t queued_x, queued_y;
    int queued_at;
    uint32_t delayed_hits;
    uint64_t delayed_at;
};

// Plan a fresh phrase from step 0 and point at the mode's params.
// Used by mode reset() and bar restart() alike.
void drum_seq_start(drum_seq_t *s, int16_t *bank, int16_t *x, int16_t *y,
                    int16_t *swing);
// One clock tick: emit this step's hits (or arm the swing delay).
void drum_seq_on_tick(drum_seq_t *s, uint64_t now, drum_fire_fn fire, void *ctx);
// Every main-loop iteration: fire delayed swing hits, fade the display glow.
void drum_seq_update(drum_seq_t *s, float dt, uint64_t now,
                     drum_fire_fn fire, void *ctx);
// 2 s EXTRA2 hold: queue random X/Y for the next bar boundary.
void drum_seq_randomize(drum_seq_t *s, int16_t *x, int16_t *y);
// FILL/DENSITY knob, quantized to whole percent - the header text and the
// velocity roll must agree on the same value.
int drum_seq_fill_percent(void);
// Hat velocity with fill-dependent variation (unity through 60% FILL).
uint16_t drum_seq_hat_velocity_q8(void);
