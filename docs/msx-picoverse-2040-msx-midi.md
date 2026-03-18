# MSX PicoVerse 2040 MSX-MIDI

This document describes the standalone MSX-MIDI firmware for the PicoVerse 2040 cartridge. The firmware turns the cartridge into a dedicated MSX-MIDI interface: plug a USB-MIDI cable (e.g., connected to a Roland SoundCanvas) into the cartridge's USB-C port and it appears as the standard MSX-MIDI interface to MSX software. MIDI players and sequencers that support the MSX-MIDI standard can send and receive MIDI data through the cartridge without any drivers or TSRs.

---

## Overview

The MSX-MIDI interface is based on the Intel 8251 USART and Intel 8253 Programmable Interval Timer, as found in the Panasonic FS-A1GT turbo R and the ESEMSX3 FPGA implementation. The PicoVerse firmware emulates these chips on I/O ports `0xE8`–`0xEF` and bridges the serial MIDI data stream to the USB MIDI cable acting as a USB host.

### How It Works

1. MSX software writes MIDI bytes to port `0xE8` (8251 data register).
2. The PicoVerse PIO hardware intercepts the I/O write cycle and places the byte into a ring buffer.
3. Core 1 of the RP2040 drains the ring buffer, parses the raw MIDI byte stream into USB-MIDI Event Packets, and sends them to the USB MIDI cable device over a bulk OUT endpoint.
4. In the reverse direction, MIDI data received from the USB device is decoded from USB-MIDI Event Packets, placed in an RX ring buffer, and made available when the MSX reads port `0xE8`.
5. MSX software polls port `0xE9` for status (TxRDY, RxRDY, TxEmpty) to know when to send or receive.

### Key Features

- Fully standalone: does not require any MSX-side driver or TSR — standard MSX-MIDI software works out of the box.
- Bidirectional: supports both MIDI TX (MSX → USB device) and MIDI RX (USB device → MSX).
- Full USB-MIDI Event Packet encoding/decoding: channel messages, System Exclusive (SysEx), System Common, and Real-Time messages.
- Running status support for efficient MIDI transmission.
- SysEx streaming with correct 3-byte grouping and end-of-SysEx packet handling.
- USB hub support: the MIDI cable can be connected through a USB hub.
- Runs at 250 MHz for minimal response latency.

---

## MSX-MIDI I/O Port Map

The firmware intercepts all eight I/O ports of the MSX-MIDI interface:

| Port | Read | Write | Chip | Description |
| --- | --- | --- | --- | --- |
| `0xE8` | RX data byte | TX data byte | 8251 | MIDI data register |
| `0xE9` | Status register | Command register | 8251 | USART status / command |
| `0xEA` | `0x00` | Ignored | — | Timer interrupt acknowledge |
| `0xEB` | `0x00` | Ignored | — | Timer interrupt acknowledge (mirror) |
| `0xEC` | `0x00` | Ignored | 8253 | Counter 0 |
| `0xED` | `0x00` | Ignored | 8253 | Counter 1 |
| `0xEE` | `0x00` | Ignored | 8253 | Counter 2 |
| `0xEF` | `0x00` | Ignored | 8253 | Timer control word |

The 8253 timer ports (`0xEA`–`0xEF`) are stubbed: writes are silently accepted and reads return `0x00`. The USB MIDI protocol handles baud rate and timing internally, so there is no need to emulate a baud rate generator or timer interrupts.

### 8251 Status Register (Port `0xE9` Read)

| Bit | Name | Description |
| --- | --- | --- |
| 0 | TxRDY | Transmitter ready — `1` when the TX ring buffer is not full and can accept a new byte |
| 1 | RRDY | Receiver ready — `1` when the RX ring buffer has MIDI data from the USB device |
| 2 | TxEM | Transmitter empty — `1` when the TX ring buffer has been completely drained |
| 3 | PE | Parity error — always `0` (USB MIDI does not use parity) |
| 4 | OE | Overrun error — always `0` |
| 5 | FE | Framing error — always `0` |
| 6 | BRK | Break detect — always `0` |
| 7 | DSR | 8253 timer interrupt flag — always `0` (timer not emulated) |

### 8251 Command Register (Port `0xE9` Write)

| Bit | Name | Effect |
| --- | --- | --- |
| 0 | TEN | Transmit enable — when set, bytes written to port `0xE8` are queued for USB MIDI transmission |
| 2 | RE | Receive enable — when set, received USB MIDI bytes are available via port `0xE8` reads |
| 6 | IR | Internal reset — reinitializes the 8251 emulation state (TX and RX disabled) |

Bits 1, 3, 4, 5, and 7 are accepted but ignored (DTR, RTS, error reset, hunt mode, EH are not relevant for MIDI).

### Typical MSX-MIDI Initialization Sequence

MSX-MIDI software follows the standard 8251 initialization pattern:

```
; Reset the 8251 — send 3 dummy bytes followed by the reset command
OUT ($E9), 0x00     ; Dummy
OUT ($E9), 0x00     ; Dummy
OUT ($E9), 0x00     ; Dummy
OUT ($E9), 0x40     ; Internal Reset (bit 6)

; Mode instruction (first write after reset)
OUT ($E9), 0x4E     ; Async, 8-bit, no parity, 1 stop, x16 clock

; Command instruction
OUT ($E9), 0x05     ; TEN=1, RE=1 (enable transmit and receive)

; Now ready to send/receive MIDI data:
; Poll port $E9 for TxRDY before writing to $E8
; Poll port $E9 for RRDY before reading from $E8
```

The firmware accepts and discards the mode instruction (first write after reset) since the USB protocol parameters are fixed. Only the command instruction's TEN and RE bits have effect.

---

## Architecture

The firmware uses both cores of the RP2040 and a dedicated pair of PIO state machines on PIO1, following the same pattern as the USB keyboard firmware.

### Dual-Core Design

| Core | Role |
| --- | --- |
| **Core 0** | Services PIO1 IRQ. The IRQ fires when an I/O read or write state machine has data in its RX FIFO. The handler processes port writes (`0xE8` for TX data, `0xE9` for commands) and port reads (`0xE8` for RX data, `0xE9` for status). Between interrupts, Core 0 sleeps via `__wfi()`. |
| **Core 1** | Runs the TinyUSB host stack. Calls `tuh_task()` in a tight loop. Drains the TX ring buffer by feeding bytes into the MIDI stream parser, which encodes them as USB-MIDI Event Packets. Calls `usb_midi_host_flush()` to send queued packets over a USB bulk OUT transfer. Incoming packets from the USB device are decoded and placed in the RX ring buffer. |

### PIO State Machines (PIO1)

| SM | Program | Role |
| --- | --- | --- |
| SM0 | `msx_midi_io_read` | Monitors `/IORQ` + `/RD`. When an I/O read cycle begins, asserts `/WAIT` (side-set), captures the 16-bit address, pushes it to the RX FIFO, then stalls on `pull block` until Core 0 supplies the response byte. Drives D0–D7, releases `/WAIT`, waits for `/RD` to deassert, then tri-states the data bus. |
| SM1 | `msx_midi_io_write` | Monitors `/IORQ` + `/WR`. Captures a full 32-bit GPIO snapshot (`mov isr, pins`) containing address and data after a 3-cycle settling delay, pushes it to the RX FIFO for Core 0 to decode. No `/WAIT` assertion needed for writes. |

### `/WAIT` Signal

The `/WAIT` line (GPIO 28, active-low) freezes the Z80 while the RP2040 prepares the response data. The PIO read program uses side-set to control `/WAIT`:

- **`side 0`** = `/WAIT` asserted (Z80 frozen)
- **`side 1`** = `/WAIT` released (Z80 runs)

Without `/WAIT`, the Z80 completes an I/O read in 3–4 T-cycles (~840–1120 ns at 3.58 MHz), which is not enough time for the push/pull FIFO round-trip. Asserting `/WAIT` guarantees the CPU receives correct data regardless of firmware processing time.

During initialization, the firmware pre-sets the PIO output register for the `/WAIT` pin to HIGH before switching the GPIO mux to PIO control. This prevents a brief glitch where `/WAIT` could be driven LOW (freezing the Z80) between the `pio_gpio_init()` call and the first PIO instruction executing.

### Data Flow Diagram

```
MSX Z80 CPU
    │
    ├── I/O write to $E8 ──► PIO1 SM1 (write captor) ──► Core 0 IRQ handler
    │                                                         │
    │                                                    TX ring buffer
    │                                                         │
    │                                              Core 1 TinyUSB host task
    │                                                         │
    │                                              MIDI stream parser
    │                                              (raw bytes → USB-MIDI packets)
    │                                                         │
    │                                              USB bulk OUT transfer
    │                                                         │
    │                                                    USB MIDI cable
    │                                                         │
    │                                              External MIDI device
    │                                              (e.g., Roland SoundCanvas)
    │
    ├── I/O read from $E8 ◄── PIO1 SM0 (read responder) ◄── Core 0 IRQ handler
    │                                                              │
    │                                                         RX ring buffer
    │                                                              │
    │                                                    Core 1 USB bulk IN
    │                                                              │
    │                                                    USB-MIDI packet decoder
    │                                                    (USB-MIDI packets → raw bytes)
    │                                                              │
    │                                                         USB MIDI cable
    │
    └── I/O read from $E9 ◄── PIO1 SM0 ◄── Core 0 returns status byte
                                            (TxRDY | RRDY | TxEM)
```

---

## Ring Buffers

Two single-producer single-consumer (SPSC) ring buffers provide lock-free, interrupt-safe inter-core communication:

| Buffer | Size | Producer | Consumer | Purpose |
| --- | --- | --- | --- | --- |
| TX ring buffer | 256 bytes | Core 0 (PIO IRQ handler) | Core 1 (USB task loop) | MSX → USB MIDI device |
| RX ring buffer | 64 bytes | Core 1 (USB RX callback) | Core 0 (PIO IRQ handler) | USB MIDI device → MSX |

Both buffers use power-of-2 sizes with bitmask indexing for wrap-around. Data memory barriers (`__dmb()`) ensure proper ordering across cores on the RP2040's dual Cortex-M0+ architecture.

The TX buffer is larger (256 bytes) because MSX software may burst-write MIDI data faster than USB bulk transfers can drain it — especially during SysEx dumps or rapid note sequences. The RX buffer is smaller (64 bytes) because incoming MIDI data from the device is typically lower throughput (responses, clock, active sensing).

---

## USB MIDI Host Driver

TinyUSB (bundled with Pico SDK 2.2.0) does not include a built-in USB MIDI host class driver. The firmware implements a custom application-level host driver that registers via TinyUSB's `usbh_app_driver_get_cb()` weak callback mechanism.

### Driver Registration

```c
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &midi_host_driver;
}
```

TinyUSB calls this weak callback during `tuh_init()`. The returned `usbh_class_driver_t` structure provides six callbacks:

| Callback | Purpose |
| --- | --- |
| `init` | Reset device state, parser, and ring buffers |
| `deinit` | Cleanup (no-op) |
| `open` | Parse USB descriptors, find MIDI Streaming interface, open bulk endpoints |
| `set_config` | Start receiving (submit first bulk IN transfer) |
| `xfer_cb` | Handle transfer completions (TX done → clear busy flag; RX done → decode packets, resubmit) |
| `close` | Device disconnected — reset all state |

### Device Enumeration

When a USB device is connected, TinyUSB presents each interface to registered drivers. The MIDI driver's `open()` callback:

1. Checks that `bInterfaceClass == TUSB_CLASS_AUDIO` (0x01).
2. Iterates through the interface descriptors looking for a descriptor with `bInterfaceSubClass == 0x03` (MIDI Streaming).
3. Once the MIDI Streaming interface is found, locates the bulk OUT and bulk IN endpoints.
4. Opens both endpoints via `tuh_edpt_open()`.

The driver accepts any USB MIDI device regardless of vendor or product ID. This includes:
- USB-MIDI cables (e.g., CH345-based cables with VID `0x1A86`, PID `0x752D`)
- MIDI keyboards and controllers with USB connectivity
- Hardware synthesizers with USB-MIDI ports

### `CFG_TUH_MIDI` Configuration Note

The `tusb_config.h` sets `CFG_TUH_MIDI` to `0`. Setting it to `1` would enable an interface-association grouping feature in TinyUSB's `usbh.c` that references `AUDIO_SUBCLASS_CONTROL` and `AUDIO_FUNC_PROTOCOL_CODE_UNDEF` — symbols defined in `class/audio/audio.h` which is not included in the host build path. Since our custom driver handles enumeration independently, this grouping feature is not needed.

---

## USB-MIDI Event Packet Encoding

The USB MIDI specification (part of the USB Audio class) defines a 4-byte event packet format for transferring MIDI data over USB:

| Byte | Field | Description |
| --- | --- | --- |
| 0 | Cable Number + CIN | Upper nibble = cable number (always 0), lower nibble = Code Index Number |
| 1 | MIDI byte 1 | Status byte or first data byte |
| 2 | MIDI byte 2 | Second data byte (or 0x00 if not used) |
| 3 | MIDI byte 3 | Third data byte (or 0x00 if not used) |

### Code Index Number (CIN) Table

| CIN | MIDI Event | Packet Size |
| --- | --- | --- |
| `0x02` | 2-byte System Common (MTC Quarter Frame, Song Select) | 2 bytes |
| `0x03` | 3-byte System Common (Song Position Pointer) | 3 bytes |
| `0x04` | SysEx start or continue (3 data bytes) | 3 bytes |
| `0x05` | SysEx end with 1 byte, or single-byte System Common | 1 byte |
| `0x06` | SysEx end with 2 bytes | 2 bytes |
| `0x07` | SysEx end with 3 bytes | 3 bytes |
| `0x08` | Note Off | 3 bytes |
| `0x09` | Note On | 3 bytes |
| `0x0A` | Poly Aftertouch | 3 bytes |
| `0x0B` | Control Change | 3 bytes |
| `0x0C` | Program Change | 2 bytes |
| `0x0D` | Channel Pressure | 2 bytes |
| `0x0E` | Pitch Bend | 3 bytes |
| `0x0F` | Single byte (Real-Time messages) | 1 byte |

### MIDI Stream Parser

The firmware includes a complete MIDI stream parser that converts the raw byte stream from the MSX into properly encoded USB-MIDI Event Packets. The parser handles:

- **Channel Voice/Mode Messages** (0x80–0xEF): Collects the expected number of data bytes (1 for Program Change/Channel Pressure, 2 for all others), then emits a packet with the appropriate CIN.
- **Running Status**: Maintains the last channel status byte. When data bytes arrive without a preceding status byte, the parser uses the stored running status. System Common messages clear running status per the MIDI specification.
- **System Exclusive (SysEx)**: Accumulates data bytes in groups of 3 and emits `CIN 0x04` packets. When `0xF7` (End of SysEx) is received, emits `CIN 0x05`, `0x06`, or `0x07` depending on how many bytes remain in the current group.
- **Real-Time Messages** (0xF8–0xFF): Immediately emitted as `CIN 0x0F` single-byte packets. These can interrupt any ongoing message (including SysEx) without disrupting the parser state.
- **System Common Messages** (0xF1–0xF6): Handled individually based on their expected data byte count.

### USB Transfer Management

- **TX**: Outgoing USB-MIDI Event Packets are accumulated in a 64-byte DMA-aligned buffer. Multiple 4-byte packets can be batched into a single USB bulk OUT transfer for efficiency. The `flush()` function initiates the transfer; the completion callback clears the busy flag and resets the buffer offset.
- **RX**: A bulk IN transfer is submitted when the device is configured. On completion, received packets are decoded and individual MIDI bytes are placed in the RX ring buffer. The transfer is immediately resubmitted for continuous reception.

---

## GPIO Pin Mapping

All 30 GPIOs of the RP2040 are mapped to MSX bus signals:

| GPIO | Signal | Direction | Description |
| --- | --- | --- | --- |
| 0–15 | A0–A15 | Input | Address bus |
| 16–23 | D0–D7 | Bidirectional (PIO) | Data bus — PIO drives during I/O reads, input otherwise |
| 24 | /RD | Input | Active-low read strobe |
| 25 | /WR | Input | Active-low write strobe |
| 26 | /IORQ | Input | Active-low I/O request |
| 27 | /SLTSL | Input | Slot select (unused — MIDI is I/O only) |
| 28 | /WAIT | Output (PIO side-set) | Active-low, freezes Z80 during I/O reads |
| 29 | BUSSDIR | Input | Bus direction (unused — hardware uses BSS138 level shifters) |

---

## USB Host Configuration

The firmware uses TinyUSB in USB Host mode with the following configuration:

| Setting | Value | Purpose |
| --- | --- | --- |
| `CFG_TUSB_RHPORT0_MODE` | `OPT_MODE_HOST` | USB host mode — Pico acts as host to USB MIDI cable |
| `CFG_TUH_HUB` | `1` | USB hub support enabled |
| `CFG_TUH_MIDI` | `0` | No built-in MIDI host (custom app driver used instead) |
| `CFG_TUH_DEVICE_MAX` | `4` | Up to 4 USB devices (with hub) |
| `CFG_TUH_HID` | `0` | HID disabled (not needed for MIDI) |
| `CFG_TUH_CDC` | `0` | CDC disabled |
| `CFG_TUH_MSC` | `0` | Mass Storage disabled |
| `CFG_TUH_VENDOR` | `0` | Vendor class disabled |
| `CFG_TUH_ENUMERATION_BUFSIZE` | `256` | Descriptor buffer size for enumeration |

---

## Building

The MIDI firmware is built as part of the `loadrom.pio` build system.

### Build the MIDI firmware only

```
cd 2040/software/loadrom.pio
make midi
```

This runs CMake to configure and build the firmware. The resulting binary is copied to `pico/midi/dist/midi.bin`.

### Build everything (firmware + keyboard + MIDI + tool)

```
cd 2040/software/loadrom.pio
make all
```

### Generate a MIDI UF2 file

```
loadrom.exe -i [-o output.uf2]
```

The `-i` (or `--midi`) option embeds the pre-built MIDI firmware binary into a UF2 file suitable for flashing to the RP2040 via USB mass storage boot mode. The option is mutually exclusive with `-s` (Sunrise), `-m` (mapper), `-k` (keyboard), and ROM file arguments — the USB port is dedicated to the MIDI cable and cannot be shared with other functions.

---

## Compatibility

### MSX Software

The firmware is compatible with any MSX software that uses the standard MSX-MIDI I/O ports (`0xE8`–`0xE9`). This includes:

- **MIDRY** — Standard MIDI File player for MSX-DOS. Tested successfully with the PicoVerse MIDI firmware. Use the `/I5` option to select the MSX-MIDI interface: `MIDRY /I5 filename.MID`. Without `/I5`, MIDRY defaults to a different MIDI output method that does not use the 8251 USART ports.
- **SZMMP** - MIDI player for MSX-DOS. Tested successfully with the PicoVerse MIDI firmware. Use the `/U0` option to force select the MSX-MIDI interface: `SZMMP filename.MID /U0`. Without the /U0 option, SZMMP auto-detects the MSX-MIDI interface as well.

Software must follow the standard polling protocol: check TxRDY before writing and RRDY before reading. The firmware does not generate interrupts — only polled I/O is supported.

### MSX Systems

The firmware works with real MSX hardware (MSX1, MSX2, MSX2+, turbo R) where the I/O bus is directly accessible from the cartridge slot. 

> **FPGA Compatibility**: The MIDI firmware intercepts I/O ports `0xE8`–`0xEF`. If an FPGA-based MSX system already implements MSX-MIDI internally, there will be bus contention. Test with your specific FPGA implementation.

### USB MIDI Devices

Any USB device that implements the USB Audio class with MIDI Streaming subclass (class `0x01`, subclass `0x03`) and uses bulk endpoints is supported. This includes:

- Generic USB-MIDI cables (CH345, TI-based, etc.)
- Roland UM-ONE / UM-ONE mk2
- Yamaha UX16
- Direct USB connections to synthesizers, sound modules, and MIDI controllers

---

## Source Files

All MIDI firmware source files are located in `2040/software/loadrom.pio/pico/midi/`:

| File | Description |
| --- | --- |
| `midi.h` | Pin definitions, MSX-MIDI port addresses, 8251 status register bit masks, ring buffer sizes |
| `tusb_config.h` | TinyUSB configuration for USB host mode |
| `msx_midi.pio` | PIO assembly programs for I/O read (with `/WAIT` side-set) and I/O write capture |
| `usb_midi_host.h` | Public API for the USB MIDI host driver |
| `usb_midi_host.c` | Complete USB MIDI host driver: TinyUSB app-level driver registration, descriptor parsing, bulk endpoint management, MIDI stream parser/encoder, USB-MIDI event packet decoder, RX ring buffer |
| `midi_main.c` | Main firmware: TX ring buffer, 8251 USART emulation logic, PIO1 IRQ handler, GPIO/PIO initialization, Core 1 USB task loop |
| `CMakeLists.txt` | CMake build configuration |
| `pico_sdk_import.cmake` | Pico SDK CMake integration |

---

## References

- [MSX-MIDI specification](https://www.msx.org/wiki/MSX-MIDI) — I/O port map and 8251/8253 register details
- [Intel 8251 USART datasheet](https://www.righto.com/2023/01/the-8251-uart-chip-designs-for.html) — Command/mode/status register definitions
- [USB MIDI specification (USB.org)](https://www.usb.org/sites/default/files/midi10.pdf) — USB-MIDI Event Packet format, CIN codes
- [ESEMSX3 midi.vhd](https://github.com/msx-association/ocm-pld-dev) — FPGA reference implementation (TX-only)
- [TinyUSB documentation](https://docs.tinyusb.org/) — Host stack, `usbh_app_driver_get_cb()` mechanism

---

Author: Cristiano Goncalves
Last updated: 03/07/2026
