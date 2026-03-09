// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// midi.h - Pin definitions and constants for MSX PicoVerse USB MIDI firmware
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MIDI_H
#define MIDI_H

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
#define PIN_SLTSL  27   // /SLTSL — active-low slot select (unused by MIDI)
#define PIN_WAIT   28   // /WAIT  — active-low, driven by PIO to freeze Z80 during I/O reads
#define PIN_BUSSDIR 29  // BUSSDIR — bus direction control (unused by MIDI)

#define PICO_FLASH_SPI_CLKDIV 2

// -----------------------------------------------------------------------
// MSX-MIDI I/O port addresses (8251 USART + 8253 Timer)
// -----------------------------------------------------------------------
#define MIDI_PORT_DATA      0xE8    // 8251 data register (read: RX, write: TX)
#define MIDI_PORT_STATUS    0xE9    // 8251 status (read) / command-mode (write)
#define MIDI_PORT_TIMERACK  0xEA    // Timer interrupt acknowledge
#define MIDI_PORT_TIMERACK2 0xEB    // Mirror of 0xEA
#define MIDI_PORT_COUNTER0  0xEC    // 8253 counter 0
#define MIDI_PORT_COUNTER1  0xED    // 8253 counter 1
#define MIDI_PORT_COUNTER2  0xEE    // 8253 counter 2
#define MIDI_PORT_TIMERCTRL 0xEF    // 8253 control word

// -----------------------------------------------------------------------
// 8251 status register bits (read from port 0xE9)
// -----------------------------------------------------------------------
#define MIDI_STATUS_TXRDY   (1u << 0)   // Transmitter ready
#define MIDI_STATUS_RRDY    (1u << 1)   // Receiver ready (data available)
#define MIDI_STATUS_TXEM    (1u << 2)   // Transmitter empty
#define MIDI_STATUS_PE      (1u << 3)   // Parity error
#define MIDI_STATUS_OE      (1u << 4)   // Overrun error
#define MIDI_STATUS_FE      (1u << 5)   // Framing error
#define MIDI_STATUS_BRK     (1u << 6)   // Break detect
#define MIDI_STATUS_DSR     (1u << 7)   // 8253 timer interrupt flag

// -----------------------------------------------------------------------
// Ring buffer sizes (must be power of 2)
// -----------------------------------------------------------------------
#define MIDI_TX_BUFSIZE     256
#define MIDI_RX_BUFSIZE     64

#endif
