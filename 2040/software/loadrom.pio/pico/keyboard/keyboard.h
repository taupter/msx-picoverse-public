// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// keyboard.h - Pin definitions and constants for MSX PicoVerse USB Keyboard firmware
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef KEYBOARD_H
#define KEYBOARD_H

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
#define PIN_SLTSL  27   // /SLTSL — active-low slot select (unused by keyboard)
#define PIN_WAIT   28   // /WAIT  — active-low, driven by PIO to freeze Z80 during I/O reads
#define PIN_BUSSDIR 29  // BUSSDIR — bus direction control (unused by keyboard)

#define PICO_FLASH_SPI_CLKDIV 2

#endif
