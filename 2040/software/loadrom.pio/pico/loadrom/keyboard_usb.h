// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// keyboard_usb.h - USB HID keyboard to MSX PPI keyboard matrix emulation

#ifndef KEYBOARD_USB_H
#define KEYBOARD_USB_H

#include <stdint.h>
#include "hardware/pio.h"

// Initialize the keyboard matrix to all keys released (0xFF).
void keyboard_init(void);

// Pass PIO1 I/O bus context to the keyboard module.
// Must be called before multicore_launch_core1(keyboard_usb_task).
void keyboard_set_io_bus(PIO pio_read, uint sm_read, PIO pio_write, uint sm_write);

// Install PIO1 IRQ handler on the current core for keyboard I/O service.
// Must be called from Core 0 before launching Core 1.
void keyboard_install_io_irq(void);

// Core 1 entry point: runs TinyUSB host task.
void keyboard_usb_task(void);

#endif // KEYBOARD_USB_H
