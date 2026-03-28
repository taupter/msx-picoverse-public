# MSX PicoVerse 2040 USB Keyboard

This document describes the standalone USB keyboard firmware for the PicoVerse 2040 cartridge. The firmware turns the cartridge into a dedicated USB-to-MSX keyboard interface: plug a standard USB keyboard into the cartridge's USB-C port and it appears as the native MSX keyboard to the host system.

> **Compatibility Notice**: The keyboard firmware does **not** work with FPGA-based MSX systems (e.g. OCM/1chipMSX, uMSX, TRHMSX, SM-X, Zemmix Neo, etc.) or MSX computers whose PPI is integrated into a custom chip (e.g. Panasonic models using the T9769 MSX-ENGINE). In both cases, the PPI keyboard ports (`0xA8`–`0xAB`) are decoded and handled internally by the FPGA or MSX-ENGINE, so the I/O read data bus is actively driven by the on-chip PPI logic, causing bus contention with the PicoVerse. See the [FPGA Incompatibility Details](#fpga-incompatibility-details) section below for a full technical explanation.

---

## Overview

The MSX keyboard is a passive 11-row × 8-column matrix scanned by the 8255 PPI (Programmable Peripheral Interface). The BIOS selects a row by writing to I/O port `0xAA` or `0xAB`, then reads the column state from port `0xA9`. The PicoVerse keyboard firmware intercepts these three I/O ports and responds with key states derived from USB HID keyboard reports received over USB-C.

### Intercepted I/O Ports

| Port | Direction | Purpose |
| --- | --- | --- |
| `0xA9` | IN (Z80 reads) | Returns column data for the currently selected keyboard row |
| `0xAA` | OUT (Z80 writes) | Selects the active keyboard row (PPI port C full write) |
| `0xAB` | OUT (Z80 writes) | PPI port C bit set/reset register — the MSX BIOS uses this to change individual row-select bits without rewriting all of port C |

### Key Features

- Fully standalone: does not require any MSX-side software or driver.
- Supports USB HID boot-protocol keyboards, including keyboards connected through USB hubs.
- Full MSX keyboard matrix coverage: rows 0–10, including alphanumeric keys, function keys, cursor keys, numpad, and all MSX-specific keys (GRAPH, CODE, SELECT, STOP).
- USB modifier keys are mapped to MSX equivalents: Shift, Ctrl, Left Alt → GRAPH, Right Alt → CODE, Caps Lock → CAPS.
- Up to 6 simultaneous key presses (USB boot protocol limit) plus modifier keys.
- Runs at 250 MHz for minimal response latency.

---

## Architecture

The firmware uses both cores of the RP2040 and a dedicated pair of PIO state machines on PIO1.

### Dual-Core Design

| Core | Role |
| --- | --- |
| **Core 0** | Services PIO1 IRQ. The IRQ fires when the I/O read or write PIO state machine has data in its RX FIFO. The handler processes port writes (`0xAA`, `0xAB`) to track the selected keyboard row, and port reads (`0xA9`) to supply the column data. Between interrupts, Core 0 sleeps via `__wfi()`. |
| **Core 1** | Runs the TinyUSB host stack. Calls `tuh_task()` in a tight loop to poll for USB events. When a HID keyboard report arrives, the callback converts it to MSX matrix format and updates the shared `keys[]` array. |

### PIO State Machines (PIO1)

| SM | Program | Role |
| --- | --- | --- |
| SM0 | `msx_kb_io_read` | Monitors `/IORQ` + `/RD`. When an I/O read cycle begins, asserts `/WAIT` (side-set), captures the 16-bit address, pushes it to the RX FIFO, then stalls on `pull block` until Core 0 supplies the response byte. Drives D0–D7, releases `/WAIT`, waits for `/RD` to deassert, then tri-states the data bus. |
| SM1 | `msx_kb_io_write` | Monitors `/IORQ` + `/WR`. Captures a full 32-bit GPIO snapshot (`mov isr, pins`) containing address and data, pushes it to the RX FIFO for Core 0 to decode. No `/WAIT` assertion needed for writes. |

### `/WAIT` Signal

The `/WAIT` line (active-low, active on GPIO 28) freezes the Z80 while the RP2040 prepares the keyboard data. The PIO read program uses side-set to control `/WAIT`:

- **`side 0`** = `/WAIT` asserted (Z80 frozen)
- **`side 1`** = `/WAIT` released (Z80 runs)

Without `/WAIT`, the Z80 completes an I/O read in 3–4 T-cycles (~840–1120 ns at 3.58 MHz), which is not enough time for the push/pull FIFO round-trip. Asserting `/WAIT` guarantees the CPU receives correct data regardless of firmware processing time.

During initialisation, the firmware pre-sets the PIO output register for the `/WAIT` pin to HIGH before switching the GPIO mux to PIO control. This prevents a brief glitch where `/WAIT` could be driven LOW (freezing the Z80) between the `pio_gpio_init()` call and the first PIO instruction executing.

---

## MSX Keyboard Matrix

The MSX standard keyboard matrix has 11 rows (0–10) with 8 columns each. Each bit in a row byte represents one key: `1` = released, `0` = pressed (active-low).

### Matrix Layout

| Row | Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| 1 | ; | ] | [ | \ | = | - | 9 | 8 |
| 2 | B | A | — | / | . | , | \` | ' |
| 3 | J | I | H | G | F | E | D | C |
| 4 | R | Q | P | O | N | M | L | K |
| 5 | Z | Y | X | W | V | U | T | S |
| 6 | F3 | F2 | F1 | CODE | CAPS | GRAPH | CTRL | SHIFT |
| 7 | RETURN | SELECT | BS | STOP | TAB | ESC | F5 | F4 |
| 8 | RIGHT | DOWN | UP | LEFT | DEL | INS | HOME | SPACE |
| 9 | NUM4 | NUM3 | NUM2 | NUM1 | NUM0 | NUM/ | NUM+ | NUM* |
| 10 | NUM. | — | NUM- | NUM9 | NUM8 | NUM7 | NUM6 | NUM5 |

### USB HID to MSX Key Mapping

USB HID boot protocol reports are 8 bytes:

| Byte | Content |
| --- | --- |
| 0 | Modifier flags (Ctrl, Shift, Alt, GUI — left and right) |
| 1 | Reserved (always 0) |
| 2–7 | Up to 6 simultaneous HID key codes |

The firmware maps modifiers and keycodes as follows:

**Modifier Mapping (byte 0 bitmask):**

| USB Modifier | MSX Key | Row | Bit |
| --- | --- | --- | --- |
| Left Shift (bit 1) or Right Shift (bit 5) | SHIFT | 6 | 0 |
| Left Ctrl (bit 0) or Right Ctrl (bit 4) | CTRL | 6 | 1 |
| Left Alt (bit 2) | GRAPH | 6 | 2 |
| Right Alt (bit 6) | CODE | 6 | 4 |

**Notable Non-Obvious Key Assignments:**

| USB Key | HID Code | MSX Key |
| --- | --- | --- |
| Page Up | `0x4B` | STOP |
| End | `0x4D` | SELECT |
| Home | `0x4A` | HOME |
| Insert | `0x49` | INS |
| Delete | `0x4C` | DEL |
| Caps Lock | `0x39` | CAPS |
| Numpad Enter | `0x58` | RETURN |

---

## Port 0xAB Bit Set/Reset Handling

The MSX BIOS frequently uses port `0xAB` instead of `0xAA` to change the keyboard row. Port `0xAB` is the 8255 PPI port C bit set/reset register:

- **Bit 7** = 1 indicates set/reset mode is active.
- **Bits 3–1** encode the bit number (0–7) within port C.
- **Bit 0** is the value to write (1 = set, 0 = reset).

The keyboard row is stored in the lower nibble of PPI port C (bits 0–3). The firmware tracks the current row state and applies individual bit modifications when port `0xAB` writes arrive, mimicking the behavior of a real 8255 PPI.

---

## GPIO Initialisation

Before the PIO state machines are configured, all 30 MSX bus GPIOs are explicitly initialised:

| GPIO Range | Signal | Initial Direction |
| --- | --- | --- |
| 0–15 | A0–A15 | Input |
| 16–23 | D0–D7 | Input (PIO takes control later) |
| 24 | /RD | Input |
| 25 | /WR | Input |
| 26 | /IORQ | Input |
| 27 | /SLTSL | Input (unused by keyboard) |
| 28 | /WAIT | Output, driven HIGH |
| 29 | BUSSDIR | Input (unused — hardware uses BSS138 bidirectional level shifters) |

Setting `/WAIT` HIGH via the SIO pad controller before PIO takes ownership ensures the Z80 is never inadvertently frozen during boot.

---

## USB Host Configuration

The firmware uses TinyUSB in USB Host mode with the following configuration:

| Setting | Value | Purpose |
| --- | --- | --- |
| `CFG_TUSB_RHPORT0_MODE` | `OPT_MODE_HOST` | USB host mode |
| `CFG_TUH_HID` | 4 | Up to 4 HID interfaces |
| `CFG_TUH_HUB` | 1 | USB hub support enabled |
| `CFG_TUH_DEVICE_MAX` | 4 | Up to 4 USB devices (with hub) |
| `CFG_TUH_HID_DEFAULT_PROTOCOL` | `HID_PROTOCOL_BOOT` | Force boot protocol during enumeration |

The `CFG_TUH_HID_DEFAULT_PROTOCOL` setting is critical: it tells TinyUSB to request boot protocol during USB enumeration. Boot protocol guarantees the fixed 8-byte report format that the firmware's key mapper expects. Without this setting, keyboards may enumerate in report protocol where the report format is defined by the HID report descriptor and varies between manufacturers, which would cause silent key mapping failures.

On mount, the firmware checks `tuh_hid_interface_protocol()` and only starts receiving reports from `HID_ITF_PROTOCOL_KEYBOARD` interfaces. On unmount, the keyboard matrix is reset to all-keys-released.

---

## Concurrency and Data Sharing

The `keys[16]` array is shared between Core 1 (writer, from USB reports) and Core 0 (reader, from PIO IRQ handler). Updates use a snapshot pattern:

1. Core 1 builds a complete `new_keys[16]` array from the latest HID report.
2. A `__dmb()` (data memory barrier) is issued before and after `memcpy()` to ensure the entire array is visible atomically to Core 0.

The `keyboard_row` variable is written only by Core 0 (IRQ handler) and read only by Core 0, so no cross-core synchronisation is needed for it.

---

## FIFO Protocol

### I/O Read (SM0 → Core 0 → SM0)

1. SM0 detects `/IORQ=0` + `/RD=0`, asserts `/WAIT`, captures A0–A15, pushes 16-bit address to RX FIFO.
2. Core 0 IRQ handler reads the address. If port is `0xA9`, it looks up `keys[keyboard_row & 0x0F]` and pushes a 16-bit response token to the TX FIFO.
3. The response token encodes both the data byte (bits 7:0) and the pin direction mask (bits 15:8). `0xFF00 | data` drives the bus; `0x0000 | 0xFF` tri-states it.
4. SM0 drives D0–D7, releases `/WAIT`, waits for `/RD` deassert, then tri-states.

For ports other than `0xA9`, the firmware responds with a tri-state token (`0x00FF`) so it does not interfere with other I/O devices on the bus.

### I/O Write (SM1 → Core 0)

1. SM1 detects `/IORQ=0` + `/WR=0`, waits 3 cycles for signals to settle, snapshots all GPIOs via `mov isr, pins`, pushes 32-bit word to RX FIFO.
2. Core 0 IRQ handler extracts `port = bits[7:0]` and `data = bits[23:16]`.
3. Port `0xAA` → full row update. Port `0xAB` → bit set/reset on the tracked row.

---

## Build System

The keyboard firmware is built as part of the `loadrom.pio` aggregate Makefile:

```
cd 2040/software/loadrom.pio
make keyboard
```

This invokes CMake + Ninja to compile the firmware from `pico/keyboard/` and produces:

| Output | Location |
| --- | --- |
| `keyboard.uf2` | `pico/keyboard/build/keyboard.uf2` |
| `keyboard.bin` | `pico/keyboard/dist/keyboard.bin` |

The `keyboard.bin` is embedded into the `loadrom.exe` tool via `xxd` during the tool build step (`make tool`). Building everything at once:

```
make all
```

### Source Files

| File | Purpose |
| --- | --- |
| `pico/keyboard/keyboard_main.c` | Main firmware: GPIO init, PIO setup, IRQ handler, HID-to-MSX mapping, TinyUSB callbacks |
| `pico/keyboard/msx_keyboard.pio` | PIO assembly: `msx_kb_io_read` (with `/WAIT` side-set) and `msx_kb_io_write` |
| `pico/keyboard/keyboard.h` | Pin definitions for all MSX bus signals |
| `pico/keyboard/tusb_config.h` | TinyUSB host configuration with boot protocol enforcement |
| `pico/keyboard/CMakeLists.txt` | CMake project linking Pico SDK, TinyUSB host, PIO, and multicore libraries |

### Dependencies

- Raspberry Pi Pico SDK 2.1.0
- TinyUSB (bundled with Pico SDK)
- CMake 3.13+
- Ninja (Windows) or Make (Linux)
- GCC arm-none-eabi toolchain 13.3

---

## Using the Keyboard Firmware

The keyboard firmware is accessed through the `loadrom.exe` tool with the `-k` flag:

### Generating the UF2

```
loadrom.exe -k -o keyboard.uf2
```

The `-k` option is standalone and cannot be combined with `-s` (Sunrise), `-m` (mapper), or a ROM file argument.

### Flashing

1. Hold BOOTSEL on the RP2040 board while connecting it to the PC via USB-C.
2. Copy `keyboard.uf2` to the `RPI-RP2` drive that appears.
3. The board reboots automatically after flashing completes.

### Using

1. Connect a USB keyboard to the PicoVerse cartridge's USB-C port (use a USB-C OTG adapter or a USB-A to USB-C adapter as needed).
2. Insert the cartridge into the MSX slot.
3. Power on the MSX. The USB keyboard is now the MSX keyboard.

USB hubs are supported, so you can use a hub to connect the keyboard if your adapter arrangement requires it.

---

## Limitations

- **FPGA MSX systems and MSX computers with an integrated PPI are not supported.** The FPGA's (or MSX-ENGINE's) internal PPI unconditionally drives the cartridge slot data bus on keyboard port reads, causing bus contention with the PicoVerse. This affects all FPGA clones and Panasonic models using the T9769 MSX-ENGINE (e.g. FS-A1FX, FS-A1ST, FS-A1GT). See the [FPGA Incompatibility Details](#fpga-incompatibility-details) section for the full analysis.
- **Boot protocol only.** The firmware processes USB HID boot protocol reports (8 bytes). Keyboards that do not support boot protocol or that only work in report protocol are not compatible.
- **6-key rollover limit.** USB boot protocol reports a maximum of 6 simultaneous keycodes plus modifiers. This is a USB specification constraint, not a firmware limitation.
- **No LED feedback.** The firmware does not currently send LED state (Caps Lock, Num Lock, Scroll Lock) back to the USB keyboard.
- **Standalone mode only.** The keyboard firmware is a dedicated mode — it cannot be combined with ROM loading or Nextor in the same UF2. Use the regular `loadrom.exe` (without `-k`) for those workflows.
- **PicoVerse 2040 only.** The keyboard firmware is currently available only for the PicoVerse 2040 cartridge. PicoVerse 2350 support is not yet implemented.

---

## FPGA Incompatibility Details

FPGA-based MSX systems like the OCM (One Chip MSX), SM-X, Zemmix Neo, and derivatives implement the entire MSX chipset—including the PPI (8255)—inside the FPGA fabric. The keyboard firmware on the PicoVerse expects to be the sole responder on the cartridge slot for I/O ports `0xA9`, `0xAA`, and `0xAB`, but the FPGA's architecture prevents this.

### Internal PPI Implementation

The FPGA contains a full PPI keyboard subsystem. When the CPU reads port `0xA9`, the FPGA's internal I/O decoder matches address bits `[7:2] = "101010"` (ports `0xA8`–`0xAB`) and routes the read to its internal `PpiDbi` register. For port `0xA9`, this returns `PpiPortB` — the keyboard column data generated by the FPGA's built-in PS/2 keyboard controller (`eseps2`).

The PS/2 controller continuously maintains a virtual keyboard matrix derived from PS/2 scan codes. When the CPU writes to port `0xAA` or `0xAB` to select a row, the FPGA updates the row select internally and the PS/2 controller immediately reflects the correct column data. This is a self-contained loop that never leaves the FPGA.

### Bus Contention

The critical issue is the FPGA's bus direction signal (`BusDir`). This signal is unconditionally set to `1` for all PPI addresses:

```vhdl
BusDir <= '1' when( pSltAdr(7 downto 2) = "101010" ) else ...  -- I/O:A8-ABh
```

When `BusDir = '1'` and the CPU is performing an I/O read (`/IORQ` low, `/RD` low), the FPGA's top-level logic drives its internal PPI data onto the cartridge slot data bus (`pSltDat`):

```vhdl
BusDir_o <= '1' when( pSltIorq_n = '0' and BusDir = '1' ) else ...
pSltDat  <= dbi when( BusDir_o = '1' ) else (others => 'Z');
```

The FPGA's I/O pins are push-pull digital outputs — they drive both HIGH and LOW strongly. Simultaneously, the PicoVerse PIO detects the same I/O read, asserts `/WAIT`, and drives its own USB keyboard data onto the same D0–D7 lines through the BSS138 level shifters. Both the FPGA and the PicoVerse are actively driving the data bus at the same time, with potentially different values on each bit. This bus contention produces undefined or corrupted keyboard data.

### No Exclusion Mechanism

Unlike a real MSX where the physical 8255 chip outputs `0xFF` for all keys when no keyboard cable is connected (making it safe for the PicoVerse to override), the FPGA's PPI always returns active PS/2 keyboard data. There is no switch, register, or slot-control mechanism in the OCM/SM-X design to exclude ports `0xA8`–`0xAB` from the internal decode and let an external cartridge respond instead.

### `/WAIT` Timing

The FPGA does sample the external `/WAIT` signal, but through a registered path that adds at least one internal clock cycle of latency compared to a real Z80's direct `/WAIT` sampling:

```vhdl
pSltWait_n <= 'Z';  -- FPGA does not drive /WAIT (tri-state, external pull-up)

-- Sampled on CPU clock rising edge, registered into wait_n_s
if( ... or pSltWait_n = '0' or ... )then
    wait_n_s <= '0';
```

This means the FPGA's CPU may begin driving return data before recognising the PicoVerse's `/WAIT` assertion, further reducing any window where the PicoVerse could respond cleanly.

### Summary

| Aspect | Real MSX Hardware | FPGA MSX (OCM) |
| --- | --- | --- |
| PPI port `0xA9` decode | Physical 8255 chip on the motherboard | Internal VHDL PPI inside the FPGA |
| Data bus on I/O read | 8255 drives (can be overridden) | FPGA push-pull I/O pins (strong drive, cannot be overridden) |
| `/WAIT` handling | Z80 samples `/WAIT` directly | Extra register stage adds latency |
| Keyboard source | Physical matrix (returns `0xFF` if disconnected) | PS/2 controller always active |
| External cartridge override | Works — PicoVerse wins the bus | Fails — bus contention with FPGA |

---

## Technical Reference

### Clock Speed

The RP2040 runs at 250 MHz (overclocked from the default 125 MHz). PIO state machines run at the same clock with a divider of 1.0, giving each PIO cycle a period of 4 ns — well within the Z80's I/O timing requirements.

### IRQ Priority

The PIO1 IRQ handler runs at the highest ARM priority level (0). This ensures keyboard I/O requests are serviced immediately, minimising the time `/WAIT` is held.

### Memory Placement

The IRQ handler (`keyboard_pio1_irq_handler`) and the HID report mapper (`map_hid_report`) are placed in SRAM via `__not_in_flash_func()` to avoid XIP flash latency jitter during time-critical bus operations.

---

Author: Cristiano Goncalves  
Last updated: 03/01/2026
