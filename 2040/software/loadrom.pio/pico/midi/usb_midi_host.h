// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// usb_midi_host.h - USB MIDI host class driver for TinyUSB
//
// Implements a minimal USB MIDI host driver that registers via TinyUSB's
// usbh_app_driver_get_cb() mechanism. Enumerates USB MIDI devices
// (Audio class, MIDI Streaming subclass) and provides APIs to send
// and receive raw MIDI bytes over bulk endpoints.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef USB_MIDI_HOST_H
#define USB_MIDI_HOST_H

#include <stdbool.h>
#include <stdint.h>

// Check if a MIDI device is currently mounted and ready
bool usb_midi_host_mounted(void);

// Get the device address of the mounted MIDI device (0 if none)
uint8_t usb_midi_host_dev_addr(void);

// Feed a raw MIDI byte from the MSX into the parser/sender.
// Parses MIDI stream into USB-MIDI event packets and queues them
// for transmission. Call usb_midi_host_flush() afterwards.
void usb_midi_host_send_byte(uint8_t byte);

// Returns true if the TX path can safely accept one more raw MIDI byte
// without risking packet loss in the current 64-byte USB TX buffer.
bool usb_midi_host_can_accept_byte(void);

// Flush any pending USB-MIDI event packets to the device.
// Should be called from the Core 1 USB task loop.
void usb_midi_host_flush(void);

// Read a raw MIDI byte received from the USB MIDI device.
// Returns true if a byte was available, false if RX buffer is empty.
bool usb_midi_host_receive_byte(uint8_t *byte);

// Check if there are received MIDI bytes waiting
bool usb_midi_host_rx_available(void);

#endif // USB_MIDI_HOST_H
