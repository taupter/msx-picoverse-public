// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// keyboard_usb.c - USB HID keyboard to MSX PPI keyboard matrix emulation
//
// USB HID processing runs on Core 1 (TinyUSB host task).
// PPI I/O port service (ports 0xA9/0xAA) runs as a PIO1 IRQ handler
// on Core 0, providing deterministic response within the Z80 I/O read
// cycle (~700 ns budget).  Core 0's ROM handler loops remain untouched.

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "keyboard_usb.h"

// -----------------------------------------------------------------------
// PIO1 I/O bus context (set by loadrom.c before Core 1 launch)
// -----------------------------------------------------------------------
static PIO  kb_pio_read;
static uint kb_sm_read;
static PIO  kb_pio_write;
static uint kb_sm_write;

void keyboard_set_io_bus(PIO pio_read, uint sm_read, PIO pio_write, uint sm_write)
{
    kb_pio_read  = pio_read;
    kb_sm_read   = sm_read;
    kb_pio_write = pio_write;
    kb_sm_write  = sm_write;
}

// -----------------------------------------------------------------------
// MSX keyboard matrix state
// -----------------------------------------------------------------------
static volatile uint8_t msx_matrix[11];
static volatile uint8_t current_keyboard_row = 0;

void keyboard_init(void)
{
    memset((void *)msx_matrix, 0xFF, sizeof(msx_matrix));
    current_keyboard_row = 0;
}

// Build a response token matching the PIO I/O read responder protocol.
// Same format as pio_build_token() in loadrom.c.
static inline uint16_t kb_build_token(bool drive, uint8_t data)
{
    uint8_t dir_mask = drive ? 0xFFu : 0x00u;
    return (uint16_t)data | ((uint16_t)dir_mask << 8);
}

// -----------------------------------------------------------------------
// HID keycode → MSX matrix mapping (US layout)
// -----------------------------------------------------------------------
// Each entry: { HID usage ID, MSX row, MSX column bit position }
// Bit is CLEARED when pressed (active low).
typedef struct {
    uint8_t hid_code;
    uint8_t row;
    uint8_t col;  // bit position 0-7
} hid_to_msx_t;

static const hid_to_msx_t hid_to_msx_map[] = {
    // Row 0: 0 1 2 3 4 5 6 7
    { 0x27, 0, 0 },  // 0
    { 0x1E, 0, 1 },  // 1
    { 0x1F, 0, 2 },  // 2
    { 0x20, 0, 3 },  // 3
    { 0x21, 0, 4 },  // 4
    { 0x22, 0, 5 },  // 5
    { 0x23, 0, 6 },  // 6
    { 0x24, 0, 7 },  // 7

    // Row 1: 8 9 - = \ @ [ ;
    { 0x25, 1, 0 },  // 8
    { 0x26, 1, 1 },  // 9
    { 0x2D, 1, 2 },  // -
    { 0x2E, 1, 3 },  // = (MSX: ^)
    { 0x31, 1, 4 },  // backslash
    { 0x34, 1, 5 },  // ' (MSX: @)
    { 0x2F, 1, 6 },  // [
    { 0x33, 1, 7 },  // ;

    // Row 2: ' ` , . / DEAD A B
    { 0x30, 2, 0 },  // ] (MSX: :)
    { 0x35, 2, 1 },  // ` (MSX: ])
    { 0x36, 2, 2 },  // ,
    { 0x37, 2, 3 },  // .
    { 0x38, 2, 4 },  // /
    // 2,5 = DEAD key (no standard USB mapping)
    { 0x04, 2, 6 },  // A
    { 0x05, 2, 7 },  // B

    // Row 3: C D E F G H I J
    { 0x06, 3, 0 },  // C
    { 0x07, 3, 1 },  // D
    { 0x08, 3, 2 },  // E
    { 0x09, 3, 3 },  // F
    { 0x0A, 3, 4 },  // G
    { 0x0B, 3, 5 },  // H
    { 0x0C, 3, 6 },  // I
    { 0x0D, 3, 7 },  // J

    // Row 4: K L M N O P Q R
    { 0x0E, 4, 0 },  // K
    { 0x0F, 4, 1 },  // L
    { 0x10, 4, 2 },  // M
    { 0x11, 4, 3 },  // N
    { 0x12, 4, 4 },  // O
    { 0x13, 4, 5 },  // P
    { 0x14, 4, 6 },  // Q
    { 0x15, 4, 7 },  // R

    // Row 5: S T U V W X Y Z
    { 0x16, 5, 0 },  // S
    { 0x17, 5, 1 },  // T
    { 0x18, 5, 2 },  // U
    { 0x19, 5, 3 },  // V
    { 0x1A, 5, 4 },  // W
    { 0x1B, 5, 5 },  // X
    { 0x1C, 5, 6 },  // Y
    { 0x1D, 5, 7 },  // Z

    // Row 6: SHIFT CTRL GRAPH CAPS CODE F1 F2 F3
    // (SHIFT, CTRL, GRAPH, CODE handled via modifier byte)
    { 0x39, 6, 3 },  // Caps Lock → CAPS
    { 0x3A, 6, 5 },  // F1
    { 0x3B, 6, 6 },  // F2
    { 0x3C, 6, 7 },  // F3

    // Row 7: F4 F5 ESC TAB STOP BS SELECT RETURN
    { 0x3D, 7, 0 },  // F4
    { 0x3E, 7, 1 },  // F5
    { 0x29, 7, 2 },  // Escape → ESC
    { 0x2B, 7, 3 },  // Tab
    { 0x48, 7, 4 },  // Pause → STOP
    { 0x2A, 7, 5 },  // Backspace → BS
    { 0x49, 7, 6 },  // Insert → SELECT
    { 0x28, 7, 7 },  // Return

    // Row 8: SPACE HOME INS DEL LEFT UP DOWN RIGHT
    { 0x2C, 8, 0 },  // Space
    { 0x4A, 8, 1 },  // Home
    { 0x49, 8, 2 },  // Insert → INS
    { 0x4C, 8, 3 },  // Delete → DEL
    { 0x50, 8, 4 },  // Left
    { 0x52, 8, 5 },  // Up
    { 0x51, 8, 6 },  // Down
    { 0x4F, 8, 7 },  // Right

    // Row 9: numpad (NUM* NUM8 NUM9 NUM- NUM, NUM. NUM3 NUM4)
    { 0x55, 9, 0 },  // Numpad *
    { 0x60, 9, 1 },  // Numpad 8
    { 0x61, 9, 2 },  // Numpad 9
    { 0x56, 9, 3 },  // Numpad -
    { 0x5D, 9, 4 },  // Numpad , (use numpad 5 as substitute)
    { 0x63, 9, 5 },  // Numpad .
    { 0x5B, 9, 6 },  // Numpad 3
    { 0x5C, 9, 7 },  // Numpad 4

    // Row 10: NUM5 NUM6 NUM7 NUM8 NUM9 NUM- NUM, NUM.
    { 0x5D, 10, 0 }, // Numpad 5
    { 0x5E, 10, 1 }, // Numpad 6
    { 0x5F, 10, 2 }, // Numpad 7
    { 0x57, 10, 3 }, // Numpad + (MSX: NUM=)
    { 0x58, 10, 4 }, // Numpad Enter (MSX: numpad enter)
};

#define HID_TO_MSX_MAP_SIZE (sizeof(hid_to_msx_map) / sizeof(hid_to_msx_map[0]))

// -----------------------------------------------------------------------
// Process a boot-protocol keyboard report into the MSX matrix
// -----------------------------------------------------------------------
static void map_usb_report_to_matrix(const uint8_t report[8])
{
    uint8_t new_matrix[11];
    memset(new_matrix, 0xFF, sizeof(new_matrix));

    // Modifier byte (report[0]):
    //   bit 0 = L-Ctrl   → row 6 bit 1 (CTRL)
    //   bit 1 = L-Shift  → row 6 bit 0 (SHIFT)
    //   bit 2 = L-Alt    → row 6 bit 2 (GRAPH)
    //   bit 4 = R-Ctrl   → row 6 bit 1 (CTRL)
    //   bit 5 = R-Shift  → row 6 bit 0 (SHIFT)
    //   bit 6 = R-Alt    → row 6 bit 4 (CODE)
    uint8_t mods = report[0];

    if (mods & 0x22u)  // L-Shift or R-Shift
        new_matrix[6] &= ~(1u << 0);
    if (mods & 0x11u)  // L-Ctrl or R-Ctrl
        new_matrix[6] &= ~(1u << 1);
    if (mods & 0x04u)  // L-Alt → GRAPH
        new_matrix[6] &= ~(1u << 2);
    if (mods & 0x40u)  // R-Alt → CODE
        new_matrix[6] &= ~(1u << 4);

    // Keycodes (report[2] through report[7])
    for (int k = 2; k < 8; k++)
    {
        uint8_t code = report[k];
        if (code == 0x00u || code == 0x01u)  // No event or error roll-over
            continue;

        for (uint32_t m = 0; m < HID_TO_MSX_MAP_SIZE; m++)
        {
            if (hid_to_msx_map[m].hid_code == code)
            {
                new_matrix[hid_to_msx_map[m].row] &= ~(1u << hid_to_msx_map[m].col);
                break;
            }
        }
    }

    // Copy to shared matrix with memory barrier for cross-core visibility
    memcpy((void *)msx_matrix, new_matrix, sizeof(msx_matrix));
    __dmb();
}

// -----------------------------------------------------------------------
// TinyUSB HID host callbacks
// -----------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;

    // Request boot protocol for keyboards
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)dev_addr;
    (void)instance;

    // Reset matrix: all keys released
    keyboard_init();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                 uint8_t const *report, uint16_t len)
{
    if (len >= 8)
    {
        map_usb_report_to_matrix(report);
    }

    // Continue receiving reports
    tuh_hid_receive_report(dev_addr, instance);
}

// -----------------------------------------------------------------------
// PIO1 IRQ handler — services MSX PPI keyboard I/O on Core 0
// -----------------------------------------------------------------------
// Runs from SRAM for deterministic ISR latency.
// Drains I/O writes (port 0xAA row select) then responds to I/O reads
// (port 0xA9 column data).  Must complete within ~700 ns of /IORQ
// assertion (Z80 I/O read cycle with automatic wait state).
static void __not_in_flash_func(keyboard_pio1_irq_handler)(void)
{
    // Drain I/O writes (port 0xAA row select)
    while (!pio_sm_is_rx_fifo_empty(kb_pio_write, kb_sm_write))
    {
        uint32_t sample = pio_sm_get(kb_pio_write, kb_sm_write);
        if ((sample & 0xFFu) == 0xAAu)
            current_keyboard_row = (uint8_t)((sample >> 16) & 0x0Fu);
    }

    // Handle I/O reads (port 0xA9 column data)
    while (!pio_sm_is_rx_fifo_empty(kb_pio_read, kb_sm_read))
    {
        uint16_t io_addr = (uint16_t)pio_sm_get(kb_pio_read, kb_sm_read);
        uint8_t port = (uint8_t)(io_addr & 0xFFu);
        bool drive = false;
        uint8_t data = 0xFFu;

        if (port == 0xA9u)
        {
            drive = true;
            uint8_t row = current_keyboard_row;
            data = (row < 11u) ? msx_matrix[row] : 0xFFu;
        }

        pio_sm_put_blocking(kb_pio_read, kb_sm_read, kb_build_token(drive, data));
    }
}

// Install PIO1 IRQ handler on the current core (must be Core 0).
void keyboard_install_io_irq(void)
{
    // Route SM0 (I/O read) and SM1 (I/O write) RX-not-empty to PIO1_IRQ_0
    pio_set_irq0_source_enabled(kb_pio_read,  pis_sm0_rx_fifo_not_empty, true);
    pio_set_irq0_source_enabled(kb_pio_write, pis_sm1_rx_fifo_not_empty, true);
    irq_set_exclusive_handler(PIO1_IRQ_0, keyboard_pio1_irq_handler);
    irq_set_enabled(PIO1_IRQ_0, true);
}

// -----------------------------------------------------------------------
// Core 1 entry point — TinyUSB host only
// -----------------------------------------------------------------------
void keyboard_usb_task(void)
{
    tusb_init();
    tuh_init(0);

    while (true)
    {
        tuh_task();
    }
}
