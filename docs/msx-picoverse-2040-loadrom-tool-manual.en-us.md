# MSX PicoVerse 2040 LoadROM Tool Manual (en-US)

The PicoVerse 2040 cartridge extends MSX systems by flashing different Raspberry Pi Pico firmwares. While the MultiROM firmware offers a menu-driven launcher, some workflows require flashing a single ROM image that boots immediately on power‑on. That is the purpose of the `loadrom` firmware and of the companion `loadrom.exe` console tool documented here.

`loadrom.exe` bundles the Pico firmware, a configuration record (game name, mapper code, ROM size and offset), and a single MSX ROM payload into an RP2040-compatible UF2 image. Copying the generated UF2 to the Pico’s `RPI-RP2` drive programs the cartridge so that it boots directly into the embedded ROM whenever the MSX starts.

Alternatively, the `-s` (Sunrise) option can be used to flash the cartridge with the Sunrise IDE Nextor firmware, which exposes the USB-C port as a block device for use with Nextor-compatible loaders like SofaRun.

The `-m` option flashes Sunrise IDE Nextor with an additional 192KB memory mapper implementation. In the firmware internals this is mapper mode (type `11`), while on-screen mapper text is still presented as `SYSTEM` for end users.

The standalone firmware options `-k`, `-i`, and `-p` generate dedicated USB keyboard, MSX-MIDI, and MIDI-PAC PSG-to-MIDI converter cartridges without embedding a ROM payload.

### LoadRom option matrix

| Option | Nextor ROM | Extra mapper RAM | Intended use |
| --- | --- | --- | --- |
| `-s` | Sunrise IDE Nextor | No | Standard Nextor USB mass-storage mode |
| `-m` | Sunrise IDE Nextor | Yes, 192KB | Nextor workflows that need mapper-capable behavior |
| `-k` | — | — | Standalone USB keyboard cartridge mode |
| `-i` | — | — | Standalone MSX-MIDI cartridge mode (USB MIDI interface) |
| `-p` | — | — | Standalone MIDI-PAC cartridge mode (passive PSG-to-MIDI converter) |
## Overview

1. **Input**: one `.ROM` file (case-insensitive extension) supplied via the command line.
2. **Processing**: the tool trims/normalizes the ROM name, detects or forces the mapper type, and streams the Pico firmware, configuration record, and ROM payload into UF2 blocks.
3. **Output**: a UF2 file (default `loadrom.uf2`) that can be copied to the Pico while it is in BOOTSEL mode.

Key characteristics:

- Works on Windows (console app). Tested in `cmd.exe` and PowerShell.
- Supports ROM sizes from 8 KB up to 16 MB (subject to Pico flash capacity).
- Detects common mapper types automatically. Mapper can be forced via filename tags (same scheme as `multirom`): `PLA-16`, `PLA-32`, `KonSCC`, `PLN-48`, `ASC-08`, `ASC-16`, `ASC-16X`, `Konami`, `NEO-8`, `NEO-16`, `PLN-64`.
- Generates UF2 files recognized by the RP2040 ROM bootloader (sets the RP2040 family ID flag).

## Command-line usage

Only the Windows executable is currently distributed.

### Basic syntax

```
loadrom.exe [options] [romfile]
```

### Options

- `-h`, `--help` : Print usage information and exit.
- `-s`, `--sunrise` : Embed the Sunrise IDE Nextor ROM instead of a regular MSX ROM. When this option is used, the cartridge boots into Nextor firmware, and the USB-C port exposes a block device for use with Nextor loaders. No ROM file argument is needed with this option.
- `-m`, `--mapper` : Embed Sunrise IDE Nextor with 192KB memory mapper support. This is intended for Nextor workflows that require mapper-capable behavior. No ROM file argument is needed with this option.
- `-k`, `--keyboard` : Build a UF2 with the standalone USB keyboard firmware. The cartridge becomes a dedicated USB-to-MSX keyboard interface. This option is standalone and cannot be combined with `-s`, `-m`, `-i`, or a ROM file.
- `-i`, `--midi` : Build a UF2 with the standalone MSX-MIDI firmware. The cartridge becomes a dedicated MSX-MIDI interface that bridges MSX-MIDI I/O ports (`0xE8`–`0xE9`) to a USB MIDI device connected to the USB-C port. This option is standalone and cannot be combined with `-s`, `-m`, `-k`, `-p`, or a ROM file.
- `-p`, `--midipac` : Build a UF2 with the standalone MIDI-PAC firmware. The cartridge becomes a passive PSG-to-MIDI converter that listens to AY-3-8910 / YM2149 PSG writes on ports `0xA0`–`0xA1` and forwards a musical MIDI interpretation to a USB MIDI device connected to the USB-C port. This option is standalone and cannot be combined with `-s`, `-m`, `-k`, `-i`, or a ROM file.
- `-o <filename>`, `--output <filename>` : Override the UF2 output name (default `loadrom.uf2`).
- Positional argument: the path to the MSX ROM you wish to embed (not used with `-s`, `-m`, `-k`, `-i`, or `-p`).

### Mapper forcing via filename tags

`loadrom.exe` shares the same forcing mechanism as the MultiROM tool. Append a dot-separated mapper tag before the `.ROM` extension to override detection:

```
Penguin Adventure.PLA-32.ROM
Space Manbow.KonSCC.rom
```

Tags are case-insensitive. If no valid tag is present, the tool falls back to heuristic detection.

## Typical workflow

### Loading a regular MSX ROM

1. Place `loadrom.exe` in a working directory alongside the desired ROM file.
2. Open a Command Prompt or PowerShell window in that directory.
3. Run `loadrom.exe` pointing to the ROM and optionally overriding the UF2 name:
   ```
   loadrom.exe "Space Manbow.rom" -o space_manbow.uf2
   ```
4. Observe the console output:
   - Tool banner with embedded version.
   - Resolved ROM name (max 50 characters), size, mapper result, and Pico flash offset.
   - Progress message when UF2 blocks are written.
5. Put the Pico in BOOTSEL mode (hold BOOTSEL, plug USB) to mount the `RPI-RP2` drive.
6. Copy the generated UF2 file (e.g., `space_manbow.uf2`) to the drive. The Pico reboots once flashing completes.
7. Insert the PicoVerse cartridge into the MSX and power on—the embedded ROM boots immediately without a menu.

### Loading Sunrise IDE Nextor

1. Place `loadrom.exe` in a working directory.
2. Open a Command Prompt or PowerShell window in that directory.
3. Run `loadrom.exe` with the `-s` option and set the output filename:
   ```
   loadrom.exe -s -o nextor_sunrise.uf2
   ```
4. Put the Pico in BOOTSEL mode (hold BOOTSEL, plug USB) to mount the `RPI-RP2` drive.
5. Copy the generated UF2 file to the drive. The Pico reboots once flashing completes.
6. Insert the PicoVerse cartridge into the MSX with a USB thumb drive connected (use a USB OTG adapter if needed).
7. Power on the MSX to boot into Nextor. The cartridge's USB-C port now acts as a block device for loading ROMs and DSK files.

### Loading Sunrise IDE Nextor + 192KB Mapper

1. Place `loadrom.exe` in a working directory.
2. Open a Command Prompt or PowerShell window in that directory.
3. Run `loadrom.exe` with the `-m` option and set the output filename:
   ```
   loadrom.exe -m -o nextor_sunrise_mapper.uf2
   ```
4. Put the Pico in BOOTSEL mode (hold BOOTSEL, plug USB) to mount the `RPI-RP2` drive.
5. Copy the generated UF2 file to the drive. The Pico reboots once flashing completes.
6. Insert the PicoVerse cartridge into the MSX with a USB thumb drive connected (use a USB OTG adapter if needed).
7. Power on the MSX to boot into Nextor with the mapper-capable mode enabled. UI mapper text is shown as `SYSTEM`.
8. In this mode, the additional memory mapper capacity is 192KB (12 x 16KB pages).

### Loading USB Keyboard Firmware

1. Place `loadrom.exe` in a working directory.
2. Open a Command Prompt or PowerShell window in that directory.
3. Run `loadrom.exe` with the `-k` option:
   ```
   loadrom.exe -k -o keyboard.uf2
   ```
4. Put the Pico in BOOTSEL mode (hold BOOTSEL, plug USB) to mount the `RPI-RP2` drive.
5. Copy the generated UF2 file to the drive. The Pico reboots once flashing completes.
6. Connect a USB keyboard to the cartridge's USB-C port (use a USB-C OTG adapter or USB-A to USB-C adapter as needed).
7. Insert the PicoVerse cartridge into the MSX and power on. The USB keyboard now functions as the MSX keyboard.

> **Note**: The keyboard firmware does not work with FPGA-based MSX systems. The FPGA's internal PPI drives the slot data bus for keyboard ports, causing bus contention with the PicoVerse. It is designed for and tested on original MSX hardware only.

### Loading MSX-MIDI Firmware

1. Place `loadrom.exe` in a working directory.
2. Open a Command Prompt or PowerShell window in that directory.
3. Run `loadrom.exe` with the `-i` option:
   ```
   loadrom.exe -i -o midi.uf2
   ```
4. Put the Pico in BOOTSEL mode (hold BOOTSEL, plug USB) to mount the `RPI-RP2` drive.
5. Copy the generated UF2 file to the drive. The Pico reboots once flashing completes.
6. Connect a USB-MIDI cable to the cartridge's USB-C port (use a USB-C OTG adapter if needed), and connect the cable's MIDI OUT to your MIDI device (e.g., Roland SoundCanvas).
7. Insert the PicoVerse cartridge into the MSX and power on. The cartridge now emulates the standard MSX-MIDI interface on ports `0xE8`–`0xEF`.
8. Run MIDI software from MSX-DOS. For MIDRY, use the `/I5` option: `MIDRY /I5 filename.MID`.

> **Note**: The `-i` option is standalone — the USB port is dedicated to the MIDI cable. You need a separate way to boot MSX-DOS (e.g., another cartridge with Nextor, or the MSX's built-in disk drive).

### Loading MIDI-PAC Firmware

1. Place `loadrom.exe` in a working directory.
2. Open a Command Prompt or PowerShell window in that directory.
3. Run `loadrom.exe` with the `-p` option:
   ```
   loadrom.exe -p -o midipac.uf2
   ```
4. Put the Pico in BOOTSEL mode (hold BOOTSEL, plug USB) to mount the `RPI-RP2` drive.
5. Copy the generated UF2 file to the drive. The Pico reboots once flashing completes.
6. Connect a USB MIDI cable, USB MIDI module, or USB-equipped synthesizer to the cartridge's USB-C port. (Roland Sound Canvas SC-55 and compatible modules are recommended.)
7. Insert the PicoVerse cartridge into the MSX and power on. The cartridge now passively monitors PSG traffic on ports `0xA0`–`0xA1` and converts music and sound effects to MIDI in real time.
8. Boot and run the MSX software you want to hear through the external MIDI module.

See [MSX PicoVerse 2040 MIDI-PAC](/docs/msx-picoverse-2040-midipac.md) for in-depth documentation on architecture, conversion algorithms, and limitations.

> **Note**: The `-p` option is standalone. MIDI-PAC does not provide ROM loading, mass storage, or MSX-MIDI ports; it is a dedicated PSG-to-MIDI conversion firmware. Requires a USB MIDI cable and a device capable of General MIDI playback for best results.

## Troubleshooting

| Symptom | Possible cause | Resolution |
| --- | --- | --- |
| "Invalid ROM file" | Missing or wrong extension | Ensure the ROM ends with `.ROM` (case-insensitive). |
| "Failed to detect the ROM type" | Mapper heuristics inconclusive | Add a mapper tag suffix (e.g., `.Konami.ROM`). |
| UF2 not recognized by Pico | File copied before BOOTSEL, or wrong family ID | Enter BOOTSEL mode before copying, and regenerate the UF2 with the latest tool. |
| Game title looks truncated | Name exceeds 50 characters | Use shorter filenames; the config record stores 50 bytes max. |

## Known limitations

- With a regular ROM: only one ROM can be embedded per UF2; use the MultiROM tool for menus and multiple titles.
- With Sunrise Nextor: requires a compatible USB thumb drive and appropriate Nextor loaders on the MSX side.
- In mapper mode (`-m`), mapper register handling is global (ports `FC`-`FF`) and the mapper capacity is 192KB.
- Linux/macOS binaries are not yet provided; use Wine or build from source with GCC.
- The tool does not verify ROM integrity beyond size checks and simple header heuristics.
- The keyboard firmware (`-k`) does not work with FPGA-based MSX implementations. The FPGA's internal PPI (8255) unconditionally drives keyboard port data onto the cartridge slot bus, which conflicts with the PicoVerse's bus response. See [USB Keyboard — FPGA Incompatibility Details](/docs/msx-picoverse-2040-keyboard.md#fpga-incompatibility-details) for the full analysis.
- The MIDI-PAC firmware (`-p`) depends on the target software using the standard PSG ports and on the connected MIDI device producing a musically useful result from the converted stream; it is a converter, not a literal PSG synthesizer.

## Tested MSX models

The PicoVerse 2040 cartridge with LoadROM firmware has been tested when executing ROMs of the supported mappers on the following MSX models:

| Model | Type | Status | Comments |
| --- | --- | --- | --- |
| Adermir Carchano Expert 4 | MSX2+ | OK | Verified operation |
| Gradiente Expert | MSX1 | OK | Verified operation |
| JFF MSX | MSX1 | OK | Verified operation |
| MSX Book | MSX2+ (FPGA clone) | OK | Verified operation |
| MSX One | MSX1 | Not OK | Cartridge not recognized |
| National FS-4500 | MSX1 | OK | Verified operation |
| Omega MSX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1GT | TurboR | OK | Verified operation |
| Panasonic FS-A1ST | TurboR | OK | Verified operation |
| Panasonic FS-A1WX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1WSX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1FX | MSX2+ | OK | Verified operation |
| Sanyo Wavy 70FD | MSX2+ | OK | Verified operation |
| Sharp HotBit HB8000 | MSX1 | OK | Verified operation |
| SMX-HB | MSX2+ (FPGA clone) | OK | Verified operation |
| Sony HB-F1XD | MSX2 | OK | Verified operation |
| Sony HB-F1XDJ | MSX2 | OK | Verified operation |
| Sony HB-F1XV | MSX2+ | OK | Verified operation |
| Sony Hit-Bit 20HB | MSX1 | OK | Verified operation |
| TRHMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| Yamaha YIS604 | MSX1 | OK | Verified operation |

Sunrise IDE Nextor ROM (options `-s` and `-m`) has been tested on the following MSX models:

| Model | Type | Status | Comments |
| --- | --- | --- | --- |
| Gradiente Expert | MSX1 | OK | Verified operation |
| Panasonic FS-A1WX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1WSX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1FX | MSX2+ | OK | Verified operation. Only -m option works correctly as Nextor requires minimum of 128KB RAM and originally this computer has only 64KB of base RAM in slot 3. |
| Sanyo Wavy 70FD | MSX2+ | OK | Verified operation |
| Sharp HotBit HB8000 | MSX1 | OK | Verified operation |
| TRHMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Verified operation |

The USB keyboard firmware (option `-k`) with the PPI rephase has been tested on the following MSX models:

Note: The keyboard firmware is not compatible with FPGA-based MSX implementations due to bus contention issues. It is designed for and tested on original MSX hardware only.

| Model | Type | Status | Comments |
| --- | --- | --- | --- | --- |
|Gradiente Expert | MSX1 | OK | Verified operation |
| Panasonic FS-A1FX | MSX2+ | Not OK | Keyboard does not function correctly. This computer uses the T9769 MSX-ENGINE with the integrated PPI, the firmware is not compatible. |
| Panasonic FS-A1GT | TurboR | Not OK | Keyboard does not function correctly. This computer uses the T9769 MSX-ENGINE with the integrated PPI, the firmware is not compatible. |
| Panasonic FS-A1ST | TurboR | Not OK | Keyboard does not function correctly. This computer uses the T9769 MSX-ENGINE with the integrated PPI, the firmware is not compatible. |
| Sharp HotBit HB8000 | MSX1 | OK | Verified operation |
|Sony HB-101P| MSX1 | OK | Verified operation |

The MSX-MIDI firmware (option `-i`) has been tested on the following MSX models:

| MSX Model | Type | Status | MIDI Devices | Comments |
| --- | --- | --- | --- | --- |
| TRHMSX | MSX2+ (FPGA clone) | OK | Roland SC-55, [AliExpress MIDI/USB Simple Cable](https://s.click.aliexpress.com/e/_c3iqcWDh)  | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Roland SC-55, [AliExpress MIDI/USB Simple Cable](https://s.click.aliexpress.com/e/_c3iqcWDh)  | Verified operation |

The MIDI-PAC firmware (option `-p`) has been tested on the following MSX models and MIDI devices:

| MSX Model | Type | Status | MIDI Devices | Comments |
| --- | --- | --- | --- | --- |
| TRHMSX | MSX2+ (FPGA clone) | OK | Roland SC-55, [AliExpress MIDI/USB Simple Cable](https://s.click.aliexpress.com/e/_c3iqcWDh) | Verified frame-based PSG-to-MIDI conversion, stable vibrato handling, and 6-band percussion mapping |
| uMSX | MSX2+ (FPGA clone) | OK | Roland SC-55, [AliExpress MIDI/USB Simple Cable](https://s.click.aliexpress.com/e/_c3iqcWDh) | Verified operation; note-center stability and pitch-bend behavior confirmed |

Note: MIDI-PAC has been tested with games heavy in melody, modulation loops, arpeggios, envelopes, and percussion. Best results on General MIDI-compatible devices; the current voicing is optimized for Roland Sound Canvas class modules (SC-55, SC-88, etc.) while keeping the three melodic channels slightly forward relative to channel 10.


Author: Cristiano Almeida Goncalves
Last updated: 03/17/2026
