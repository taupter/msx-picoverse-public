// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// hid_gamepad_parser.c - Lightweight HID report descriptor parser for USB gamepads
//
// Walks the raw HID report descriptor byte stream, tracking the global/
// local item state as defined by the USB HID specification (section 6.2.2).
// When an Input item is encountered inside a Gamepad or Joystick usage
// context, the parser records the bit offset and size of recognised
// fields (hat switch, X/Y axes, buttons).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "hid_gamepad_parser.h"

// -----------------------------------------------------------------------
// HID item tag constants (short items only — long items are skipped)
// -----------------------------------------------------------------------

// Main items
#define HID_INPUT           0x80
#define HID_OUTPUT          0x90
#define HID_COLLECTION      0xA0
#define HID_FEATURE         0xB0
#define HID_END_COLLECTION  0xC0

// Global items
#define HID_USAGE_PAGE      0x04
#define HID_LOGICAL_MIN     0x14
#define HID_LOGICAL_MAX     0x24
#define HID_PHYSICAL_MIN    0x34
#define HID_PHYSICAL_MAX    0x44
#define HID_REPORT_SIZE     0x74
#define HID_REPORT_ID       0x84
#define HID_REPORT_COUNT    0x94

// Local items
#define HID_USAGE           0x08
#define HID_USAGE_MIN       0x18
#define HID_USAGE_MAX       0x28

// Usage Pages
#define UP_GENERIC_DESKTOP  0x01
#define UP_BUTTON           0x09

// Generic Desktop usages
#define GD_JOYSTICK         0x04
#define GD_GAMEPAD          0x05
#define GD_X                0x30
#define GD_Y                0x31
#define GD_HAT_SWITCH       0x39

// -----------------------------------------------------------------------
// Internal parser state
// -----------------------------------------------------------------------
typedef struct {
    // Global state
    uint16_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t  report_id;

    // Local state (cleared after each Main item)
    uint16_t usages[32];
    uint8_t  usage_count;
    uint16_t usage_min;
    uint16_t usage_max;

    // Tracking
    uint16_t bit_position;      // Current bit offset within the report
    bool     in_gamepad;        // Inside a Gamepad/Joystick collection
    uint8_t  collection_depth;  // Nesting depth
    uint8_t  gp_depth;          // Depth at which the gamepad collection was opened
} parser_state_t;

// -----------------------------------------------------------------------
// Read a signed integer of 1–4 bytes from the descriptor (little-endian)
// -----------------------------------------------------------------------
static int32_t read_signed(const uint8_t *data, uint8_t size)
{
    switch (size) {
        case 1: return (int8_t)data[0];
        case 2: return (int16_t)(data[0] | ((uint16_t)data[1] << 8));
        case 4: return (int32_t)(data[0] | ((uint32_t)data[1] << 8) |
                                 ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
        default: return 0;
    }
}

// -----------------------------------------------------------------------
// Read an unsigned integer of 1–4 bytes from the descriptor
// -----------------------------------------------------------------------
static uint32_t read_unsigned(const uint8_t *data, uint8_t size)
{
    switch (size) {
        case 1: return data[0];
        case 2: return data[0] | ((uint16_t)data[1] << 8);
        case 4: return data[0] | ((uint32_t)data[1] << 8) |
                       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        default: return 0;
    }
}

// -----------------------------------------------------------------------
// Clear local item state (called after each Main item)
// -----------------------------------------------------------------------
static void clear_local(parser_state_t *s)
{
    s->usage_count = 0;
    s->usage_min = 0;
    s->usage_max = 0;
}

// -----------------------------------------------------------------------
// Extract a field value from a report at an arbitrary bit offset/size
// -----------------------------------------------------------------------
static uint32_t extract_bits(const uint8_t *report, uint16_t report_len_bits,
                             uint16_t bit_offset, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size > 32)
        return 0;
    if ((uint32_t)bit_offset + bit_size > report_len_bits)
        return 0;

    uint32_t value = 0;
    for (uint8_t i = 0; i < bit_size; i++) {
        uint16_t abs_bit = bit_offset + i;
        uint8_t byte_idx = abs_bit / 8;
        uint8_t bit_idx  = abs_bit % 8;
        if (report[byte_idx] & (1u << bit_idx))
            value |= (1u << i);
    }
    return value;
}

// -----------------------------------------------------------------------
// Sign-extend a value given its bit width
// -----------------------------------------------------------------------
static int32_t sign_extend(uint32_t value, uint8_t bits)
{
    if (bits == 0 || bits >= 32) return (int32_t)value;
    if (value & (1u << (bits - 1)))
        value |= ~((1u << bits) - 1);
    return (int32_t)value;
}

// -----------------------------------------------------------------------
// gp_parse_descriptor
// -----------------------------------------------------------------------
bool gp_parse_descriptor(const uint8_t *desc, uint16_t desc_len,
                         gamepad_layout_t *out)
{
    memset(out, 0, sizeof(*out));

    parser_state_t s;
    memset(&s, 0, sizeof(s));

    uint16_t pos = 0;

    while (pos < desc_len) {
        uint8_t prefix = desc[pos];

        // Long items (bTag = 0xFE, bType = 3): skip
        if (prefix == 0xFEu) {
            if (pos + 2 >= desc_len) break;
            uint8_t data_size = desc[pos + 1];
            pos += 3 + data_size;
            continue;
        }

        // Short item: prefix encodes bSize (bits 0-1), bType (2-3), bTag (4-7)
        uint8_t bSize = prefix & 0x03u;
        if (bSize == 3) bSize = 4;  // bSize=3 means 4 bytes
        uint8_t bTag_bType = prefix & 0xFCu;

        if (pos + 1 + bSize > desc_len) break;
        const uint8_t *item_data = &desc[pos + 1];

        // ---- Global items ----
        switch (bTag_bType) {
            case HID_USAGE_PAGE:
                s.usage_page = (uint16_t)read_unsigned(item_data, bSize);
                break;

            case HID_LOGICAL_MIN:
                s.logical_min = read_signed(item_data, bSize);
                break;

            case HID_LOGICAL_MAX:
                s.logical_max = read_signed(item_data, bSize);
                break;

            case HID_REPORT_SIZE:
                s.report_size = read_unsigned(item_data, bSize);
                break;

            case HID_REPORT_COUNT:
                s.report_count = read_unsigned(item_data, bSize);
                break;

            case HID_REPORT_ID:
                s.report_id = (uint8_t)read_unsigned(item_data, bSize);
                if (!out->report_id)
                    out->report_id = s.report_id;
                break;

            // ---- Local items ----
            case HID_USAGE:
                if (s.usage_count < 32)
                    s.usages[s.usage_count++] = (uint16_t)read_unsigned(item_data, bSize);
                break;

            case HID_USAGE_MIN:
                s.usage_min = (uint16_t)read_unsigned(item_data, bSize);
                break;

            case HID_USAGE_MAX:
                s.usage_max = (uint16_t)read_unsigned(item_data, bSize);
                break;

            // ---- Main items ----
            case HID_COLLECTION: {
                s.collection_depth++;
                // Check if this is a Gamepad or Joystick application collection
                if (s.usage_page == UP_GENERIC_DESKTOP && s.usage_count > 0) {
                    uint16_t u = s.usages[0];
                    if (u == GD_GAMEPAD || u == GD_JOYSTICK) {
                        s.in_gamepad = true;
                        s.gp_depth = s.collection_depth;
                    }
                }
                clear_local(&s);
                break;
            }

            case HID_END_COLLECTION:
                if (s.collection_depth > 0) {
                    if (s.in_gamepad && s.collection_depth == s.gp_depth)
                        s.in_gamepad = false;
                    s.collection_depth--;
                }
                clear_local(&s);
                break;

            case HID_INPUT: {
                // Only process fields inside a gamepad/joystick collection
                // or if no collection was found yet (some descriptors are flat)
                uint8_t input_flags = (bSize > 0) ? item_data[0] : 0;
                bool is_constant = (input_flags & 0x01u) != 0;

                uint32_t total_bits = s.report_count * s.report_size;

                if (s.in_gamepad && !is_constant) {
                    // --- Hat switch ---
                    if (s.usage_page == UP_GENERIC_DESKTOP &&
                        s.usage_count > 0 && s.usages[0] == GD_HAT_SWITCH &&
                        !out->hat.present) {
                        out->hat.bit_offset  = s.bit_position;
                        out->hat.bit_size    = (uint8_t)s.report_size;
                        out->hat.logical_min = s.logical_min;
                        out->hat.logical_max = s.logical_max;
                        out->hat.present     = true;
                    }
                    // --- X / Y axes ---
                    else if (s.usage_page == UP_GENERIC_DESKTOP) {
                        for (uint32_t i = 0; i < s.report_count && i < s.usage_count; i++) {
                            uint16_t u = s.usages[i];
                            uint16_t field_bit = s.bit_position + (uint16_t)(i * s.report_size);
                            if (u == GD_X && !out->x.present) {
                                out->x.bit_offset  = field_bit;
                                out->x.bit_size    = (uint8_t)s.report_size;
                                out->x.logical_min = s.logical_min;
                                out->x.logical_max = s.logical_max;
                                out->x.present     = true;
                            } else if (u == GD_Y && !out->y.present) {
                                out->y.bit_offset  = field_bit;
                                out->y.bit_size    = (uint8_t)s.report_size;
                                out->y.logical_min = s.logical_min;
                                out->y.logical_max = s.logical_max;
                                out->y.present     = true;
                            }
                        }
                    }
                    // --- Buttons ---
                    else if (s.usage_page == UP_BUTTON) {
                        uint32_t count = s.report_count;
                        for (uint32_t i = 0; i < count; i++) {
                            if (out->button_count >= GP_MAX_BUTTONS) break;
                            uint8_t idx = out->button_count;
                            out->buttons[idx].bit_offset  = s.bit_position + (uint16_t)(i * s.report_size);
                            out->buttons[idx].bit_size    = (uint8_t)s.report_size;
                            out->buttons[idx].logical_min = s.logical_min;
                            out->buttons[idx].logical_max = s.logical_max;
                            out->buttons[idx].present     = true;
                            out->button_count++;
                        }
                    }
                }

                // Advance bit position regardless
                s.bit_position += (uint16_t)total_bits;
                clear_local(&s);
                break;
            }

            case HID_OUTPUT:
            case HID_FEATURE: {
                // Skip output/feature items but still clear local state
                uint32_t total_bits = s.report_count * s.report_size;
                (void)total_bits; // Output/Feature bits are in a separate report
                clear_local(&s);
                break;
            }

            default:
                break;
        }

        pos += 1 + bSize;
    }

    out->valid = out->hat.present || (out->x.present && out->y.present);
    return out->valid;
}

// -----------------------------------------------------------------------
// Hat switch value → direction bits mapping
// -----------------------------------------------------------------------
// Standard 8-direction hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
// Null state (no direction) is logical_max + 1 or outside 0-7.
//
// Returns active-low direction bits: bit0=Up, bit1=Right, bit2=Down, bit3=Left

static const uint8_t hat_to_dir[8] = {
    /* 0 N  */ (uint8_t)~(1u << 0),                        // Up
    /* 1 NE */ (uint8_t)~((1u << 0) | (1u << 3)),          // Up + Right
    /* 2 E  */ (uint8_t)~(1u << 3),                        // Right
    /* 3 SE */ (uint8_t)~((1u << 1) | (1u << 3)),          // Down + Right
    /* 4 S  */ (uint8_t)~(1u << 1),                        // Down
    /* 5 SW */ (uint8_t)~((1u << 1) | (1u << 2)),          // Down + Left
    /* 6 W  */ (uint8_t)~(1u << 2),                        // Left
    /* 7 NW */ (uint8_t)~((1u << 0) | (1u << 2)),          // Up + Left
};

// -----------------------------------------------------------------------
// gp_extract_joystick
// -----------------------------------------------------------------------
uint8_t gp_extract_joystick(const uint8_t *report, uint16_t report_len,
                            const gamepad_layout_t *layout,
                            uint8_t deadzone_pct)
{
    uint8_t result = 0xFF;  // All bits high = nothing pressed
    uint16_t report_len_bits = report_len * 8;

    // --- Hat switch → directions ---
    if (layout->hat.present) {
        uint32_t raw = extract_bits(report, report_len_bits,
                                    layout->hat.bit_offset, layout->hat.bit_size);
        int32_t val = (int32_t)raw;
        // Normalise: if logical_min != 0, subtract it
        val -= layout->hat.logical_min;
        if (val >= 0 && val <= 7)
            result &= hat_to_dir[val];
        // else: null position, no direction bits
    }

    // --- Analog stick → directions (with deadzone) ---
    if (layout->x.present && layout->y.present) {
        int32_t range_x = layout->x.logical_max - layout->x.logical_min;
        int32_t range_y = layout->y.logical_max - layout->y.logical_min;

        if (range_x > 0 && range_y > 0) {
            uint32_t raw_x = extract_bits(report, report_len_bits,
                                          layout->x.bit_offset, layout->x.bit_size);
            uint32_t raw_y = extract_bits(report, report_len_bits,
                                          layout->y.bit_offset, layout->y.bit_size);

            // Sign-extend if logical_min is negative
            int32_t x, y;
            if (layout->x.logical_min < 0) {
                x = sign_extend(raw_x, layout->x.bit_size);
                y = sign_extend(raw_y, layout->y.bit_size);
            } else {
                x = (int32_t)raw_x;
                y = (int32_t)raw_y;
            }

            // Normalise to center = 0
            int32_t center_x = layout->x.logical_min + range_x / 2;
            int32_t center_y = layout->y.logical_min + range_y / 2;
            int32_t dx = x - center_x;
            int32_t dy = y - center_y;

            int32_t threshold_x = (range_x * deadzone_pct) / 200;
            int32_t threshold_y = (range_y * deadzone_pct) / 200;
            if (threshold_x < 1) threshold_x = 1;
            if (threshold_y < 1) threshold_y = 1;

            if (dx < -threshold_x) result &= ~(1u << 2); // Left
            if (dx >  threshold_x) result &= ~(1u << 3); // Right
            if (dy < -threshold_y) result &= ~(1u << 0); // Up
            if (dy >  threshold_y) result &= ~(1u << 1); // Down
        }
    }

    // --- Buttons → triggers ---
    // Button 1 (index 0) → Trigger A (bit 4)
    // Button 2 (index 1) → Trigger B (bit 5)
    if (layout->button_count > 0 && layout->buttons[0].present) {
        uint32_t val = extract_bits(report, report_len_bits,
                                    layout->buttons[0].bit_offset,
                                    layout->buttons[0].bit_size);
        if (val) result &= ~(1u << 4);
    }
    if (layout->button_count > 1 && layout->buttons[1].present) {
        uint32_t val = extract_bits(report, report_len_bits,
                                    layout->buttons[1].bit_offset,
                                    layout->buttons[1].bit_size);
        if (val) result &= ~(1u << 5);
    }

    return result;
}
