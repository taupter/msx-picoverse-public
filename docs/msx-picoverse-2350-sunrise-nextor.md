# MSX PicoVerse 2350 — Sunrise IDE Emulation for Nextor

This document describes the implementation of the four Sunrise IDE interface emulation modes in the MSX PicoVerse 2350 firmware. The 2350 variant supports two distinct storage backends — microSD card and USB mass storage — each available standalone or combined with a 1MB PSRAM-backed memory mapper.

## 1. Overview

The MSX PicoVerse 2350 provides a complete Sunrise IDE emulation that allows the MSX to boot Nextor DOS and access mass storage through either the on-board microSD card slot or the USB-C port. Four firmware modes are available:

| Option | Mapper Type | Storage Backend | Description |
|--------|-------------|-----------------|-------------|
| `-s1` / `--sunrise-sd` | 15 | microSD (SPI) | Sunrise IDE with microSD card |
| `-m1` / `--mapper-sd` | 16 | microSD (SPI) | Sunrise IDE + 1MB PSRAM mapper with microSD card |
| `-s2` / `--sunrise-usb` | 10 | USB-C (MSC) | Sunrise IDE with USB pendrive |
| `-m2` / `--mapper-usb` | 11 | USB-C (MSC) | Sunrise IDE + 1MB PSRAM mapper with USB pendrive |

All four modes:

- Emulate the Sunrise IDE FlashROM banking for the 128KB Nextor ROM image (8 × 16KB pages)
- Emulate the full ATA task-file register set at `0x7C00`–`0x7EFF`
- Translate ATA sector read/write commands into block I/O operations on the chosen storage device
- Run the Nextor 2.1.4 Sunrise IDE driver (Master Only edition)
- Use the RP2350's dual-core architecture: Core 0 for PIO bus and IDE register handling, Core 1 for storage backend

The mapper variants (`-m1`, `-m2`) additionally provide 1MB of mapper RAM (64 × 16KB pages) in an expanded sub-slot architecture, with mapper page registers intercepted via PIO1.

## 2. Architecture

All four modes share an identical ATA front-end on Core 0. Core 1 runs the appropriate storage backend depending on the mapper type.

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

## 7. ATA IDENTIFY DEVICE Response

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

## 8. Key Differences: USB vs. SD Backend

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

## 9. Firmware Dispatch

The `main()` function in `loadrom.c` reads the mapper type from the configuration record and dispatches accordingly:

```c
switch (base_rom_type) {
    // ... cases 1-9 (standard mappers) ...
    case 10: loadrom_sunrise(ROM_RECORD_SIZE, true);         break; // USB
    case 11: loadrom_sunrise_mapper(ROM_RECORD_SIZE, true);  break; // USB + mapper
    // ... cases 12-14 ...
    case 15: loadrom_sunrise_sd(ROM_RECORD_SIZE, true);      break; // SD
    case 16: loadrom_sunrise_mapper_sd(ROM_RECORD_SIZE, true); break; // SD + mapper
}
```

The SD functions (`loadrom_sunrise_sd`, `loadrom_sunrise_mapper_sd`) are structurally identical to their USB counterparts but launch `sunrise_sd_task` on Core 1 instead of `sunrise_usb_task`, and call `sunrise_sd_set_ide_ctx()` instead of `sunrise_usb_set_ide_ctx()`.

## 10. LoadROM Tool Usage

### Command-Line Options

```
loadrom.exe [-h] [-s1] [-m1] [-s2] [-m2] [-scc] [-sccplus] [-o <filename>] [romfile]
```

| Option | Long Form | Description |
|--------|-----------|-------------|
| `-s1` | `--sunrise-sd` | Sunrise IDE with microSD card |
| `-m1` | `--mapper-sd` | Sunrise IDE + 1MB PSRAM mapper with microSD card |
| `-s2` | `--sunrise-usb` | Sunrise IDE with USB pendrive |
| `-m2` | `--mapper-usb` | Sunrise IDE + 1MB PSRAM mapper with USB pendrive |

The four options are mutually exclusive. Each embeds the same Nextor 2.1.4 Sunrise IDE ROM (128KB) but sets different mapper type codes in the configuration record.

### Examples

```bash
# microSD standalone
loadrom.exe -s1
loadrom.exe -s1 -o nextor_sd.uf2

# microSD + 1MB PSRAM mapper
loadrom.exe -m1

# USB standalone
loadrom.exe -s2
loadrom.exe -s2 -o nextor_usb.uf2

# USB + 1MB PSRAM mapper
loadrom.exe -m2
```

### Typical Workflow

1. Run `loadrom.exe` with the desired Nextor option.
2. Review the console output (ROM type, name, size).
3. Hold BOOTSEL on the PicoVerse 2350 and connect USB.
4. Copy the generated UF2 to the `RPI-RP2` drive.
5. For `-s1`/`-m1`: Insert a FAT-formatted microSD card into the PicoVerse 2350 card slot.
6. For `-s2`/`-m2`: Connect a USB flash drive to the USB-C port (via OTG adapter if needed).
7. Insert the cartridge into the MSX and power on.

## 11. Build Configuration

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

### Tool Makefile

The PC tool Makefile generates `nextor_sunrise.h` (the embedded ROM as a C byte array) from:

```
nextor/kernel/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM → src/nextor_sunrise.h
```

This header is shared by all four Nextor modes.

## 12. Source File Reference

| File | Purpose |
|------|---------|
| `pico/loadrom/sunrise_ide.h` | Sunrise IDE address map, ATA constants, IDE state machine, `sunrise_ide_t` context, public API |
| `pico/loadrom/sunrise_ide.c` | ATA command handling, data register latch, mapper bit reversal, USB host backend (`sunrise_usb_task`), IDENTIFY builder, `sunrise_ide_set_device_info()` |
| `pico/loadrom/sunrise_sd.h` | SD backend API: `sunrise_sd_set_ide_ctx()`, `sunrise_sd_task()` |
| `pico/loadrom/sunrise_sd.c` | SD card initialization, CID extraction, synchronous block I/O backend (`sunrise_sd_task`) |
| `pico/loadrom/hw_config.c` | SPI pin configuration for SD card (CS=33, SCK=34, MOSI=35, MISO=36) |
| `pico/loadrom/tusb_config.h` | TinyUSB host mode configuration for USB MSC |
| `pico/loadrom/loadrom.c` | Firmware main: `loadrom_sunrise`, `loadrom_sunrise_mapper`, `loadrom_sunrise_sd`, `loadrom_sunrise_mapper_sd` |
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
