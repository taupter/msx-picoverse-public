// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// midipac_main.c - PSG-to-MIDI conversion firmware (MIDI-PAC)
//
// Passively intercepts all AY-3-8910 PSG register writes from MSX software
// and converts them in real time to MIDI events sent to a USB MIDI device
// (e.g., Roland SoundCanvas) connected to the Pico's USB port.
//
// Architecture:
//   Core 0: PIO1 IRQ handler captures PSG I/O writes (ports 0xA0-0xA1).
//           Maintains PSG shadow registers and converts PSG events to MIDI
//           messages queued in a lock-free ring buffer.
//   Core 1: TinyUSB host task polls USB MIDI device. Drains the TX ring
//           buffer, formats USB-MIDI event packets, and sends them to the
//           USB MIDI device. Also runs a periodic envelope processor.
//
// PSG-to-MIDI conversion is based on the AY-3-8910/YM2149 hardware
// specifications (GI datasheet) and verified against the ayumi emulator
// (Peter Sovietov) used by the aymidi project (berarma/aymidi).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "tusb.h"
#include "midipac.h"
#include "usb_midi_host.h"
#include "msx_midipac.pio.h"

// Include the pre-computed frequency table
#include "psg_freq_table.h"

#define MIDI_WIRE_BYTE_US 320
#define MIDI_TONE_BEND_QUANTUM 16
#define MIDI_TONE_EXPR_QUANTUM 4
#define MIDI_TONE_BEND_MIN_INTERVAL_US 500
#define MIDI_TONE_EXPR_MIN_INTERVAL_US 8000

// 50 Hz frame processing matches the intent of common MSX music engines,
// avoids reacting to every individual PSG register write, and keeps vibrato
// as pitch bend instead of repeated note retriggers.
#define PSG_FRAME_RATE_HZ           50
#define PSG_FRAME_TICK_US           (1000000 / PSG_FRAME_RATE_HZ)
#define PSG_BEND_RATE_HZ            200
#define PSG_BEND_TICK_US            (1000000 / PSG_BEND_RATE_HZ)
#define MIDI_VIBRATO_BEND_ONLY_DELTA_X256 256
#define TONE_CENTER_WINDOW_X256     192
#define TONE_STABLE_RETRIGGER_FRAMES 3
#define TONE_HISTORY_LEN            4
#define NOISE_DIVISOR_SCALE         8
#define NOISE_DIVISOR_BIAS          16

// -----------------------------------------------------------------------
// TX ring buffer: Core 0 (IRQ) writes, Core 1 reads
// Single-producer single-consumer — safe without locks on RP2040
// -----------------------------------------------------------------------
static volatile uint8_t tx_ring_buf[MIDIPAC_TX_BUFSIZE];
static volatile uint32_t tx_ring_head;   // Written by Core 0
static volatile uint32_t tx_ring_tail;   // Written by Core 1

static inline bool __not_in_flash_func(tx_ring_put)(uint8_t byte) {
    uint32_t next = (tx_ring_head + 1) & (MIDIPAC_TX_BUFSIZE - 1);
    if (next == tx_ring_tail) return false;  // Full — drop byte
    tx_ring_buf[tx_ring_head] = byte;
    __dmb();
    tx_ring_head = next;
    return true;
}

static inline bool tx_ring_get(uint8_t *byte) {
    if (tx_ring_head == tx_ring_tail) return false;
    *byte = tx_ring_buf[tx_ring_tail];
    __dmb();
    tx_ring_tail = (tx_ring_tail + 1) & (MIDIPAC_TX_BUFSIZE - 1);
    return true;
}

// Number of free bytes in the TX ring (safe to call from either core)
static inline uint32_t __not_in_flash_func(tx_ring_free)(void) {
    return (tx_ring_tail - tx_ring_head - 1) & (MIDIPAC_TX_BUFSIZE - 1);
}

// Hardware spinlock: both Core 0 (PIO IRQ) and Core 1 (envelope tick)
// produce into the ring buffer. The spinlock makes each MIDI message
// atomic so bytes from different messages never interleave.
static spin_lock_t *tx_ring_lock;

// -----------------------------------------------------------------------
// MIDI message helpers — queue raw MIDI bytes into the ring buffer.
// Admission control: the entire message is dropped if there isn't
// enough room, preventing partial (corrupt) MIDI messages.
// -----------------------------------------------------------------------

static inline bool __not_in_flash_func(midi_send_3)(uint8_t status, uint8_t d1, uint8_t d2) {
    uint32_t save = spin_lock_blocking(tx_ring_lock);
    bool ok = (tx_ring_free() >= 3);
    if (ok) {
        tx_ring_put(status);
        tx_ring_put(d1);
        tx_ring_put(d2);
    }
    spin_unlock(tx_ring_lock, save);
    return ok;
}

static inline bool __not_in_flash_func(midi_send_2)(uint8_t status, uint8_t d1) {
    uint32_t save = spin_lock_blocking(tx_ring_lock);
    bool ok = (tx_ring_free() >= 2);
    if (ok) {
        tx_ring_put(status);
        tx_ring_put(d1);
    }
    spin_unlock(tx_ring_lock, save);
    return ok;
}

// -----------------------------------------------------------------------
// PSG shadow state (maintained in Core 0 IRQ context)
// -----------------------------------------------------------------------
static volatile uint8_t psg_addr_latch;         // Currently selected PSG register
static uint16_t psg_reg[PSG_REG_COUNT];         // Shadow of all 14 PSG registers
static volatile uint32_t psg_reg_seq;           // Seqlock for coherent snapshots

// Per-channel MIDI state
static uint16_t channel_freq[MIDI_CH_COUNT];    // Current PSG frequency divisor
static uint8_t  midi_note[MIDI_CH_COUNT];       // Current MIDI note (0 = off)
static uint16_t midi_pitchbend[MIDI_CH_COUNT];  // Current pitch bend value
static uint8_t  midi_program[MIDI_CH_COUNT];    // Current MIDI program
static uint8_t  midi_expr[MIDI_CH_COUNT];       // Current MIDI CC#11 expression
static uint8_t  psg_tone_mix[3];                // PSG tone enable per voice
static uint8_t  psg_noise_mix[3];               // PSG noise enable per voice
static uint16_t midi_pitchbend_pending[3];      // Deferred tone pitch bend target
static uint8_t  midi_pitchbend_dirty[3];        // Deferred tone pitch bend pending
static uint8_t  midi_expr_pending[3];           // Deferred tone expression target
static uint8_t  midi_expr_dirty[3];             // Deferred tone expression pending
static uint32_t midi_pitchbend_last_us[3];      // Last tone bend send timestamp
static uint32_t midi_expr_last_us[3];           // Last tone expression send timestamp
static uint16_t tone_last_period[3];            // Last processed tone period per PSG voice
static uint8_t  env_level[3];                   // Current envelope-derived level (0-127)
static uint16_t tone_note_history[3][TONE_HISTORY_LEN];
static uint8_t  tone_note_history_count[3];
static uint8_t  tone_pending_note[3];
static uint8_t  tone_pending_count[3];

// PSG volume (0-15) to MIDI CC value.
// Smoother curve with better low-level presence for SC-55 arrangement quality.
static const uint8_t psg_vol_to_midi[16] = {
    0, 18, 30, 40, 50, 58, 66, 74, 82, 90, 98, 106, 113, 119, 124, 127
};

// Envelope volume (0-31) to MIDI CC value.
// Aligned with the fixed-volume table's feel; finer granularity for 5-bit envelope.
static const uint8_t env_vol_to_midi[32] = {
     0,  8, 13, 18, 23, 28, 33, 38, 42, 47, 51, 55, 60, 64, 68, 72,
    76, 80, 84, 88, 92, 95, 99,103,107,110,114,117,120,123,125,127
};

static const uint8_t tone_program[3] = {
    MIDIPAC_MELODIC_PROG_A,
    MIDIPAC_MELODIC_PROG_B,
    MIDIPAC_MELODIC_PROG_C
};

static const uint8_t tone_pan[3] = {
    60, 64, 68
};

static const uint8_t tone_reverb[3] = {
    0, 0, 0
};

static const uint8_t tone_chorus[3] = {
    0, 0, 0
};

// Envelope state
static int      soundenv;                       // Current envelope shape (0-15)
static int      soundenvspeed;                  // Envelope speed (fixed-point)
static int      soundenvspeed_toolarge;         // Overflow flag
static int      usesoundenv[3];                 // Per-tone-channel envelope enable
static int      soundenvcounter[3];             // Per-tone-channel envelope counter

// Silence detection state
static uint32_t all_silent_since_us;             // Timestamp when all channels went quiet
static uint8_t  all_sound_off_sent;              // 1 after CC#120 sent for silence

// Noise frequency → GM percussion drum note mapping.
// 6 bands × 3 voices gives each PSG voice a distinct drum note per band.
// Band boundaries are on the synthetic divisor (R6 * 8 + 16).
static const uint8_t noise_drum_notes[6][3] = {
    {42, 44, 37},   // Band 0 (div ≤  24): Closed HiHat, Pedal HiHat, Side Stick
    {46, 51, 56},   // Band 1 (div ≤  48): Open HiHat, Ride Cymbal, Cowbell
    {38, 40, 39},   // Band 2 (div ≤  80): Snare, Electric Snare, Clap
    {47, 45, 48},   // Band 3 (div ≤ 128): Mid Tom, Low Tom, Hi-Mid Tom
    {36, 41, 43},   // Band 4 (div ≤ 176): Bass Drum, Low Floor Tom, Hi Floor Tom
    {35, 36, 41}    // Band 5 (div > 176): Acoustic Bass Drum, Bass Drum, Low Floor Tom
};

static void psg_snapshot_read(uint16_t *snapshot) {
    uint32_t seq_start;
    uint32_t seq_end;

    do {
        seq_start = psg_reg_seq;
        while (seq_start & 1u) {
            seq_start = psg_reg_seq;
        }
        __dmb();
        memcpy(snapshot, (const void *)psg_reg, sizeof(psg_reg));
        __dmb();
        seq_end = psg_reg_seq;
    } while (seq_start != seq_end || (seq_end & 1u));
}

// -----------------------------------------------------------------------
// MIDI event generators (called from IRQ context — must be fast)
// -----------------------------------------------------------------------

static inline uint8_t __not_in_flash_func(midi_tx_channel)(uint8_t channel) {
    return (channel >= MIDI_CH_NOISE_A) ? MIDI_DRUM_CH : channel;
}

static inline uint8_t __not_in_flash_func(noise_voice_index)(uint8_t channel) {
    return (uint8_t)(channel - MIDI_CH_NOISE_A);
}

static inline uint8_t __not_in_flash_func(midi_psg_level)(uint8_t channel, uint8_t val) {
    if (channel >= MIDI_CH_NOISE_A) {
        // Keep percussion clearly audible but behind the three melodic PSG
        // channels, otherwise channel 10 dominates the mix on GM modules.
        return (uint8_t)((val * 11u) / 16u);
    }
    return val;
}

static inline uint8_t __not_in_flash_func(tone_channel_index)(uint8_t channel) {
    return channel;
}

static inline uint8_t __not_in_flash_func(quantize_u7)(uint8_t value, uint8_t quantum) {
    uint16_t rounded = (uint16_t)value + (uint16_t)(quantum / 2u);
    rounded = (rounded / quantum) * quantum;
    if (rounded > 127u) rounded = 127u;
    return (uint8_t)rounded;
}

static inline uint16_t __not_in_flash_func(quantize_pitchbend)(uint16_t value) {
    uint32_t rounded = (uint32_t)value + (MIDI_TONE_BEND_QUANTUM / 2u);
    rounded = (rounded / MIDI_TONE_BEND_QUANTUM) * MIDI_TONE_BEND_QUANTUM;
    if (rounded > 16383u) rounded = 16383u;
    return (uint16_t)rounded;
}

static void __not_in_flash_func(midi_expression_apply)(uint8_t channel, uint8_t val, uint32_t now_us) {
    if (midi_expr[channel] != val) {
        midi_send_3(0xB0 | channel, 11, val);  // CC#11 Expression
        midi_expr[channel] = val;
    }
    midi_expr_last_us[tone_channel_index(channel)] = now_us;
    midi_expr_dirty[tone_channel_index(channel)] = 0;
}

static void __not_in_flash_func(midi_pitch_bend_apply)(uint8_t channel, uint16_t value, uint32_t now_us) {
    if (midi_pitchbend[channel] != value) {
        midi_send_3(0xE0 | channel, value & 0x7F, (value >> 7) & 0x7F);
        midi_pitchbend[channel] = value;
    }
    midi_pitchbend_last_us[tone_channel_index(channel)] = now_us;
    midi_pitchbend_dirty[tone_channel_index(channel)] = 0;
}

static void midi_flush_deferred_tone_controls(uint32_t now_us) {
    for (uint8_t channel = MIDI_CH_TONE_A; channel <= MIDI_CH_TONE_C; channel++) {
        uint8_t idx = tone_channel_index(channel);

        if (midi_expr_dirty[idx] &&
            (now_us - midi_expr_last_us[idx]) >= MIDI_TONE_EXPR_MIN_INTERVAL_US) {
            midi_expression_apply(channel, midi_expr_pending[idx], now_us);
        }

        if (midi_pitchbend_dirty[idx] &&
            (now_us - midi_pitchbend_last_us[idx]) >= MIDI_TONE_BEND_MIN_INTERVAL_US) {
            midi_pitch_bend_apply(channel, midi_pitchbend_pending[idx], now_us);
        }
    }
}

static void __not_in_flash_func(midi_expression_set)(uint8_t channel, uint8_t val) {
    val = midi_psg_level(channel, val);
    if (channel >= MIDI_CH_NOISE_A) {
        midi_expr[channel] = val;
        return;
    }

    val = quantize_u7(val, MIDI_TONE_EXPR_QUANTUM);
    if (midi_expr[channel] != val) {
        uint8_t idx = tone_channel_index(channel);
        uint32_t now_us = time_us_32();
        uint8_t current = midi_expr[channel];

        midi_expr_pending[idx] = val;

        if (val == 0 || current == 0 || (uint32_t)(now_us - midi_expr_last_us[idx]) >= MIDI_TONE_EXPR_MIN_INTERVAL_US) {
            midi_expression_apply(channel, val, now_us);
        } else {
            midi_expr_dirty[idx] = 1;
        }
    }
}

static void __not_in_flash_func(midi_note_off)(uint8_t channel) {
    if (midi_note[channel]) {
        midi_send_3(0x90 | midi_tx_channel(channel), midi_note[channel], 0);
        midi_note[channel] = 0;
        if (channel < MIDI_CH_NOISE_A) {
            uint8_t idx = tone_channel_index(channel);
            midi_expr_dirty[idx] = 0;
            midi_pitchbend_dirty[idx] = 0;
        }
    }
}

static void __not_in_flash_func(midi_note_on_velocity)(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (note == 0 || note > 127) return;
    if (midi_note[channel] == note) return;

    midi_note_off(channel);
    midi_send_3(0x90 | midi_tx_channel(channel), note, velocity);
    midi_note[channel] = note;
}

static uint8_t __not_in_flash_func(midi_note_velocity)(uint8_t channel) {
    uint16_t base = (channel >= MIDI_CH_NOISE_A) ? 34 : 56;
    uint16_t span = (channel >= MIDI_CH_NOISE_A) ? 52 : 72;
    uint16_t velocity = base + (midi_expr[channel] * span) / 127;
    if (velocity > 127) velocity = 127;
    return (uint8_t)velocity;
}

static void __not_in_flash_func(midi_note_on)(uint8_t channel, uint8_t note) {
    if (midi_note[channel] != note) {
        midi_note_off(channel);
        if (note > 0 && note <= 127) {
            midi_send_3(0x90 | midi_tx_channel(channel), note, midi_note_velocity(channel));
            midi_note[channel] = note;
        }
    }
}

static void __not_in_flash_func(midi_pitch_bend)(uint8_t channel, uint16_t value) {
    if (channel >= MIDI_CH_NOISE_A) {
        return;
    }

    value = quantize_pitchbend(value);

    if (midi_pitchbend[channel] != value) {
        uint8_t idx = tone_channel_index(channel);
        uint32_t now_us = time_us_32();

        midi_pitchbend_pending[idx] = value;

        if (midi_note[channel] == 0 || (uint32_t)(now_us - midi_pitchbend_last_us[idx]) >= MIDI_TONE_BEND_MIN_INTERVAL_US) {
            midi_pitch_bend_apply(channel, value, now_us);
        } else {
            midi_pitchbend_dirty[idx] = 1;
        }
    }
}

static void __not_in_flash_func(midi_pitch_bend_force)(uint8_t channel, uint16_t value) {
    if (channel >= MIDI_CH_NOISE_A) {
        return;
    }

    value = quantize_pitchbend(value);
    midi_pitchbend_pending[tone_channel_index(channel)] = value;
    midi_pitch_bend_apply(channel, value, time_us_32());
}

static void __not_in_flash_func(midi_program_change)(uint8_t channel, uint8_t prog) {
    midi_send_2(0xC0 | channel, prog);
}

static void __not_in_flash_func(midi_program_set)(uint8_t channel, uint8_t prog) {
    if (channel >= MIDI_CH_NOISE_A) {
        return;
    }

    if (midi_program[channel] != prog) {
        midi_program_change(channel, prog);
        midi_program[channel] = prog;
    }
}

// -----------------------------------------------------------------------
// PSG-to-MIDI conversion functions
// -----------------------------------------------------------------------

static inline uint16_t psg_tone_period(const uint16_t *snapshot, uint8_t voice) {
    uint16_t fine = snapshot[PSG_REG_FREQ_A_LO + (voice * 2u)] & 0x00FFu;
    uint16_t coarse = snapshot[PSG_REG_FREQ_A_HI + (voice * 2u)] & 0x000Fu;
    uint16_t period = (coarse << 8) | fine;
    return (period == 0) ? 1u : period;
}

static inline uint8_t psg_voice_level(const uint16_t *snapshot, uint8_t voice) {
    uint8_t volume_reg = snapshot[PSG_REG_VOL_A + voice] & 0x1Fu;
    if (volume_reg & 0x10u) {
        return env_level[voice];
    }
    return psg_vol_to_midi[volume_reg & 0x0Fu];
}

static inline uint8_t psg_voice_velocity(const uint16_t *snapshot, uint8_t voice, uint8_t level) {
    uint8_t volume_reg = snapshot[PSG_REG_VOL_A + voice] & 0x1Fu;
    if (volume_reg & 0x10u) {
        uint16_t velocity = ((uint16_t)level * 120u) / 127u;
        return (velocity == 0) ? 1u : (uint8_t)velocity;
    }

    uint16_t velocity = (uint16_t)(volume_reg & 0x0Fu) * 8u;
    if (velocity > 127u) velocity = 127u;
    return (velocity == 0) ? 1u : (uint8_t)velocity;
}

static inline uint16_t midi_wheel_for_anchor(uint8_t anchor_note, uint16_t fine_note_x256) {
    int32_t delta_x256 = (int32_t)fine_note_x256 - ((int32_t)anchor_note << 8);
    int32_t wheel = 8192 + (delta_x256 * 16);
    if (wheel < 0) return 0;
    if (wheel > 16383) return 16383;
    return (uint16_t)wheel;
}

static void tone_history_push(uint8_t voice, uint16_t fine_note_x256) {
    memmove(&tone_note_history[voice][0],
            &tone_note_history[voice][1],
            sizeof(tone_note_history[voice][0]) * (TONE_HISTORY_LEN - 1));
    tone_note_history[voice][TONE_HISTORY_LEN - 1] = fine_note_x256;
    if (tone_note_history_count[voice] < TONE_HISTORY_LEN) {
        tone_note_history_count[voice]++;
    }
}

static bool tone_history_indicates_modulation(uint8_t voice, uint8_t current_note) {
    if (tone_note_history_count[voice] < TONE_STABLE_RETRIGGER_FRAMES) {
        return false;
    }

    int32_t center = (int32_t)current_note << 8;
    int32_t sum_delta = 0;
    bool seen_positive = false;
    bool seen_negative = false;

    for (uint8_t i = TONE_HISTORY_LEN - TONE_STABLE_RETRIGGER_FRAMES; i < TONE_HISTORY_LEN; i++) {
        int32_t delta = (int32_t)tone_note_history[voice][i] - center;
        if (delta < -MIDI_VIBRATO_BEND_ONLY_DELTA_X256 ||
            delta >  MIDI_VIBRATO_BEND_ONLY_DELTA_X256) {
            return false;
        }
        if (delta > 0) seen_positive = true;
        if (delta < 0) seen_negative = true;
        sum_delta += delta;
    }

    return (seen_positive && seen_negative) ||
           (sum_delta >= -TONE_CENTER_WINDOW_X256 && sum_delta <= TONE_CENTER_WINDOW_X256);
}

static uint8_t __not_in_flash_func(noise_note_for_divisor)(uint8_t channel, uint16_t divisor) {
    uint8_t band;
    uint8_t voice = noise_voice_index(channel);

    if      (divisor <=  24) band = 0;   // Very high — hi-hat, side stick
    else if (divisor <=  48) band = 1;   // High      — open hi-hat, ride
    else if (divisor <=  80) band = 2;   // Mid-high  — snare, clap
    else if (divisor <= 128) band = 3;   // Mid       — toms
    else if (divisor <= 176) band = 4;   // Low       — bass drum, floor tom
    else                     band = 5;   // Very low  — deep bass drum

    return noise_drum_notes[band][voice];
}

static void process_tone_channel(const uint16_t *snapshot, uint8_t voice) {
    uint8_t channel = MIDI_CH_TONE_A + voice;
    uint8_t mixer = snapshot[PSG_REG_MIXER] & 0x3Fu;
    bool tone_enabled = ((mixer & (1u << voice)) == 0);
    uint16_t period = psg_tone_period(snapshot, voice);
    uint8_t level = psg_voice_level(snapshot, voice);
    uint8_t velocity = psg_voice_velocity(snapshot, voice, level);
    uint16_t fine_note_x256 = psg_freq_table[period].fine_note_x256;
    uint8_t target_note = psg_freq_table[period].note;
    uint8_t current_note = midi_note[channel];
    uint16_t target_wheel = midi_wheel_for_anchor(target_note, fine_note_x256);
    uint16_t anchor_wheel;

    channel_freq[channel] = period;
    tone_history_push(voice, fine_note_x256);

    if (!tone_enabled || level == 0 || period >= 4096u) {
        midi_expression_set(channel, 0);
        midi_note_off(channel);
        tone_pending_note[voice] = 0;
        tone_pending_count[voice] = 0;
        tone_last_period[voice] = period;
        return;
    }

    midi_expression_set(channel, level);

    if (current_note == 0) {
        midi_note_on_velocity(channel, target_note, velocity);
        midi_pitch_bend_force(channel, target_wheel);
        tone_pending_note[voice] = 0;
        tone_pending_count[voice] = 0;
        tone_last_period[voice] = period;
        return;
    }
    anchor_wheel = midi_wheel_for_anchor(current_note, fine_note_x256);

    if (current_note == target_note) {
        tone_pending_note[voice] = 0;
        tone_pending_count[voice] = 0;
        midi_pitch_bend(channel, anchor_wheel);
    } else if (tone_history_indicates_modulation(voice, current_note)) {
        tone_pending_note[voice] = 0;
        tone_pending_count[voice] = 0;
        midi_pitch_bend(channel, anchor_wheel);
    } else {
        if (tone_pending_note[voice] != target_note) {
            tone_pending_note[voice] = target_note;
            tone_pending_count[voice] = 1;
        } else if (tone_pending_count[voice] < 255) {
            tone_pending_count[voice]++;
        }

        if (tone_pending_count[voice] >= TONE_STABLE_RETRIGGER_FRAMES) {
            midi_note_on_velocity(channel, target_note, velocity);
            midi_pitch_bend_force(channel, target_wheel);
            tone_pending_note[voice] = 0;
            tone_pending_count[voice] = 0;
        } else {
            midi_pitch_bend(channel, anchor_wheel);
        }
    }

    tone_last_period[voice] = period;
}

static void process_tone_bends(const uint16_t *snapshot) {
    for (uint8_t voice = 0; voice < 3; voice++) {
        uint8_t channel = MIDI_CH_TONE_A + voice;
        uint8_t mixer = snapshot[PSG_REG_MIXER] & 0x3Fu;
        bool tone_enabled = ((mixer & (1u << voice)) == 0);
        uint16_t period = psg_tone_period(snapshot, voice);
        uint8_t level = psg_voice_level(snapshot, voice);

        if (!tone_enabled || level == 0 || period >= 4096u || midi_note[channel] == 0) {
            continue;
        }

        if (tone_history_indicates_modulation(voice, midi_note[channel])) {
            midi_pitch_bend(channel, midi_wheel_for_anchor(midi_note[channel], psg_freq_table[period].fine_note_x256));
        }
    }
}

static void process_noise_channel(const uint16_t *snapshot, uint8_t voice) {
    uint8_t channel = MIDI_CH_NOISE_A + voice;
    uint8_t mixer = snapshot[PSG_REG_MIXER] & 0x3Fu;
    bool noise_enabled = ((mixer & (1u << (voice + 3u))) == 0);
    uint8_t level = psg_voice_level(snapshot, voice);
    uint8_t velocity = psg_voice_velocity(snapshot, voice, level);
    uint16_t divisor = ((snapshot[PSG_REG_NOISE_FREQ] & 0x1Fu) * NOISE_DIVISOR_SCALE) + NOISE_DIVISOR_BIAS;
    uint8_t note = noise_note_for_divisor(channel, divisor);

    channel_freq[channel] = divisor;

    if (!noise_enabled || level == 0) {
        midi_note_off(channel);
        return;
    }

    if (midi_note[channel] != note) {
        midi_note_on_velocity(channel, note, velocity);
    }
}

static void process_frame(void) {
    uint16_t snapshot[PSG_REG_COUNT];
    psg_snapshot_read(snapshot);

    for (uint8_t voice = 0; voice < 3; voice++) {
        process_tone_channel(snapshot, voice);
        process_noise_channel(snapshot, voice);
    }
}

static void __not_in_flash_func(set_env_speed)(uint16_t val) {
    if (val == 0) val = 1;
    // PSG clock = CPU_clock/2 = 1,789,772.5 Hz.  Envelope step rate:
    //   step_rate = PSG_clock / (256 × EP) steps/sec
    // Using the CPU clock convention (2× PSG clock, 512× divider):
    //   step_rate = 3,579,545 / (512 × EP)
    // Fixed-point speed per tick (16.16), at PSG_FRAME_RATE_HZ:
    //   speed = step_rate × 65536 / PSG_FRAME_RATE_HZ
    //         = 3579545 × 128 / (EP × PSG_FRAME_RATE_HZ)
    uint32_t tmp = 458181760U / (val * PSG_FRAME_RATE_HZ);
    if (tmp > PSG_MAX_ENVSPEED) {
        soundenvspeed_toolarge = (int)(tmp - PSG_MAX_ENVSPEED);
        tmp = PSG_MAX_ENVSPEED;
    } else {
        soundenvspeed_toolarge = 0;
    }
    soundenvspeed = (int)tmp;
}

// -----------------------------------------------------------------------
// PSG register write handler — called from PIO IRQ
// -----------------------------------------------------------------------
static void __not_in_flash_func(psg_write)(uint8_t reg, uint8_t value) {
    if (reg >= PSG_REG_COUNT) return;

    uint16_t v = value;

    psg_reg_seq++;
    __dmb();

    switch (reg) {
        case PSG_REG_FREQ_A_LO:
        case PSG_REG_FREQ_B_LO:
        case PSG_REG_FREQ_C_LO:
            // Tone period low byte.
            break;

        case PSG_REG_FREQ_A_HI:
        case PSG_REG_FREQ_B_HI:
        case PSG_REG_FREQ_C_HI:
            // Tone period high byte (4 bits)
            v &= 0x0F;
            break;

        case PSG_REG_NOISE_FREQ:
            // Noise frequency divisor (5 bits)
            v &= 0x1F;
            break;

        case PSG_REG_MIXER:
            // Mixer register: bits 0-2 = tone disable, bits 3-5 = noise disable
            psg_tone_mix[0] = (v & 0x01) ? 0 : 1;
            psg_tone_mix[1] = (v & 0x02) ? 0 : 1;
            psg_tone_mix[2] = (v & 0x04) ? 0 : 1;
            psg_noise_mix[0] = (v & 0x08) ? 0 : 1;
            psg_noise_mix[1] = (v & 0x10) ? 0 : 1;
            psg_noise_mix[2] = (v & 0x20) ? 0 : 1;
            break;

        case PSG_REG_VOL_A:
        case PSG_REG_VOL_B:
        case PSG_REG_VOL_C: {
            // Volume register: bits 0-3 = volume, bit 4 = envelope mode
            int ch_idx = reg - PSG_REG_VOL_A;
            v &= 0x1F;
            if ((v & 0x10) == 0) {
                // Fixed volume mode
                usesoundenv[ch_idx] = 0;
                env_level[ch_idx] = psg_vol_to_midi[v & 0x0F];
            } else {
                // Envelope mode — initialize envelope counter
                usesoundenv[ch_idx] = 1;
                soundenvcounter[ch_idx] = 0;
                // Set initial volume based on envelope attack direction.
                if (soundenv & 4) {
                    env_level[ch_idx] = env_vol_to_midi[0];
                } else {
                    env_level[ch_idx] = env_vol_to_midi[31];
                }
            }
            break;
        }

        case PSG_REG_ENV_LO:
            // Envelope period low byte
            set_env_speed((psg_reg[PSG_REG_ENV_HI] << 8) + v);
            break;

        case PSG_REG_ENV_HI:
            // Envelope period high byte
            set_env_speed((v << 8) + psg_reg[PSG_REG_ENV_LO]);
            break;

        case PSG_REG_ENV_SHAPE:
            // Envelope shape — writing R13 resets the envelope cycle
            // for all channels currently in envelope mode (matches real HW).
            soundenv = v & 0x0F;
            for (int i = 0; i < 3; i++) {
                if (usesoundenv[i]) {
                    soundenvcounter[i] = 0;
                    env_level[i] = (soundenv & 4) ? env_vol_to_midi[0] : env_vol_to_midi[31];
                }
            }
            break;
    }

    psg_reg[reg] = v;
    __dmb();
    psg_reg_seq++;
}

// -----------------------------------------------------------------------
// Envelope processor — called periodically from Core 1 (~50 Hz)
// Simulates the AY-3-8910/YM2149 hardware envelope generator.
//
// The hardware uses a 5-bit counter (0-31) per ramp, giving 32 steps.
// Volume 0 = silent, 31 = loudest.  Shapes match the ayumi reference
// emulator (used by the aymidi project) and the GI AY-3-8910 datasheet.
// -----------------------------------------------------------------------
static void envelope_tick(void) {
    for (int i = 0; i < 3; i++) {
        if (!usesoundenv[i]) continue;

        int prev = soundenvcounter[i] >> 16;
        soundenvcounter[i] = (soundenvcounter[i] + soundenvspeed) & 0x7FFFFFFF;
        int counter = soundenvcounter[i] >> 16;

        if (prev == counter) continue;  // No change this tick

        int volume = 0;   // 0 = silent, 31 = loudest
        int hold_max = 0; // Set to 1 for shapes that hold at full volume

        switch (soundenv) {
            case 0: case 1: case 2: case 3: case 9:
                // \___  Decay from max to zero, then hold at zero
                if (counter >= 32) { usesoundenv[i] = 0; counter = 32; }
                volume = 31 - counter;
                if (volume < 0) volume = 0;
                break;

            case 4: case 5: case 6: case 7: case 15:
                // /___  Attack from zero to max, then drop to zero
                if (counter >= 32) { usesoundenv[i] = 0; volume = 0; }
                else { volume = counter; }
                break;

            case 8:
                // \\\\  Repeating sawtooth down
                while (counter >= 32) {
                    counter -= 32;
                    soundenvcounter[i] -= (32 << 16);
                }
                volume = 31 - counter;
                if (soundenvspeed_toolarge) volume = 24;
                break;

            case 10:
                // \/\/  Triangle down-up (repeating)
                while (counter >= 64) {
                    counter -= 64;
                    soundenvcounter[i] -= (64 << 16);
                }
                if (counter < 32) volume = 31 - counter;  // down
                else volume = counter - 32;                // up
                if (soundenvspeed_toolarge) volume = 24;
                break;

            case 11:
                // \‾‾‾  Decay from max to zero, then hold at max
                if (counter >= 32) { usesoundenv[i] = 0; hold_max = 1; }
                else { volume = 31 - counter; }
                break;

            case 12:
                // ////  Repeating sawtooth up
                while (counter >= 32) {
                    counter -= 32;
                    soundenvcounter[i] -= (32 << 16);
                }
                volume = counter;
                if (soundenvspeed_toolarge) volume = 24;
                break;

            case 13:
                // /‾‾‾  Attack from zero to max, then hold at max
                if (counter >= 32) { usesoundenv[i] = 0; hold_max = 1; }
                else { volume = counter; }
                break;

            case 14:
                // /\/\  Triangle up-down (repeating)
                while (counter >= 64) {
                    counter -= 64;
                    soundenvcounter[i] -= (64 << 16);
                }
                if (counter < 32) volume = counter;       // up
                else volume = 63 - counter;                // down
                if (soundenvspeed_toolarge) volume = 24;
                break;
        }

        if (hold_max) {
            env_level[i] = 127;
        } else if (volume > 0 || usesoundenv[i]) {
            env_level[i] = env_vol_to_midi[volume & 31];
        } else {
            env_level[i] = 0;
        }
    }
}

// -----------------------------------------------------------------------
// PIO context
// -----------------------------------------------------------------------
#define IO_PIO      pio1
#define IO_SM_WRITE 0

// -----------------------------------------------------------------------
// PIO1 IRQ handler — captures PSG I/O writes
// -----------------------------------------------------------------------
static void __not_in_flash_func(midipac_pio_irq_handler)(void) {
    while (!pio_sm_is_rx_fifo_empty(IO_PIO, IO_SM_WRITE)) {
        uint32_t sample = pio_sm_get(IO_PIO, IO_SM_WRITE);
        uint8_t port = (uint8_t)(sample & 0xFFu);
        uint8_t data = (uint8_t)((sample >> 16) & 0xFFu);

        switch (port) {
            case PSG_PORT_ADDR:
                // Register address latch: MSX writes the register number to 0xA0
                psg_addr_latch = data;
                break;

            case PSG_PORT_WRITE:
                // Data write: MSX writes data to the selected register via 0xA1
                psg_write(psg_addr_latch, data);
                break;

            default:
                break;
        }
    }
}

// -----------------------------------------------------------------------
// GPIO initialisation — all pins as inputs (passive mode)
// -----------------------------------------------------------------------
static void setup_gpio(void) {
    // Address bus A0-A15 as inputs
    for (uint pin = PIN_A0; pin <= PIN_A15; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Data bus D0-D7 as inputs (never driven — passive)
    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Control signals as inputs
    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);

    // /WAIT — keep as input (passive mode, never driven)
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_IN);
}

// -----------------------------------------------------------------------
// I/O bus PIO initialisation — passive write snooper
// -----------------------------------------------------------------------
static void io_bus_init(void) {
    // Load PIO program
    uint offset_write = pio_add_program(IO_PIO, &msx_midipac_io_write_program);

    // --- SM0: I/O write snooper (passive) ---
    pio_sm_set_enabled(IO_PIO, IO_SM_WRITE, false);
    pio_sm_clear_fifos(IO_PIO, IO_SM_WRITE);
    pio_sm_restart(IO_PIO, IO_SM_WRITE);

    pio_sm_config cfg_w = msx_midipac_io_write_program_get_default_config(offset_write);
    sm_config_set_in_pins(&cfg_w, PIN_A0);
    sm_config_set_in_shift(&cfg_w, false, false, 32);
    sm_config_set_fifo_join(&cfg_w, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_w, PIN_WR);
    sm_config_set_clkdiv(&cfg_w, 1.0f);
    pio_sm_init(IO_PIO, IO_SM_WRITE, offset_write, &cfg_w);

    // Enable the state machine
    pio_sm_set_enabled(IO_PIO, IO_SM_WRITE, true);

    // Route SM0 RX-not-empty to PIO1_IRQ_0 → Core 0
    pio_set_irq0_source_enabled(IO_PIO, pis_sm0_rx_fifo_not_empty, true);
    irq_set_exclusive_handler(PIO1_IRQ_0, midipac_pio_irq_handler);
    irq_set_priority(PIO1_IRQ_0, 0);  // Highest priority
    irq_set_enabled(PIO1_IRQ_0, true);
}

// -----------------------------------------------------------------------
// GM System On SysEx: F0 7E 7F 09 01 F7
// Ensures the SC-55 starts from a clean GM state.
// -----------------------------------------------------------------------
static void midi_send_sysex_gm_on(void) {
    uint32_t save = spin_lock_blocking(tx_ring_lock);
    bool ok = (tx_ring_free() >= 6);
    if (ok) {
        tx_ring_put(0xF0);
        tx_ring_put(0x7E);
        tx_ring_put(0x7F);
        tx_ring_put(0x09);
        tx_ring_put(0x01);
        tx_ring_put(0xF7);
    }
    spin_unlock(tx_ring_lock, save);
}

// -----------------------------------------------------------------------
// Silence detection — runs at ENV_TICK_RATE_HZ from Core 1.
// If all channels are silent for >100 ms, send All Sound Off (CC#120)
// on all MIDI channels to clear any stuck notes on the target module.
// -----------------------------------------------------------------------
static void silence_detect_tick(void) {
    bool all_silent = true;
    for (int i = 0; i < MIDI_CH_COUNT; i++) {
        if (midi_note[i] != 0) { all_silent = false; break; }
    }

    if (all_silent) {
        if (!all_sound_off_sent) {
            uint32_t now = time_us_32();
            if (all_silent_since_us == 0) {
                all_silent_since_us = now;
            } else if ((now - all_silent_since_us) >= 100000) {
                for (int i = 0; i < 16; i++) {
                    midi_send_3(0xB0 | i, 120, 0);  // CC#120 All Sound Off
                }
                all_sound_off_sent = 1;
            }
        }
    } else {
        all_silent_since_us = 0;
        all_sound_off_sent = 0;
    }
}

// -----------------------------------------------------------------------
// Initialise MIDI channel state and send setup messages
// -----------------------------------------------------------------------
static void midi_init_channels(void) {
    // Send GM System On to reset the module to a known state.
    midi_send_sysex_gm_on();

    // Use brighter lead patches and a light stereo/ambience spread so the
    // three PSG tone voices feel more vivid without changing the note logic.
    for (int i = 0; i < 3; i++) {
        midi_program_set(MIDI_CH_TONE_A + i, tone_program[i]);
    }

    for (int i = 0; i < MIDI_CH_COUNT; i++) {
        midi_send_3(0xB0 | i, 7, 127);  // CC#7 Channel Volume
        midi_send_3(0xB0 | i, 10, 64);  // CC#10 default center
        midi_send_3(0xB0 | i, 91, 0);   // CC#91 default dry
        midi_send_3(0xB0 | i, 93, 0);   // CC#93 default no chorus
        midi_send_3(0xB0 | i, 11, 127); // CC#11 Expression full scale
        midi_expr[i] = 127;
    }

    for (int i = 0; i < 3; i++) {
        midi_send_3(0xB0 | (MIDI_CH_TONE_A + i), 10, tone_pan[i]);
        midi_send_3(0xB0 | (MIDI_CH_TONE_A + i), 91, tone_reverb[i]);
        midi_send_3(0xB0 | (MIDI_CH_TONE_A + i), 93, tone_chorus[i]);
    }

    midi_send_3(0xB0 | MIDI_DRUM_CH, 7, 84);   // Keep rhythm present but lift the melodic voices forward
    midi_send_3(0xB0 | MIDI_DRUM_CH, 10, 64);  // Center pan for percussion
    midi_send_3(0xB0 | MIDI_DRUM_CH, 91, 0);   // Keep drums dry
    midi_send_3(0xB0 | MIDI_DRUM_CH, 93, 0);   // No chorus on drums

    // Set pitch bend range to ±2 semitones (RPN 0, Data Entry)
    // This matches the PSG frequency table's pitch wheel sensitivity
    // (4096 wheel units per semitone × 2 semitones = ±8192 range).
    for (int i = 0; i < MIDI_CH_COUNT; i++) {
        midi_send_3(0xB0 | i, 100, 0);   // RPN LSB = 0
        midi_send_3(0xB0 | i, 101, 0);   // RPN MSB = 0 (Pitch Bend Sensitivity)
        midi_send_3(0xB0 | i, 6, 2);     // Data Entry MSB = 2 semitones
        midi_send_3(0xB0 | i, 38, 0);    // Data Entry LSB = 0
    }
}

// -----------------------------------------------------------------------
// Core 1 — TinyUSB host task + MIDI TX processing + envelope tick
// -----------------------------------------------------------------------
static void core1_entry(void) {
    tusb_init();
    tuh_init(0);

    bool init_sent = false;
    absolute_time_t next_frame_tick = get_absolute_time();
    absolute_time_t next_bend_tick = get_absolute_time();
    absolute_time_t next_midi_tx_tick = get_absolute_time();

    while (true) {
        tuh_task();
        absolute_time_t now = get_absolute_time();

        // Send initial MIDI setup once a device is connected
        if (!init_sent && usb_midi_host_mounted()) {
            midi_init_channels();
            init_sent = true;
        }

        // If device disconnected, reset state
        if (init_sent && !usb_midi_host_mounted()) {
            init_sent = false;
            next_midi_tx_tick = now;
        }

        // Feed raw MIDI bytes at standard DIN MIDI wire speed.
        // MIDI 1.0 uses 31,250 baud with 10 bits per framed byte,
        // so each byte must be spaced by 320 us.
        if (usb_midi_host_mounted() && absolute_time_diff_us(next_midi_tx_tick, now) >= 0) {
            uint8_t byte;
            if (usb_midi_host_can_accept_byte() && tx_ring_get(&byte)) {
                usb_midi_host_send_byte(byte);
                next_midi_tx_tick = delayed_by_us(now, MIDI_WIRE_BYTE_US);
            } else {
                next_midi_tx_tick = now;
            }
            usb_midi_host_flush();
        }

        midi_flush_deferred_tone_controls(time_us_32());

        if (absolute_time_diff_us(next_bend_tick, now) >= 0) {
            uint16_t snapshot[PSG_REG_COUNT];
            psg_snapshot_read(snapshot);
            process_tone_bends(snapshot);
            next_bend_tick = delayed_by_us(next_bend_tick, PSG_BEND_TICK_US);
        }

        // Process PSG state once per frame and keep envelope dynamics aligned.
        if (absolute_time_diff_us(next_frame_tick, now) >= 0) {
            envelope_tick();
            process_frame();
            silence_detect_tick();
            next_frame_tick = delayed_by_us(next_frame_tick, PSG_FRAME_TICK_US);
        }
    }
}

// -----------------------------------------------------------------------
// Main — Core 0
// -----------------------------------------------------------------------
int main(void) {
    set_sys_clock_khz(250000, true);

    // Initialize state
    tx_ring_lock = spin_lock_instance(spin_lock_claim_unused(true));
    tx_ring_head = 0;
    tx_ring_tail = 0;
    psg_addr_latch = 0;
    memset((void *)psg_reg, 0, sizeof(psg_reg));
    memset((void *)channel_freq, 0, sizeof(channel_freq));
    memset((void *)midi_note, 0, sizeof(midi_note));
    memset((void *)midi_program, 0xFF, sizeof(midi_program));
    memset((void *)midi_expr, 0, sizeof(midi_expr));
    memset((void *)psg_tone_mix, 0, sizeof(psg_tone_mix));
    memset((void *)psg_noise_mix, 0, sizeof(psg_noise_mix));
    memset((void *)midi_pitchbend_pending, 0, sizeof(midi_pitchbend_pending));
    memset((void *)midi_pitchbend_dirty, 0, sizeof(midi_pitchbend_dirty));
    memset((void *)midi_expr_pending, 0, sizeof(midi_expr_pending));
    memset((void *)midi_expr_dirty, 0, sizeof(midi_expr_dirty));
    memset((void *)midi_pitchbend_last_us, 0, sizeof(midi_pitchbend_last_us));
    memset((void *)midi_expr_last_us, 0, sizeof(midi_expr_last_us));
    memset((void *)tone_last_period, 0, sizeof(tone_last_period));
    memset((void *)env_level, 0, sizeof(env_level));
    memset((void *)tone_note_history, 0, sizeof(tone_note_history));
    memset((void *)tone_note_history_count, 0, sizeof(tone_note_history_count));
    memset((void *)tone_pending_note, 0, sizeof(tone_pending_note));
    memset((void *)tone_pending_count, 0, sizeof(tone_pending_count));
    for (int i = 0; i < MIDI_CH_COUNT; i++)
        midi_pitchbend[i] = 8192;
    soundenv = 0;
    soundenvspeed = 0;
    soundenvspeed_toolarge = 0;
    memset((void *)usesoundenv, 0, sizeof(usesoundenv));
    memset((void *)soundenvcounter, 0, sizeof(soundenvcounter));
    psg_reg_seq = 0;
    all_silent_since_us = 0;
    all_sound_off_sent = 0;

    setup_gpio();
    io_bus_init();

    multicore_launch_core1(core1_entry);

    while (true)
        __wfi();

    return 0;
}
