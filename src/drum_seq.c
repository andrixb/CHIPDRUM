// =============================================================================
// drum_seq - shared sequencing for DRUMS and SYNDRUM. See drum_seq.h.
// This is the logic both modes ran duplicated; behaviour is unchanged.
// =============================================================================
#include "drum_seq.h"

#include "clockgen.h"
#include "drum_timing.h"
#include "rng.h"
#include "state.h"
#include "truchet_patterns.h"

// Roll every random choice before playback so the OLED always shows the
// phrase that is going to play. Replanning mid-phrase only changes steps at
// and after the playhead; steps already played are left alone.
static void plan_range(drum_seq_t *s, int first_step, int end_step,
                       int16_t bank, int16_t x, int16_t y) {
    if (first_step < 0) first_step = 0;
    if (first_step >= TRUCHET_STEPS) first_step = TRUCHET_STEPS - 1;
    if (end_step > TRUCHET_STEPS) end_step = TRUCHET_STEPS;
    for (int d = 0; d < 3; d++) {
        for (int step = first_step; step < end_step; step++) {
            uint32_t bit = 1u << step;
            if (truchet_should_hit(bank, x, y, d, step, g.cv1, rng_f()))
                s->planned[d] |= bit;
            else
                s->planned[d] &= ~bit;
        }
    }
}

static void remember_current_plan(drum_seq_t *s) {
    s->planned_bank = *s->bank;
    s->planned_x = *s->x;
    s->planned_y = *s->y;
    s->planned_fill = drum_seq_fill_percent();
}

static void plan_from(drum_seq_t *s, int first_step) {
    plan_range(s, first_step, TRUCHET_STEPS, *s->bank, *s->x, *s->y);
    remember_current_plan(s);
}

static void ensure_plan(drum_seq_t *s) {
    bool pattern_changed = s->planned_bank != *s->bank || s->planned_x != *s->x ||
                           s->planned_y != *s->y;
    bool fill_changed = s->planned_fill != drum_seq_fill_percent();
    if (pattern_changed) {
        // A direct BANK/X/Y edit supersedes a queued random jump.
        // Restore the wrapped first bar too if it had already been pre-rolled.
        if (s->xy_queued && s->queued_at == 0 && s->pos >= 16)
            plan_range(s, 0, 16, *s->bank, *s->x, *s->y);
        s->xy_queued = false;
        plan_from(s, s->pos);
    } else if (fill_changed && s->xy_queued) {
        // Keep a queued X/Y jump alive while the fill knob moves: update the
        // rest of this bar with the current coordinates and re-roll the next
        // bar with the queued ones.
        if (s->queued_at == 16) {
            plan_range(s, s->pos, 16, *s->bank, *s->x, *s->y);
            plan_range(s, 16, 32, *s->bank, s->queued_x, s->queued_y);
        } else {
            plan_range(s, s->pos, 32, *s->bank, *s->x, *s->y);
            plan_range(s, 0, 16, *s->bank, s->queued_x, s->queued_y);
        }
        s->planned_fill = drum_seq_fill_percent();
    } else if (fill_changed) {
        plan_from(s, s->pos);
    }
}

static void apply_queued_xy(drum_seq_t *s) {
    if (!s->xy_queued || s->pos != s->queued_at) return;
    *s->x = s->queued_x;
    *s->y = s->queued_y;
    s->xy_queued = false;

    // At a wrapped boundary, the second bar occupied cells that were still
    // playing when the request was queued. It is safe to pre-roll them now.
    if (s->pos == 0)
        plan_range(s, 16, TRUCHET_STEPS, *s->bank, *s->x, *s->y);
    remember_current_plan(s);
}

void drum_seq_start(drum_seq_t *s, int16_t *bank, int16_t *x, int16_t *y,
                    int16_t *swing) {
    s->bank = bank;
    s->x = x;
    s->y = y;
    s->swing = swing;
    s->pos = 0;
    s->delayed_hits = 0;
    s->xy_queued = false;
    plan_from(s, 0);
}

void drum_seq_randomize(drum_seq_t *s, int16_t *x, int16_t *y) {
    s->queued_x = (int16_t)(rng_below(21) * 5);
    s->queued_y = (int16_t)(rng_below(21) * 5);
    *x = s->queued_x;
    *y = s->queued_y;
    s->queued_at = drum_next_bar_boundary(s->pos);
    s->xy_queued = true;

    // Pre-roll only the next bar. This updates the OLED immediately without
    // touching any step that still belongs to the currently playing bar.
    plan_range(s, s->queued_at, s->queued_at + 16, *s->bank, s->queued_x,
               s->queued_y);
}

void drum_seq_on_tick(drum_seq_t *s, uint64_t now, drum_fire_fn fire,
                      void *ctx) {
    // A stalled main loop must never let a delayed offbeat overtake the next
    // clock tick. Normally update() has already fired it well before here.
    if (s->delayed_hits) {
        fire(ctx, s->delayed_hits);
        s->delayed_hits = 0;
    }
    ensure_plan(s);
    apply_queued_xy(s);
    if (s->pos >= TRUCHET_STEPS) s->pos = 0;

    uint32_t hits = 0;
    for (int d = 0; d < 3; d++) {
        if ((s->planned[d] >> s->pos) & 1u) hits |= 1u << d;
    }
    uint32_t delay = drum_swing_delay_us(*s->swing, clock_period_us(), s->pos);
    if (hits && delay) {
        s->delayed_hits = hits;
        s->delayed_at = now + delay;
    } else if (hits) {
        fire(ctx, hits);
    }
    s->pos = (s->pos + 1) % TRUCHET_STEPS;
    if (s->pos == 0 && !(s->xy_queued && s->queued_at == 0)) plan_from(s, 0);
}

void drum_seq_update(drum_seq_t *s, float dt, uint64_t now, drum_fire_fn fire,
                     void *ctx) {
    if (s->delayed_hits && now >= s->delayed_at) {
        fire(ctx, s->delayed_hits);
        s->delayed_hits = 0;
    }
    int fade = (int)(dt * 900.0f);
    if (fade < 1) fade = 1;
    for (int d = 0; d < 3; d++)
        s->lit[d] = s->lit[d] > fade ? (uint8_t)(s->lit[d] - fade) : 0;
}

int drum_seq_fill_percent(void) {
    int pct = (int)(g.cv1 * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

uint16_t drum_seq_hat_velocity_q8(void) {
    int fill = drum_seq_fill_percent();
    return fill > 60 ? drum_hat_velocity_q8(fill, rng_u32()) : 256;
}
