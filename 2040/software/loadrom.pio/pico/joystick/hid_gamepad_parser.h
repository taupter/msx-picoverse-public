// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// hid_gamepad_parser.h - Lightweight HID report descriptor parser for USB gamepads
//
// Parses a raw HID report descriptor to locate the bit offsets, sizes,
// and logical ranges of common gamepad fields (hat switch, X/Y axes,
// buttons).  At runtime, extracts those fields from incoming HID reports
// and maps them to an MSX-format joystick byte.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef HID_GAMEPAD_PARSER_H
#define HID_GAMEPAD_PARSER_H

#include <stdint.h>
#include <stdbool.h>

// Maximum number of button fields we track
#define GP_MAX_BUTTONS 16

// -----------------------------------------------------------------------
// Parsed field descriptor — describes one item inside the HID report
// -----------------------------------------------------------------------
typedef struct {
    uint16_t bit_offset;    // Bit position within the report (after report ID)
    uint8_t  bit_size;      // Number of bits for this field
    int32_t  logical_min;   // Logical minimum declared by the descriptor
    int32_t  logical_max;   // Logical maximum declared by the descriptor
    bool     present;       // true if this field was found in the descriptor
} gp_field_t;

// -----------------------------------------------------------------------
// Complete layout of a gamepad's HID report
// -----------------------------------------------------------------------
typedef struct {
    gp_field_t hat;                     // Hat switch (D-pad, typically 0-7 or 0-8)
    gp_field_t x;                       // X axis (left stick horizontal)
    gp_field_t y;                       // Y axis (left stick vertical)
    gp_field_t buttons[GP_MAX_BUTTONS]; // Button fields (indexed from 0)
    uint8_t    button_count;            // Number of buttons found
    uint8_t    report_id;               // Report ID (0 if none)
    bool       valid;                   // true if at least hat or axes were found
} gamepad_layout_t;

// -----------------------------------------------------------------------
// Parse a HID report descriptor to locate gamepad fields.
//
// desc     — raw HID report descriptor bytes
// desc_len — length of the descriptor
// out      — receives the parsed layout
//
// Returns true if a usable gamepad layout was found.
// -----------------------------------------------------------------------
bool gp_parse_descriptor(const uint8_t *desc, uint16_t desc_len,
                         gamepad_layout_t *out);

// -----------------------------------------------------------------------
// Extract MSX joystick bits from a runtime HID report.
//
// report       — the incoming HID report bytes (after stripping report ID
//                if report_id != 0)
// report_len   — length of the report
// layout       — previously parsed layout
// deadzone_pct — analog stick deadzone as a percentage (0–100)
//
// Returns an MSX-format active-low byte:
//   bit 0 = Up, bit 1 = Right, bit 2 = Down, bit 3 = Left
//   bit 4 = Trigger A, bit 5 = Trigger B
//   bits 6-7 = 1 (unused, always high)
//   0xFF = idle (nothing pressed)
// -----------------------------------------------------------------------
uint8_t gp_extract_joystick(const uint8_t *report, uint16_t report_len,
                            const gamepad_layout_t *layout,
                            uint8_t deadzone_pct);

#endif /* HID_GAMEPAD_PARSER_H */
