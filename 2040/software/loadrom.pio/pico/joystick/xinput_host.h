// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// xinput_host.h - Xbox controller USB host driver for TinyUSB
//
// Supports two Xbox controller families:
//   1. Xbox 360 (XInput): subclass 0x5D, protocol 0x01
//      - 20-byte fixed reports, no initialisation needed
//   2. Xbox One / Series X|S (GIP): subclass 0x47, protocol 0xD0
//      - 18-byte reports with 4-byte GIP header, requires init packet
//
// Both use vendor-specific USB interfaces (class 0xFF) and are invisible
// to the standard TinyUSB HID host driver.  This driver registers as a
// TinyUSB custom class driver to claim these interfaces.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef XINPUT_HOST_H
#define XINPUT_HOST_H

#include <stdint.h>
#include <stdbool.h>

// -----------------------------------------------------------------------
// Controller protocol type
// -----------------------------------------------------------------------
typedef enum {
    XBOX_PROTO_360,     // Xbox 360 / XInput
    XBOX_PROTO_ONE,     // Xbox One / Series X|S (GIP)
} xbox_proto_t;

// -----------------------------------------------------------------------
// Xbox 360 (XInput) USB interface identifiers
// -----------------------------------------------------------------------
#define XINPUT_IF_CLASS     0xFFu
#define XINPUT_IF_SUBCLASS  0x5Du
#define XINPUT_IF_PROTOCOL  0x01u

// -----------------------------------------------------------------------
// Xbox One (GIP) USB interface identifiers
// -----------------------------------------------------------------------
#define GIP_IF_CLASS        0xFFu
#define GIP_IF_SUBCLASS     0x47u
#define GIP_IF_PROTOCOL     0xD0u

// -----------------------------------------------------------------------
// Xbox 360 (XInput) report format
// -----------------------------------------------------------------------
#define XINPUT_MSG_INPUT    0x00u   // Message type: input report
#define XINPUT_REPORT_LEN   20     // Fixed report length (bytes)

// Xbox 360 button bits (16-bit, little-endian)
#define XINPUT_BTN_DPAD_UP     (1u << 0)
#define XINPUT_BTN_DPAD_DOWN   (1u << 1)
#define XINPUT_BTN_DPAD_LEFT   (1u << 2)
#define XINPUT_BTN_DPAD_RIGHT  (1u << 3)
#define XINPUT_BTN_START       (1u << 4)
#define XINPUT_BTN_BACK        (1u << 5)
#define XINPUT_BTN_L3          (1u << 6)
#define XINPUT_BTN_R3          (1u << 7)
#define XINPUT_BTN_LB          (1u << 8)
#define XINPUT_BTN_RB          (1u << 9)
#define XINPUT_BTN_GUIDE       (1u << 10)
// Bit 11 is unused
#define XINPUT_BTN_A           (1u << 12)
#define XINPUT_BTN_B           (1u << 13)
#define XINPUT_BTN_X           (1u << 14)
#define XINPUT_BTN_Y           (1u << 15)

// Xbox 360 input report (20 bytes, little-endian, packed)
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;        // 0x00 for input report
    uint8_t  msg_length;      // 0x14 (20)
    uint16_t buttons;         // Digital button bitmap
    uint8_t  left_trigger;    // 0-255
    uint8_t  right_trigger;   // 0-255
    int16_t  stick_lx;        // Left stick X  (-32768 to 32767)
    int16_t  stick_ly;        // Left stick Y  (-32768 to 32767)
    int16_t  stick_rx;        // Right stick X (-32768 to 32767)
    int16_t  stick_ry;        // Right stick Y (-32768 to 32767)
    uint8_t  reserved[6];
} xinput_report_t;

// -----------------------------------------------------------------------
// Xbox One / Series X|S (GIP) report format
// -----------------------------------------------------------------------
#define GIP_CMD_INPUT       0x20u   // GIP command: input report
#define GIP_PAYLOAD_LEN     0x0Eu   // 14 bytes of gamepad payload

// Xbox One button bits (16-bit, little-endian)
// Different layout from Xbox 360!
#define GIP_BTN_SYNC          (1u << 0)
// Bit 1 unused
#define GIP_BTN_MENU          (1u << 2)   // Start
#define GIP_BTN_VIEW          (1u << 3)   // Back/Select
#define GIP_BTN_A             (1u << 4)
#define GIP_BTN_B             (1u << 5)
#define GIP_BTN_X             (1u << 6)
#define GIP_BTN_Y             (1u << 7)
#define GIP_BTN_DPAD_UP       (1u << 8)
#define GIP_BTN_DPAD_DOWN     (1u << 9)
#define GIP_BTN_DPAD_LEFT     (1u << 10)
#define GIP_BTN_DPAD_RIGHT    (1u << 11)
#define GIP_BTN_LB            (1u << 12)
#define GIP_BTN_RB            (1u << 13)
#define GIP_BTN_L3            (1u << 14)
#define GIP_BTN_R3            (1u << 15)

// Xbox One input report (18 bytes: 4-byte GIP header + 14-byte payload)
typedef struct __attribute__((packed)) {
    // GIP header
    uint8_t  command;         // 0x20 for input
    uint8_t  client;          // Usually 0x00
    uint8_t  sequence;        // Incrementing sequence number
    uint8_t  length;          // Payload length (0x0E = 14)
    // Payload
    uint16_t buttons;         // Digital button bitmap
    uint16_t left_trigger;    // 0-1023 (10-bit range, 16-bit field)
    uint16_t right_trigger;   // 0-1023
    int16_t  stick_lx;        // Left stick X  (-32768 to 32767)
    int16_t  stick_ly;        // Left stick Y  (-32768 to 32767)
    int16_t  stick_rx;        // Right stick X (-32768 to 32767)
    int16_t  stick_ry;        // Right stick Y (-32768 to 32767)
} gip_report_t;

// GIP initialisation packet - must be sent on OUT endpoint to start input
#define GIP_INIT_POWER_LEN  5

// -----------------------------------------------------------------------
// Common constants
// -----------------------------------------------------------------------

// Analog stick deadzone (approximately 25% of 32768)
#define XINPUT_STICK_DEADZONE  8192

// -----------------------------------------------------------------------
// Forward declaration of the TinyUSB class driver descriptor.
// Defined in xinput_host.c.
// -----------------------------------------------------------------------
#include "host/usbh_pvt.h"
extern usbh_class_driver_t const xinput_class_driver;

#endif /* XINPUT_HOST_H */
