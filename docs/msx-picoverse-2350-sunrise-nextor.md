# MSX PicoVerse 2350 — Sunrise IDE Emulation for Nextor

This document describes the implementation of the Sunrise IDE interface emulation modes in the MSX PicoVerse 2350 firmware. The 2350 variant supports two distinct storage backends — microSD card and USB mass storage — each available standalone, combined with a 1MB PSRAM-backed memory mapper, or combined with that mapper plus Carnivore2-compatible RAM-mode loading for `SROM.COM /D15`. The four non-Carnivore2 Sunrise modes can also optionally expose ESP-01 WiFi support when built with `loadrom.exe -w`.

## 1. Overview

The MSX PicoVerse 2350 provides a complete Sunrise IDE emulation that allows the MSX to boot Nextor DOS and access mass storage through either the on-board microSD card slot or the USB-C port. Six firmware modes are available:

| Option | Mapper Type | Storage Backend | Description |
|--------|-------------|-----------------|-------------|
| `-s1` / `--sunrise-sd` | 15 | microSD (SPI) | Sunrise IDE with microSD card |
| `-m1` / `--mapper-sd` | 16 | microSD (SPI) | Sunrise IDE + 1MB PSRAM mapper with microSD card |
| `-c1` / `--carnivore2-sd` | 17 | microSD (SPI) | Sunrise IDE + 1MB PSRAM mapper + Carnivore2 RAM-mode target for `SROM.COM /D15` |
| `-s2` / `--sunrise-usb` | 10 | USB-C (MSC) | Sunrise IDE with USB pendrive |
| `-m2` / `--mapper-usb` | 11 | USB-C (MSC) | Sunrise IDE + 1MB PSRAM mapper with USB pendrive |
| `-c2` / `--carnivore2-usb` | 18 | USB-C (MSC) | Sunrise IDE + 1MB PSRAM mapper + Carnivore2 RAM-mode target for `SROM.COM /D15` |

All six modes:

- Emulate the Sunrise IDE FlashROM banking for the 128KB Nextor ROM image (8 × 16KB pages)
- Emulate the full ATA task-file register set at `0x7C00`–`0x7EFF`
- Translate ATA sector read/write commands into block I/O operations on the chosen storage device
- Run the Nextor 2.1.4 Sunrise IDE driver (Master Only edition)
- Use the RP2350's dual-core architecture: Core 0 for PIO bus and IDE register handling, Core 1 for storage backend

The mapper variants (`-m1`, `-m2`) additionally provide 1MB of mapper RAM (64 × 16KB pages) in an expanded sub-slot architecture, with mapper page registers intercepted via PIO1.

The Carnivore2 RAM-mode variants (`-c1`, `-c2`) build on that same mapper architecture and add a Carnivore2-compatible control/register surface plus banked RAM windows so `SROM.COM /D15` can upload ROM images into PSRAM and launch them directly from RAM.

The `-w` WiFi flag is available for `-s1`, `-m1`, `-s2`, and `-m2`. It does not create new mapper IDs; instead, it sets an additional configuration flag that makes the firmware expose a WiFi subslot containing the ESP8266P system ROM and memio UART surface. See the dedicated WiFi guide for the full register-level details.

## 2. Architecture

All six modes share an identical ATA front-end on Core 0. Core 1 runs the appropriate storage backend depending on the mapper type.

### Shared ATA Front-End

The Sunrise IDE ATA emulation is implemented in `sunrise_ide.c` and provides:

- **Segment/IDE control register** at `0x4104` (write-only) — FlashROM page select with bit-reversed bank number + IDE enable bit
- **16-bit data register** at `0x7C00`–`0x7DFF` — with low/high byte latch for MSX 8-bit bus
- **ATA task-file registers** at `0x7E00`–`0x7EFF` — mirrored every 16 bytes
- **ATA commands**: IDENTIFY DEVICE (`0xEC`), READ SECTORS (`0x20`), WRITE SECTORS (`0x30`), EXECUTE DEVICE DIAGNOSTIC (`0x90`), DEVICE RESET (`0x08`), SET FEATURES (`0xEF`), INIT PARAMS (`0x91`), RECALIBRATE (`0x10`)

The front-end sets volatile request flags (`usb_read_requested`, `usb_write_requested`) that are serviced by whichever backend runs on Core 1.

### USB Backend (Types 10, 11)

`sunrise_ide.c` — `sunrise_usb_task()`:

```
┌─────────────────────────────────────────────────────┐
│  Core 1 — USB Backend                               │
│  ┌───────────────────────────────────────────────┐  │
│  │  TinyUSB host stack (Full Speed)              │  │
│  │  tuh_task() polling loop                      │  │
│  │  tuh_msc_read10() / tuh_msc_write10()         │  │
│  │  SCSI INQUIRY → vendor/product/revision       │  │
│  │  Async callbacks with timeout watchdog        │  │
│  └───────────────────────────────────────────────┘  │
└────────────────────────┬────────────────────────────┘
                         │ USB Host (Full Speed)
                  ┌──────▼──────┐
                  │   USB-C     │
                  │ Mass Storage│
                  │   Device    │
                  └─────────────┘
```

- Transfers are asynchronous via TinyUSB callbacks
- A 3-second timeout watchdog prevents stalled transfers from keeping the IDE in BSY
- Supports devices with non-512-byte native sectors by reading the native block and extracting the correct 512-byte slice
- Device info for IDENTIFY comes from the USB SCSI INQUIRY response

### microSD Backend (Types 15, 16)

`sunrise_sd.c` — `sunrise_sd_task()`:

```
┌─────────────────────────────────────────────────────┐
│  Core 1 — SD Backend                                │
│  ┌───────────────────────────────────────────────┐  │
│  │  disk_initialize() → SPI init + card detect   │  │
│  │  disk_ioctl(GET_SECTOR_COUNT)                 │  │
│  │  CID register → vendor/product/revision       │  │
│  │  disk_read() / disk_write() polling loop      │  │
│  └───────────────────────────────────────────────┘  │
└────────────────────────┬────────────────────────────┘
                         │ SPI0 @ 31.25 MHz
                  ┌──────▼──────┐
                  │   microSD   │
                  │   Card      │
                  └─────────────┘
```

- Transfers are synchronous — `disk_read()` and `disk_write()` block until complete, which simplifies the code vs. the async USB path
- All SD cards use 512-byte sectors natively; no sector-size conversion is needed
- Device info for IDENTIFY is extracted from the SD card's CID register (OEM ID, Product Name, Product Revision)
- Card initialization happens once at startup; no hot-plug support

### Combined Diagram

```
┌──────────────────────────────────────────────────────────┐
│  MSX Bus                                                 │
│  /SLTSL, /RD, /WR, A0-A15, D0-D7, /WAIT                │
└──────────────┬───────────────────────┬───────────────────┘
               │                       │
        ┌──────▼───────┐        ┌──────▼────────┐
        │  SM0 (PIO0)  │        │  SM1 (PIO0)   │
        │  Read Resp.  │        │  Write Capt.  │
        └──────┬───────┘        └──────┬────────┘
               │                       │
        ┌──────▼───────────────────────▼────────┐
        │            Core 0 (CPU)               │
        │  sunrise_ide_handle_read/write()      │
        │  Sunrise mapper page lookup           │ 
        │  PIO bus + FIFO management            │
        └──────────────────┬────────────────────┘
                           │  Shared volatile flags
                           │  (sunrise_ide_t)
             ┌─────────────┴─────────────┐
             │                           │
     ┌───────▼───────┐          ┌────────▼────────┐
     │   Core 1      │          │    Core 1       │
     │ sunrise_usb_  │    OR    │ sunrise_sd_     │
     │ task()        │          │ task()          │
     │ (Types 10,11) │          │ (Types 15,16)   │
     └───────┬───────┘          └────────┬────────┘
             │                           │
      ┌──────▼──────┐           ┌────────▼────────┐
      │   USB-C     │           │    microSD      │
      │ Pendrive    │           │    Card (SPI)   │
      └─────────────┘           └─────────────────┘
```

## 3. microSD Card Hardware

The microSD card connects to the RP2350 via SPI0:

| GPIO | Signal | SPI Function |
|------|--------|-------------|
| 33 | CS | Chip Select (manual) |
| 34 | SCK | SPI Clock |
| 35 | MOSI | SPI Data Out |
| 36 | MISO | SPI Data In |

- **SPI peripheral**: `spi0`
- **Baud rate**: 31.25 MHz (`125 MHz / 4`)
- **Library**: Carl Kugler's `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` — provides the FatFS `diskio.h` low-level API (`disk_initialize`, `disk_read`, `disk_write`, `disk_ioctl`)

The hardware configuration is defined in `hw_config.c`, which registers a single `sd_card_t` with the SPI interface parameters. This is the same configuration used by the Explorer firmware.

## 4. SD Card Device Identification

When the SD card is initialized, the firmware reads the card's CID (Card IDentification) register to populate the ATA IDENTIFY DEVICE response with real device information. The CID is a 16-byte register embedded in every SD card containing manufacturer-programmed identification data.

### CID Fields Used

| Field | CID Bits | Size | Purpose |
|-------|----------|------|---------|
| OEM/Application ID | [119:104] | 2 chars | ATA model prefix (vendor) |
| Product Name | [103:64] | 5 chars | ATA model name |
| Product Revision | [63:56] | 4+4 bits | ATA firmware revision ("major.minor") |

### Extraction Code

```c
sd_card_t *sd = sd_get_by_num(0);
char vendor[3] = {0};
char product[6] = {0};
char revision[5] = {0};

ext_str(16, sd->state.CID, 119, 104, sizeof(vendor), vendor);
ext_str(16, sd->state.CID, 103, 64, sizeof(product), product);
uint8_t prv_major = (uint8_t)ext_bits16(sd->state.CID, 63, 60);
uint8_t prv_minor = (uint8_t)ext_bits16(sd->state.CID, 59, 56);
snprintf(revision, sizeof(revision), "%u.%u", prv_major, prv_minor);
```

This data is passed to `sunrise_ide_set_device_info()`, which populates the shared ATA state used by `build_identify_data()`. During Nextor boot, the driver prints the model name from IDENTIFY words 27–46 and the firmware revision from words 23–26, showing the actual microSD card identity.

For USB devices, the equivalent information comes from the SCSI INQUIRY response (vendor ID, product ID, and product revision).

## 5. Inter-Core Communication

Both backends reuse the same set of volatile flags originally defined for the USB path. The flag names retain their `usb_` prefix but serve as generic IDE↔backend signals:

| Flag | Direction | Purpose |
|------|-----------|---------|
| `usb_device_mounted` | Backend → Front-end | Storage device is ready for I/O |
| `usb_read_requested` | Front-end → Backend | ATA READ SECTORS pending |
| `usb_read_lba` | Front-end → Backend | LBA for the pending read |
| `usb_write_requested` | Front-end → Backend | ATA WRITE SECTORS pending (buffer full) |
| `usb_write_lba` | Front-end → Backend | LBA for the pending write |
| `usb_write_buffer[512]` | Front-end → Backend | Sector data to write |
| `usb_read_ready` | Backend → Front-end | Read completed successfully |
| `usb_read_failed` | Backend → Front-end | Read failed |
| `usb_write_ready` | Backend → Front-end | Write completed successfully |
| `usb_write_failed` | Backend → Front-end | Write failed |
| `usb_identify_pending` | Front-end → Backend | IDENTIFY waiting for device mount |

These flags are declared non-static in `sunrise_ide.c` and accessed via `extern` in `sunrise_sd.c`. Flag ownership is strictly unidirectional — one core sets, the other clears.

## 6. Mapper Mode (Types 11, 16)

The mapper variants (`-m1`, `-m2`) add a two-phase bootstrap and an expanded sub-slot architecture:

### Phase 1 — Bootstrap ROM

A minimal 18-byte ROM is served that:
1. Presents itself with the `AB` header so the BIOS executes the INIT routine
2. Reads `I/O port 0xF4`, ORs bit 7 (boot slot), and writes it back
3. Executes `RST 0x00` to restart the MSX

This forces the MSX to reboot with the cartridge slot marked as the boot slot, required for the expanded sub-slot mapper to be recognized by the BIOS.

### Phase 2 — Expanded Sub-Slot Architecture

After restart detection, the firmware reinitializes PIO and sets up:

| Sub-slot | Content | Address Range |
|----------|---------|---------------|
| 0 | Nextor ROM (128KB, 8 pages) + IDE registers | `0x4000`–`0x7FFF` |
| 1 | Mapper RAM (1MB, 64 pages) | All four 16KB pages |

- **Sub-slot register** (`0xFFFF`): Read returns bitwise complement; write sets sub-slot configuration
- **Mapper page registers** (I/O ports `0xFC`–`0xFF`): Intercepted by PIO1 state machines, controlling which 16KB page of mapper RAM is visible in each CPU page
- **1MB mapper RAM**: Backed by external PSRAM mapped at the uncached QMI CS1 window (`0x15000000`)
- The ROM cache is disabled (`rom_cache_capacity = 0`) because mapper RAM now lives in external PSRAM instead of the internal SRAM cache pool

### PIO1 Usage in Mapper Mode

| SM | Program | Function |
|----|---------|----------|
| SM0 | `msx_io_read_responder` | Returns mapper page register values for reads on ports FC–FF |
| SM1 | `msx_io_write_captor` | Captures mapper page register writes on ports FC–FF |

## 7. Optional WiFi Overlay (`-w`)

The `-w` flag augments the four standard Sunrise modes:

| Base Mode | With `-w` |
|---|---|
| `-s1` | microSD Nextor + WiFi |
| `-m1` | microSD Nextor + 1MB mapper + WiFi |
| `-s2` | USB Nextor + WiFi |
| `-m2` | USB Nextor + 1MB mapper + WiFi |

Implementation summary:

- The host tool sets bit `0x20` in the configuration record.
- The firmware keeps the original base mapper type and switches to WiFi-aware Sunrise handlers.
- A dedicated 16KB ESP8266P system ROM is exposed in its own expanded subslot.
- The memio UART interface is mapped at `0x7F05`-`0x7F07` inside that same subslot.

Subslot layout with WiFi enabled:

| Mode Family | Sub-slot 0 | Sub-slot 1 | Sub-slot 2 |
|---|---|---|---|
| `-s1 -w`, `-s2 -w` | Nextor + IDE | ESP8266P ROM + memio UART | unused |
| `-m1 -w`, `-m2 -w` | Nextor + IDE | ESP8266P ROM + memio UART | 1MB PSRAM mapper |

The WiFi implementation is intentionally not enabled for `-c1` and `-c2` in the current firmware.

## 8. ATA IDENTIFY DEVICE Response

Both backends produce a 512-byte IDENTIFY DEVICE response. The key fields:

| Word(s) | Field | USB Value | SD Value |
|---------|-------|-----------|----------|
| 0 | Configuration | `0x0040` (fixed) | `0x0040` (fixed) |
| 1, 3, 6 | CHS geometry | From USB block count | From SD sector count |
| 10–19 | Serial number | `"PICOVERSE00000001"` | `"PICOVERSE00000001"` |
| 23–26 | Firmware revision | USB SCSI product rev | CID Product Revision |
| 27–46 | Model number | USB SCSI vendor + product | CID OEM ID + Product Name |
| 49 | Capabilities | `0x0200` (LBA) | `0x0200` (LBA) |
| 60–61 | Total LBA sectors | USB block count | SD sector count |

The USB backend reads SCSI INQUIRY fields directly from the USB device. The SD backend reads the CID register via `ext_str()` and `ext_bits16()`. Both populate the shared ATA state via `sunrise_ide_set_device_info()`, which writes to the `usb_block_count`, `usb_block_size`, and `inquiry_resp` variables used by `build_identify_data()`.

## 9. Key Differences: USB vs. SD Backend

| Aspect | USB Backend | SD Backend |
|--------|-------------|------------|
| **Transfer model** | Asynchronous (TinyUSB callbacks) | Synchronous (blocking `disk_read`/`disk_write`) |
| **Timeout handling** | 3-second watchdog in Core 1 loop | Not needed (SPI is deterministic) |
| **Native sector size** | Variable (512, 4096, etc.) | Always 512 bytes |
| **4K sector support** | Yes — reads native block, extracts 512-byte slice | N/A |
| **Hot-plug** | Yes — `tuh_msc_mount_cb`/`umount_cb` | No — initialized once at startup |
| **Device info source** | SCSI INQUIRY response | CID register |
| **Write with non-512 sectors** | Returns error (would need read-modify-write) | Always 512-byte aligned |
| **Core 1 dependencies** | TinyUSB, `tinyusb_board`, `tinyusb_host` | `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico`, `hw_config.c` |
| **Initialization time** | Variable (USB enumeration + INQUIRY) | Fast (~100ms SPI init) |
| **IDENTIFY before ready** | Defers to BSY + `usb_identify_pending` | Usually ready before first IDENTIFY |

## 10. Firmware Dispatch

The `main()` function in `loadrom.c` reads the mapper type from the configuration record and dispatches accordingly:

```c
switch (base_rom_type) {
    // ... cases 1-9 (standard mappers) ...
    case 10: wifi_enabled ? loadrom_sunrise_wifi(ROM_RECORD_SIZE, true)
                          : loadrom_sunrise(ROM_RECORD_SIZE, true);          break;
    case 11: wifi_enabled ? loadrom_sunrise_mapper_wifi(ROM_RECORD_SIZE, true)
                          : loadrom_sunrise_mapper(ROM_RECORD_SIZE, true);   break;
    // ... cases 12-14 ...
    case 15: wifi_enabled ? loadrom_sunrise_wifi_sd(ROM_RECORD_SIZE, true)
                          : loadrom_sunrise_sd(ROM_RECORD_SIZE, true);       break;
    case 16: wifi_enabled ? loadrom_sunrise_mapper_wifi_sd(ROM_RECORD_SIZE, true)
                          : loadrom_sunrise_mapper_sd(ROM_RECORD_SIZE, true); break;
}
```

The SD functions (`loadrom_sunrise_sd`, `loadrom_sunrise_mapper_sd`) are structurally identical to their USB counterparts but launch `sunrise_sd_task` on Core 1 instead of `sunrise_usb_task`, and call `sunrise_sd_set_ide_ctx()` instead of `sunrise_usb_set_ide_ctx()`.

When WiFi is enabled, the dispatch keeps the same base mapper type and swaps in the WiFi-aware Sunrise handlers instead.

## 11. LoadROM Tool Usage

### Command-Line Options

```
loadrom.exe [-h] [-s1] [-m1] [-s2] [-m2] [-c1] [-c2] [-w] [-scc] [-sccplus] [-o <filename>] [romfile]
```

| Option | Long Form | Description |
|--------|-----------|-------------|
| `-s1` | `--sunrise-sd` | Sunrise IDE with microSD card |
| `-m1` | `--mapper-sd` | Sunrise IDE + 1MB PSRAM mapper with microSD card |
| `-s2` | `--sunrise-usb` | Sunrise IDE with USB pendrive |
| `-m2` | `--mapper-usb` | Sunrise IDE + 1MB PSRAM mapper with USB pendrive |
| `-w` | `--wifi` | Add the ESP8266P system ROM + memio UART WiFi surface to `-s1`/`-m1`/`-s2`/`-m2` |

The Sunrise and Carnivore2 base options are mutually exclusive. The `-w` flag is valid only with `-s1`, `-m1`, `-s2`, or `-m2`.

### Examples

```bash
# microSD standalone
loadrom.exe -s1
loadrom.exe -s1 -o nextor_sd.uf2
loadrom.exe -s1 -w -o nextor_sd_wifi.uf2

# microSD + 1MB PSRAM mapper
loadrom.exe -m1
loadrom.exe -m1 -w -o nextor_mapper_sd_wifi.uf2

# USB standalone
loadrom.exe -s2
loadrom.exe -s2 -o nextor_usb.uf2
loadrom.exe -s2 -w -o nextor_usb_wifi.uf2

# USB + 1MB PSRAM mapper
loadrom.exe -m2
loadrom.exe -m2 -w -o nextor_mapper_usb_wifi.uf2
```

### Typical Workflow

1. Run `loadrom.exe` with the desired Nextor option.
2. Review the console output (ROM type, name, size).
3. Hold BOOTSEL on the PicoVerse 2350 and connect USB.
4. Copy the generated UF2 to the `RPI-RP2` drive.
5. For `-s1`/`-m1`: Insert a FAT-formatted microSD card into the PicoVerse 2350 card slot.
6. For `-s2`/`-m2`: Connect a USB flash drive to the USB-C port (via OTG adapter if needed).
7. Insert the cartridge into the MSX and power on.

## 12. Build Configuration

### CMakeLists.txt

The build includes both USB and SD dependencies:

```cmake
add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src build_sd_lib)

add_executable(loadrom
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    loadrom.c
    emu2212.c
    sunrise_ide.c
    sunrise_sd.c
    hw_config.c)

target_link_libraries(loadrom
    pico_stdlib
    pico_multicore
    pico_audio_i2s
    hardware_dma
    hardware_pio
    tinyusb_board
    tinyusb_host
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
```

Both backends are compiled into every firmware binary. The runtime dispatch in `main()` selects which backend runs based on the mapper type stored in the configuration record.

The WiFi-enabled Sunrise variants additionally use the host tool to append the ESP8266P ROM after the Sunrise ROM payload in the UF2 image, while the firmware keeps only the UART/memio backend and serves the WiFi ROM from the appended flash payload.

### Tool Makefile

The PC tool Makefile generates `nextor_sunrise.h` (the embedded ROM as a C byte array) from:

```
nextor/kernel/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM → src/nextor_sunrise.h
```

This header is shared by all four Nextor modes.

## 13. Source File Reference

| File | Purpose |
|------|---------|
| `pico/loadrom/sunrise_ide.h` | Sunrise IDE address map, ATA constants, IDE state machine, `sunrise_ide_t` context, public API |
| `pico/loadrom/sunrise_ide.c` | ATA command handling, data register latch, mapper bit reversal, USB host backend (`sunrise_usb_task`), IDENTIFY builder, `sunrise_ide_set_device_info()` |
| `pico/loadrom/sunrise_sd.h` | SD backend API: `sunrise_sd_set_ide_ctx()`, `sunrise_sd_task()` |
| `pico/loadrom/sunrise_sd.c` | SD card initialization, CID extraction, synchronous block I/O backend (`sunrise_sd_task`) |
| `pico/loadrom/hw_config.c` | SPI pin configuration for SD card (CS=33, SCK=34, MOSI=35, MISO=36) |
| `pico/loadrom/tusb_config.h` | TinyUSB host mode configuration for USB MSC |
| `pico/loadrom/loadrom.c` | Firmware main: Sunrise, mapper, Carnivore2, and WiFi-aware Sunrise handlers |
| `wifi/ESP8266P.rom` | ESP8266P system ROM asset used by the WiFi-enabled Sunrise builds |
| `tool/src/esp8266p_rom.h` | Generated host-tool header created from `wifi/ESP8266P.rom` and appended to the UF2 image |

For the full WiFi-specific register map and usage notes, see `docs/msx-picoverse-2350-wifi.md`.
| `pico/loadrom/loadrom.h` | Pin definitions, SRAM pool, mapper constants |
| `pico/loadrom/CMakeLists.txt` | Build configuration with both USB and SD dependencies |
| `tool/src/loadrom.c` | PC tool: `-s1`/`-m1`/`-s2`/`-m2` options, UF2 generation |
| `tool/Makefile` | Build scripts, embedded ROM header generation |
| `nextor/kernel/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM` | 128KB Nextor Sunrise IDE kernel |
| `lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/` | Carl Kugler's FatFS + SPI SD driver library |

All paths are relative to `2350/software/loadrom.pio/`.

## 13. Comparison with PicoVerse 2040

| Feature | PicoVerse 2040 | PicoVerse 2350 |
|---------|----------------|----------------|
| USB Sunrise IDE | Yes (`-s`) | Yes (`-s2`) |
| USB Sunrise + Mapper | Yes (`-m`, 192KB) | Yes (`-m2`, 1MB) |
| microSD Sunrise IDE | No | Yes (`-s1`) |
| microSD Sunrise + Mapper | No | Yes (`-m1`, 1MB) |
| Mapper RAM | 192KB (12 pages) | 1MB (64 pages) |
| SD card slot | No | Yes (SPI0, GPIO 33–36) |
| Nextor ROM | Nextor 2.1.4 Sunrise IDE | Nextor 2.1.4 Sunrise IDE |
| ATA front-end | Identical | Identical |
| Device info | USB SCSI INQUIRY | USB SCSI INQUIRY or SD CID |

The ATA front-end code (`sunrise_ide.c`) is shared between both platforms. The 2350 simply adds the SD backend as an alternative to USB.

Author: Cristiano Goncalves  
Last updated: 03/29/2026
