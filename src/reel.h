// =============================================================================
// REEL - live-recorded loop buffer + 3 gene playback heads (CHIPMODS/MORPH)
// =============================================================================
// Least-risky design notes:
// - Fixed static buffer, no malloc, no flash writes. Reel lives in SRAM only.
// - Sample rate == AUDIO_SR (25 kHz), 8-bit unsigned, same domain as sampler.
// - All DSP integer-only; powf() (for SPEED) runs in mode on_tick (main loop),
//   never in the 25 kHz ISR.
// - When reel is disabled (all other modes) the sampler ISR does one branch
//   and nothing else.
// - Host-testable: the mix/interp helpers compile with -DPICO_NO_HARDWARE.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define REEL_LEN 75000u      // 3.0 s @ 25 kHz. Static .bss, ~73 KB.
#define REEL_NSPLICE 8
#define REEL_NHEADS 3

// FX ids (param FX)
#define REEL_FX_OFF 0
#define REEL_FX_CRUSH 1
#define REEL_FX_TILT 2

void reel_reset(void);             // silence reel, park heads, REC off
void reel_enable(bool on);         // ISR hook master switch
bool reel_active(void);

// --- record path (called from the audio ISR, or host test) ---
void reel_set_rec(bool on);
bool reel_recording(void);
void reel_set_sos_pct(int pct);    // 0..100 Sound-On-Sound overdub amount
void reel_capture_sample(uint16_t adc12); // one 12-bit ADC read, 0..4095

// --- playback heads (configured from mode on_tick/update, main loop) ---
// The head position is 17.15: enough integer bits for the whole reel (75000)
// plus a 15-bit fraction. step_q15: signed 17.15 increment, (1<<15) = 1x
// forward, negative = reverse.
// gain_q8: 0..256 amplitude.
void reel_head_setup(int head, uint32_t start, uint32_t len,
                     int32_t step_q15, uint16_t gain_q8);
void reel_head_gain(int head, uint16_t gain_q8);
void reel_head_retrigger(int head, uint32_t start);

// --- FX (global, set from mode params) ---
void reel_set_fx(int fx, int amt_pct); // fx 0..2, amt 0..100

// --- ISR render: one voice -> PWM DAC level 0..DAC_WRAP ---
uint16_t reel_render_level(int head);

// --- read-only UI helpers (core1 may call; tearing is cosmetic) ---
uint32_t reel_length(void);
uint8_t reel_peek(uint32_t idx);   // raw 0..255 sample for waveform draw
uint32_t reel_write_pos(void);
uint32_t reel_head_pos(int head);  // integer sample index of head
uint32_t reel_splice_start(int i); // i in 0..7
uint32_t reel_splice_len(int i);

// --- pure helpers exposed for host tests ---
int32_t reel_mix_signed(int32_t old_s, int32_t in_s, int sos_q8);
uint8_t reel_interp_u8(uint8_t a, uint8_t b, uint16_t frac_q16);
