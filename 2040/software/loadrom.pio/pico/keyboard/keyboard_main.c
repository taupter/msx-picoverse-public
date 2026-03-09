// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// keyboard_main.c - Standalone USB Keyboard → MSX PPI firmware
//
// Turns the PicoVerse 2040 cartridge into a dedicated USB keyboard interface.
// Intercepts MSX I/O ports:
//   0xA9 (IN)  — returns keyboard column data for the selected row
//   0xAA (OUT) — selects the active keyboard row (PPI port C full write)
//   0xAB (OUT) — PPI port C bit set/reset (MSX BIOS uses this)
//
// Architecture:
//   Core 0: PIO1 IRQ handler services I/O bus requests.
//           The I/O read PIO asserts /WAIT so the Z80 is frozen until
//           Core 0 supplies the keyboard data.  Main loop idles (wfi).
//   Core 1: TinyUSB host task processes USB HID keyboard reports and
//           updates the MSX keyboard matrix.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "keyboard.h"
#include "msx_keyboard.pio.h"

// -----------------------------------------------------------------------
// MSX keyboard matrix — 16 entries (rows 0-10 used), active-low
// All entries start at 0xFF = no key pressed
// -----------------------------------------------------------------------
static volatile uint8_t keys[16];
static volatile uint8_t keyboard_row;

static void keyboard_reset(void)
{
    for (int i = 0; i < 16; i++)
        keys[i] = 0xFF;
    keyboard_row = 0;
}

// -----------------------------------------------------------------------
// PIO1 context
// -----------------------------------------------------------------------
#define IO_PIO      pio1
#define IO_SM_READ  0
#define IO_SM_WRITE 1

// -----------------------------------------------------------------------
// Build a 16-bit response token for the I/O read PIO:
//   bits[7:0]  = data byte
//   bits[15:8] = pindirs (0xFF = drive, 0x00 = float)
// -----------------------------------------------------------------------
static inline uint16_t __not_in_flash_func(build_token)(bool drive, uint8_t data)
{
    return drive ? ((uint16_t)0xFF00u | data) : (uint16_t)data;
}

// -----------------------------------------------------------------------
// PIO1 IRQ handler — services I/O reads and writes
// -----------------------------------------------------------------------
static void __not_in_flash_func(keyboard_pio1_irq_handler)(void)
{
    // --- Handle I/O writes first (ports 0xAA, 0xAB) ---
    while (!pio_sm_is_rx_fifo_empty(IO_PIO, IO_SM_WRITE))
    {
        uint32_t sample = pio_sm_get(IO_PIO, IO_SM_WRITE);
        uint8_t port = (uint8_t)(sample & 0xFFu);
        uint8_t data = (uint8_t)((sample >> 16) & 0xFFu);

        if (port == 0xAAu)
        {
            // PPI port C full write — keyboard row in lower nibble
            keyboard_row = data;
        }
        else if (port == 0xABu)
        {
            // PPI port C bit set/reset (8255 mode):
            //   bit 7 = 1 → set/reset mode active
            //   bits 3-1 = bit number (0-7)
            //   bit 0 = value (1=set, 0=reset)
            // MSX keyboard row is port C lower nibble (bits 0-3).
            if ((data >> 7) & 1)
            {
                uint8_t bit_num = (data >> 1) & 0x07u;
                switch (bit_num)
                {
                    case 0: keyboard_row = (data & 1) ? (keyboard_row | (1u << 0)) : (keyboard_row & ~(1u << 0)); break;
                    case 1: keyboard_row = (data & 1) ? (keyboard_row | (1u << 1)) : (keyboard_row & ~(1u << 1)); break;
                    case 2: keyboard_row = (data & 1) ? (keyboard_row | (1u << 2)) : (keyboard_row & ~(1u << 2)); break;
                    case 3: keyboard_row = (data & 1) ? (keyboard_row | (1u << 3)) : (keyboard_row & ~(1u << 3)); break;
                    default: break;
                }
            }
        }
    }

    // --- Handle I/O reads (port 0xA9) ---
    // The PIO asserts /WAIT and is stalled on pull — we MUST respond
    // to every read or the Z80 hangs.
    while (!pio_sm_is_rx_fifo_empty(IO_PIO, IO_SM_READ))
    {
        uint16_t addr = (uint16_t)pio_sm_get(IO_PIO, IO_SM_READ);
        uint8_t port = (uint8_t)(addr & 0xFFu);

        if (port == 0xA9u)
        {
            uint8_t row = keyboard_row & 0x0Fu;
            pio_sm_put(IO_PIO, IO_SM_READ, build_token(true, keys[row]));
        }
        else
        {
            // Not our port — release with tri-state (0xFF on bus, no drive)
            pio_sm_put(IO_PIO, IO_SM_READ, build_token(false, 0xFFu));
        }
    }
}

// -----------------------------------------------------------------------
// GPIO initialisation — set every bus pin to a safe state before PIO
// -----------------------------------------------------------------------
static void setup_gpio(void)
{
    // Address bus A0-A15 as inputs
    for (uint pin = PIN_A0; pin <= PIN_A15; ++pin)
    {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Data bus D0-D7 — managed by PIO later, start as input
    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Control signals as inputs
    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);

    // /WAIT — start HIGH (released) so Z80 is not frozen during boot
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 1);
}

// -----------------------------------------------------------------------
// I/O bus PIO initialisation
// -----------------------------------------------------------------------
static void io_bus_init(void)
{
    // Load PIO programs
    uint offset_read  = pio_add_program(IO_PIO, &msx_kb_io_read_program);
    uint offset_write = pio_add_program(IO_PIO, &msx_kb_io_write_program);

    // --- SM0: I/O read responder (with /WAIT) ---
    pio_sm_set_enabled(IO_PIO, IO_SM_READ, false);
    pio_sm_clear_fifos(IO_PIO, IO_SM_READ);
    pio_sm_restart(IO_PIO, IO_SM_READ);

    pio_sm_config cfg_r = msx_kb_io_read_program_get_default_config(offset_read);
    sm_config_set_in_pins(&cfg_r, PIN_A0);
    sm_config_set_in_shift(&cfg_r, false, false, 16);
    sm_config_set_out_pins(&cfg_r, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_r, true, false, 32);
    sm_config_set_sideset_pins(&cfg_r, PIN_WAIT);
    sm_config_set_jmp_pin(&cfg_r, PIN_RD);
    sm_config_set_clkdiv(&cfg_r, 1.0f);
    pio_sm_init(IO_PIO, IO_SM_READ, offset_read, &cfg_r);

    // Set /WAIT pin HIGH in the PIO output register BEFORE switching mux.
    // This prevents a brief /WAIT=LOW glitch that would freeze the Z80.
    pio_sm_set_pins_with_mask(IO_PIO, IO_SM_READ, (1u << PIN_WAIT), (1u << PIN_WAIT));

    // Now hand /WAIT to PIO1 — the output register already has it HIGH
    pio_gpio_init(IO_PIO, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(IO_PIO, IO_SM_READ, PIN_WAIT, 1, true);

    // Data bus D0-D7: hand to PIO1 for bidirectional control
    for (int i = PIN_D0; i <= PIN_D7; i++)
        pio_gpio_init(IO_PIO, i);
    pio_sm_set_consecutive_pindirs(IO_PIO, IO_SM_READ, PIN_D0, 8, false);

    // --- SM1: I/O write captor ---
    pio_sm_set_enabled(IO_PIO, IO_SM_WRITE, false);
    pio_sm_clear_fifos(IO_PIO, IO_SM_WRITE);
    pio_sm_restart(IO_PIO, IO_SM_WRITE);

    pio_sm_config cfg_w = msx_kb_io_write_program_get_default_config(offset_write);
    sm_config_set_in_pins(&cfg_w, PIN_A0);
    sm_config_set_in_shift(&cfg_w, false, false, 32);
    sm_config_set_fifo_join(&cfg_w, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_w, PIN_WR);
    sm_config_set_clkdiv(&cfg_w, 1.0f);
    pio_sm_init(IO_PIO, IO_SM_WRITE, offset_write, &cfg_w);

    // --- Enable both state machines ---
    pio_sm_set_enabled(IO_PIO, IO_SM_READ,  true);
    pio_sm_set_enabled(IO_PIO, IO_SM_WRITE, true);

    // --- Route SM0+SM1 RX-not-empty to PIO1_IRQ_0 → Core 0 ---
    pio_set_irq0_source_enabled(IO_PIO, pis_sm0_rx_fifo_not_empty, true);
    pio_set_irq0_source_enabled(IO_PIO, pis_sm1_rx_fifo_not_empty, true);
    irq_set_exclusive_handler(PIO1_IRQ_0, keyboard_pio1_irq_handler);
    irq_set_priority(PIO1_IRQ_0, 0);  // Highest priority
    irq_set_enabled(PIO1_IRQ_0, true);
}

// -----------------------------------------------------------------------
// USB HID → MSX matrix mapping
// -----------------------------------------------------------------------
// Matrix format: keys[row] &= ~(1 << col) to press a key.

static void __not_in_flash_func(map_hid_report)(const uint8_t report[8])
{
    uint8_t new_keys[16];
    for (int i = 0; i < 16; i++)
        new_keys[i] = 0xFF;

    uint8_t mods = report[0];

    // Modifiers → row 6
    if (((mods >> 5) & 1) || ((mods >> 1) & 1)) new_keys[6] &= 0xFE;  // Shift
    if (((mods >> 4) & 1) || ((mods >> 0) & 1)) new_keys[6] &= 0xFD;  // Ctrl
    if ((mods >> 2) & 1) new_keys[6] &= 0xFB;                          // Graph (L-Alt)
    if ((mods >> 6) & 1) new_keys[6] &= 0xEF;                          // Code (R-Alt)

    // Keycodes (report[2]..report[7])
    for (int i = 2; i < 8; i++)
    {
        if (report[i] == 0x00 || report[i] == 0x01)
            continue;

        switch (report[i])
        {
            // Row 0: 0 1 2 3 4 5 6 7
            case 0x27: new_keys[0] &= 0xFE; break; // 0
            case 0x1E: new_keys[0] &= 0xFD; break; // 1
            case 0x1F: new_keys[0] &= 0xFB; break; // 2
            case 0x20: new_keys[0] &= 0xF7; break; // 3
            case 0x21: new_keys[0] &= 0xEF; break; // 4
            case 0x22: new_keys[0] &= 0xDF; break; // 5
            case 0x23: new_keys[0] &= 0xBF; break; // 6
            case 0x24: new_keys[0] &= 0x7F; break; // 7

            // Row 1: 8 9 - = \ [ ] ;
            case 0x25: new_keys[1] &= 0xFE; break; // 8
            case 0x26: new_keys[1] &= 0xFD; break; // 9
            case 0x2D: new_keys[1] &= 0xFB; break; // -
            case 0x2E: new_keys[1] &= 0xF7; break; // =  (+ on MSX)
            case 0x31: new_keys[1] &= 0xEF; break; // backslash
            case 0x2F: new_keys[1] &= 0xDF; break; // [
            case 0x30: new_keys[1] &= 0xBF; break; // ]
            case 0x33: new_keys[1] &= 0x7F; break; // ;

            // Row 2: ' ` , . / dead A B
            case 0x34: new_keys[2] &= 0xFE; break; // '
            case 0x32: new_keys[2] &= 0xFD; break; // `
            case 0x36: new_keys[2] &= 0xFB; break; // ,
            case 0x37: new_keys[2] &= 0xF7; break; // .
            case 0x38: new_keys[2] &= 0xEF; break; // /
            case 0x04: new_keys[2] &= 0xBF; break; // A
            case 0x05: new_keys[2] &= 0x7F; break; // B

            // Row 3: C D E F G H I J
            case 0x06: new_keys[3] &= 0xFE; break; // C
            case 0x07: new_keys[3] &= 0xFD; break; // D
            case 0x08: new_keys[3] &= 0xFB; break; // E
            case 0x09: new_keys[3] &= 0xF7; break; // F
            case 0x0A: new_keys[3] &= 0xEF; break; // G
            case 0x0B: new_keys[3] &= 0xDF; break; // H
            case 0x0C: new_keys[3] &= 0xBF; break; // I
            case 0x0D: new_keys[3] &= 0x7F; break; // J

            // Row 4: K L M N O P Q R
            case 0x0E: new_keys[4] &= 0xFE; break; // K
            case 0x0F: new_keys[4] &= 0xFD; break; // L
            case 0x10: new_keys[4] &= 0xFB; break; // M
            case 0x11: new_keys[4] &= 0xF7; break; // N
            case 0x12: new_keys[4] &= 0xEF; break; // O
            case 0x13: new_keys[4] &= 0xDF; break; // P
            case 0x14: new_keys[4] &= 0xBF; break; // Q
            case 0x15: new_keys[4] &= 0x7F; break; // R

            // Row 5: S T U V W X Y Z
            case 0x16: new_keys[5] &= 0xFE; break; // S
            case 0x17: new_keys[5] &= 0xFD; break; // T
            case 0x18: new_keys[5] &= 0xFB; break; // U
            case 0x19: new_keys[5] &= 0xF7; break; // V
            case 0x1A: new_keys[5] &= 0xEF; break; // W
            case 0x1B: new_keys[5] &= 0xDF; break; // X
            case 0x1C: new_keys[5] &= 0xBF; break; // Y
            case 0x1D: new_keys[5] &= 0x7F; break; // Z

            // Row 6: SHIFT CTRL GRAPH CAPS CODE F1 F2 F3
            case 0xE1:                               // Left Shift
            case 0xE5: new_keys[6] &= 0xFE; break;  // Right Shift
            case 0xE0:                               // Left Ctrl
            case 0xE4: new_keys[6] &= 0xFD; break;  // Right Ctrl
            case 0xE2: new_keys[6] &= 0xFB; break;  // Graph (L-Alt)
            case 0x39: new_keys[6] &= 0xF7; break;  // CAPS
            case 0xE6: new_keys[6] &= 0xEF; break;  // Code (R-Alt)
            case 0x3A: new_keys[6] &= 0xDF; break;  // F1
            case 0x3B: new_keys[6] &= 0xBF; break;  // F2
            case 0x3C: new_keys[6] &= 0x7F; break;  // F3

            // Row 7: F4 F5 ESC TAB STOP BS SELECT RETURN
            case 0x3D: new_keys[7] &= 0xFE; break;  // F4
            case 0x3E: new_keys[7] &= 0xFD; break;  // F5
            case 0x29: new_keys[7] &= 0xFB; break;  // ESC
            case 0x2B: new_keys[7] &= 0xF7; break;  // TAB
            case 0x4B: new_keys[7] &= 0xEF; break;  // STOP (Page Up)
            case 0x2A: new_keys[7] &= 0xDF; break;  // BS
            case 0x4D: new_keys[7] &= 0xBF; break;  // SELECT (End)
            case 0x28:                               // Return
            case 0x58: new_keys[7] &= 0x7F; break;  // Numpad Enter

            // Row 8: SPACE HOME INS DEL LEFT UP DOWN RIGHT
            case 0x2C: new_keys[8] &= 0xFE; break;  // SPACE
            case 0x4A: new_keys[8] &= 0xFD; break;  // HOME
            case 0x49: new_keys[8] &= 0xFB; break;  // INS
            case 0x4C: new_keys[8] &= 0xF7; break;  // DEL
            case 0x50: new_keys[8] &= 0xEF; break;  // LEFT
            case 0x52: new_keys[8] &= 0xDF; break;  // UP
            case 0x51: new_keys[8] &= 0xBF; break;  // DOWN
            case 0x4F: new_keys[8] &= 0x7F; break;  // RIGHT

            // Row 9: NUM* NUM+ NUM/ NUM0 NUM1 NUM2 NUM3 NUM4
            case 0x55: new_keys[9] &= 0xFE; break;  // Numpad *
            case 0x57: new_keys[9] &= 0xFD; break;  // Numpad +
            case 0x54: new_keys[9] &= 0xFB; break;  // Numpad /
            case 0x62: new_keys[9] &= 0xF7; break;  // Numpad 0
            case 0x59: new_keys[9] &= 0xEF; break;  // Numpad 1
            case 0x5A: new_keys[9] &= 0xDF; break;  // Numpad 2
            case 0x5B: new_keys[9] &= 0xBF; break;  // Numpad 3
            case 0x5C: new_keys[9] &= 0x7F; break;  // Numpad 4

            // Row 10: NUM5 NUM6 NUM7 NUM8 NUM9 NUM- NUM, NUM.
            case 0x5D: new_keys[10] &= 0xFE; break; // Numpad 5
            case 0x5E: new_keys[10] &= 0xFD; break; // Numpad 6
            case 0x5F: new_keys[10] &= 0xFB; break; // Numpad 7
            case 0x60: new_keys[10] &= 0xF7; break; // Numpad 8
            case 0x61: new_keys[10] &= 0xEF; break; // Numpad 9
            case 0x56: new_keys[10] &= 0xDF; break; // Numpad -
            case 0x63: new_keys[10] &= 0x7F; break; // Numpad .

            default: break;
        }
    }

    // Atomic copy — Core 0 reads keys[] from IRQ context
    __dmb();
    memcpy((void *)keys, new_keys, sizeof(keys));
    __dmb();
}

// -----------------------------------------------------------------------
// TinyUSB HID host callbacks
// -----------------------------------------------------------------------
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;

    // Only handle boot-protocol keyboards (protocol set during enumeration
    // by CFG_TUH_HID_DEFAULT_PROTOCOL in tusb_config.h)
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD)
        tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)dev_addr;
    (void)instance;
    keyboard_reset();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len)
{
    if (len >= 8)
        map_hid_report(report);
    tuh_hid_receive_report(dev_addr, instance);
}

// -----------------------------------------------------------------------
// Core 1 — TinyUSB host task
// -----------------------------------------------------------------------
static void core1_entry(void)
{
    tusb_init();
    tuh_init(0);
    while (true)
        tuh_task();
}

// -----------------------------------------------------------------------
// Main — Core 0
// -----------------------------------------------------------------------
int main(void)
{
    set_sys_clock_khz(250000, true);

    keyboard_reset();
    setup_gpio();
    io_bus_init();

    multicore_launch_core1(core1_entry);

    while (true)
        __wfi();

    return 0;
}
