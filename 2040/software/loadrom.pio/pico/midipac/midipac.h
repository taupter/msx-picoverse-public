// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// midipac.h - Pin definitions and constants for MSX PicoVerse MIDI-PAC firmware
//
// The MIDI-PAC firmware passively intercepts all PSG (AY-3-8910) register
// writes from MSX software and converts them to MIDI events sent to a USB
// MIDI device (e.g., Roland SoundCanvas) connected to the Pico's USB port.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MIDIPAC_H
#define MIDIPAC_H

#include <stdint.h>
#include <stdbool.h>

// -----------------------------------------------------------------------
// GPIO pin assignments (directly mapped to MSX bus signals)
// -----------------------------------------------------------------------
// Address lines (A0-A15)
#define PIN_A0     0
#define PIN_A1     1
#define PIN_A2     2
#define PIN_A3     3
#define PIN_A4     4
#define PIN_A5     5
#define PIN_A6     6
#define PIN_A7     7
#define PIN_A8     8
#define PIN_A9     9
#define PIN_A10    10
#define PIN_A11    11
#define PIN_A12    12
#define PIN_A13    13
#define PIN_A14    14
#define PIN_A15    15

// Data lines (D0-D7)
#define PIN_D0     16
#define PIN_D1     17
#define PIN_D2     18
#define PIN_D3     19
#define PIN_D4     20
#define PIN_D5     21
#define PIN_D6     22
#define PIN_D7     23

// Control signals
#define PIN_RD     24   // /RD   — active-low read strobe
#define PIN_WR     25   // /WR   — active-low write strobe
#define PIN_IORQ   26   // /IORQ — active-low I/O request
#define PIN_SLTSL  27   // /SLTSL — active-low slot select (unused)
#define PIN_WAIT   28   // /WAIT  — active-low (unused by midipac — passive mode)
#define PIN_BUSSDIR 29  // BUSSDIR — bus direction control (unused)

#define PICO_FLASH_SPI_CLKDIV 2

// -----------------------------------------------------------------------
// MSX PSG I/O ports (directly accessible by MSX software)
// -----------------------------------------------------------------------
// The AY-3-8910 PSG is addressed via three I/O ports:
//   0xA0 — Register Address Latch (write only)
//   0xA1 — Write Data to selected register (write only)
//   0xA2 — Read Data from selected register (read only — we don't need this)
#define PSG_PORT_ADDR   0xA0    // Register address latch
#define PSG_PORT_WRITE  0xA1    // Data write to selected register

// -----------------------------------------------------------------------
// PSG register definitions (AY-3-8910)
// -----------------------------------------------------------------------
#define PSG_REG_COUNT       14

#define PSG_REG_FREQ_A_LO   0   // Channel A frequency low (8 bits)
#define PSG_REG_FREQ_A_HI   1   // Channel A frequency high (4 bits)
#define PSG_REG_FREQ_B_LO   2   // Channel B frequency low (8 bits)
#define PSG_REG_FREQ_B_HI   3   // Channel B frequency high (4 bits)
#define PSG_REG_FREQ_C_LO   4   // Channel C frequency low (8 bits)
#define PSG_REG_FREQ_C_HI   5   // Channel C frequency high (4 bits)
#define PSG_REG_NOISE_FREQ   6  // Noise frequency (5 bits)
#define PSG_REG_MIXER        7  // Mixer control (tone/noise enable per channel)
#define PSG_REG_VOL_A        8  // Channel A volume (4 bits) + envelope flag (bit 4)
#define PSG_REG_VOL_B        9  // Channel B volume (4 bits) + envelope flag (bit 4)
#define PSG_REG_VOL_C       10  // Channel C volume (4 bits) + envelope flag (bit 4)
#define PSG_REG_ENV_LO      11  // Envelope period low (8 bits)
#define PSG_REG_ENV_HI      12  // Envelope period high (8 bits)
#define PSG_REG_ENV_SHAPE   13  // Envelope shape (4 bits)

// -----------------------------------------------------------------------
// MIDI channel assignments for PSG voices
// -----------------------------------------------------------------------
// We use 6 MIDI channels: 3 for tone, 3 for noise shadows
// Channels 0-2: PSG tone channels A, B, C
// Channels 3-5: PSG noise channels A, B, C
#define MIDI_CH_TONE_A      0
#define MIDI_CH_TONE_B      1
#define MIDI_CH_TONE_C      2
#define MIDI_CH_NOISE_A     3
#define MIDI_CH_NOISE_B     4
#define MIDI_CH_NOISE_C     5
#define MIDI_CH_COUNT       6
#define MIDI_DRUM_CH        9

// -----------------------------------------------------------------------
// MIDI program numbers (General MIDI)
// -----------------------------------------------------------------------
#define MIDIPAC_MELODIC_PROG     80  // Square Lead (GM #81)
#define MIDIPAC_MELODIC_PROG_A   80  // Square Lead (GM #81)
#define MIDIPAC_MELODIC_PROG_B   80  // Square Lead (GM #81)
#define MIDIPAC_MELODIC_PROG_C   80  // Square Lead (GM #81)

// -----------------------------------------------------------------------
// PSG frequency table: maps 12-bit PSG divisor to MIDI note + pitch wheel
// -----------------------------------------------------------------------
typedef struct {
    uint8_t  note;           // MIDI note number (0-127)
    uint16_t wheel;          // Pitch bend value (0-16383, center = 8192)
    uint16_t fine_note_x256; // Exact MIDI note * 256 (unused at runtime)
} psg_midi_freq_t;

// Don't generate MIDI notes for very low frequency divisors
#define PSG_MIN_FREQ_DIVISOR    10

// Maximum envelope speed before clamping
#define PSG_MAX_ENVSPEED        (5 * 65536)

// -----------------------------------------------------------------------
// Ring buffer sizes for MIDI event queue (must be power of 2)
// -----------------------------------------------------------------------
#define MIDIPAC_TX_BUFSIZE      1024

#endif // MIDIPAC_H
