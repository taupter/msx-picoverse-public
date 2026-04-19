# MSX PicoVerse 2350 LoadROM Tool Manual (EN-US)

The PicoVerse 2350 cartridge can boot either a MultiROM menu or a single ROM that starts immediately. The LoadROM tool creates a UF2 image that flashes the PicoVerse 2350 with a single ROM payload and the LoadROM firmware.

`loadrom.exe` bundles the Pico firmware blob, a configuration record (game name, mapper code, ROM size, and flash offset), and one MSX ROM file into an RP2350-compatible UF2 image. Copy the generated UF2 to the board in BOOTSEL mode and the MSX will boot straight into the embedded ROM.

For RP2350, the current LoadROM package is:

- `2350/software/loadrom.pio`.

> **Important:** SCC/SCC+ emulation options (`-scc`, `-sccplus`) are supported by the current RP2350 LoadROM package (`2350/software/loadrom.pio`).

---

## Overview

1. **Input**: one `.ROM` file (case-insensitive extension).
2. **Processing**: the tool normalizes the ROM name, detects or forces the mapper, and streams firmware + config + ROM into UF2 blocks.
3. **Output**: a UF2 file (default `loadrom.uf2`) ready for the RP2350 bootloader.

Key characteristics:

- Windows console application.
- Supports ROM sizes from 8 KB up to 16 MB (subject to flash capacity).
- Mapper auto-detection plus optional filename tags to force the mapper.
- Embedded SHA-1 database derived from openMSX `softwaredb.xml` for improved mapper identification.
- Built-in Sunrise IDE Nextor ROM for standalone Nextor boot or Nextor + 1MB PSRAM mapper, using either the on-board microSD card (`-s1`/`-m1`) or a USB flash drive (`-s2`/`-m2`).
- ROM loading into the 1MB PSRAM mapper through Carnivore2-compatible RAM emulation for `SROM.COM /D15`, using either microSD (`-c1`) or USB mass storage (`-c2`) as the Nextor backend.
- UF2 uses RP2350 family ID (`0xE48BFF59`).

---

## Command-line usage

```
loadrom.exe [options] [romfile]
```

### Options

- `-h`, `--help` : Print usage information and exit.
- `-s1`, `--sunrise-sd` : Build a UF2 with the embedded Sunrise IDE Nextor ROM using the on-board microSD card slot for storage. No external ROM file is needed.
- `-m1`, `--mapper-sd` : Same as `-s1` plus a 1MB memory mapper (64 × 16KB pages) backed by external PSRAM on GPIO47 / QMI CS1 in an expanded sub-slot architecture. No external ROM file is needed.
- `-s2`, `--sunrise-usb` : Build a UF2 with the embedded Sunrise IDE Nextor ROM using the cartridge's USB-C port for storage (USB mass storage). No external ROM file is needed.
- `-m2`, `--mapper-usb` : Same as `-s2` plus a 1MB memory mapper (64 × 16KB pages) backed by external PSRAM on GPIO47 / QMI CS1 in an expanded sub-slot architecture. No external ROM file is needed.
- `-c1`, `--carnivore2-sd` : Build a UF2 with Sunrise IDE Nextor on microSD plus the 1MB PSRAM mapper and Carnivore2 RAM-mode emulation for `SROM.COM /D15`. In this mode, `SROM.COM` can upload a ROM image into the PSRAM-backed mapper and launch it from RAM without reflashing the cartridge.
- `-c2`, `--carnivore2-usb` : Same as `-c1`, but uses USB mass storage on the cartridge's USB-C port as the Nextor backend.
- `-scc`, `--scc` : Enable SCC (standard) sound emulation. For embedded ROM builds, this applies to Konami SCC or Manbow2 mapper ROMs. With `-c1` or `-c2`, it enables SCC playback for Konami SCC ROMs uploaded later through `SROM.COM /D15`.
- `-sccplus`, `--sccplus` : Enable SCC+ (enhanced) sound emulation. For embedded ROM builds, this applies to Konami SCC or Manbow2 mapper ROMs. With `-c1` or `-c2`, it enables SCC+ playback for compatible ROMs uploaded later through `SROM.COM /D15`.
- `-o <filename>`, `--output <filename>` : Override the UF2 output name (default `loadrom.uf2`).
- Positional argument: the ROM file to embed. Required for normal ROM loading; not accepted with `-s1`/`-m1`/`-s2`/`-m2`/`-c1`/`-c2`.

`-s1`, `-m1`, `-s2`, `-m2`, `-c1`, and `-c2` are mutually exclusive. `-scc` and `-sccplus` are mutually exclusive. If conflicting options are provided, the tool exits with an error.

### Mapper forcing via filename tags

Append a dot-separated mapper tag before the `.ROM` extension to override detection. Tags are case-insensitive.

Supported tags:
`PLA-16`, `PLA-32`, `KonSCC`, `PLN-48`, `PLN-64`, `ASC-08`, `ASC-16`, `ASC-16X`, `Konami`, `NEO-8`, `NEO-16`, `MANBW2`.

Additional aliases are accepted for backward compatibility: `PL-16`, `PL-32`, `PL-48`, `PL-64`, `PLN-32`, `PLANAR`, `LINEAR`, `LINEAR0`, `PLANAR48`, `PLANAR64`, `MANBOW2`, `MBW-2`.

Example:

```
Penguin Adventure.PL-32.ROM
Space Manbow.KonSCC.rom
```

Tags are case-insensitive. If no valid tag is present, the tool first computes the ROM's SHA-1 hash and looks it up in an embedded database derived from the openMSX `softwaredb.xml`. When a match is found the database mapper type is used directly. Otherwise the tool falls back to heuristic detection.

`SYSTEM` is ignored and cannot be forced.

---

## Typical workflow

1. Place `loadrom.exe` and your ROM file in a working folder.
2. Open a Command Prompt or PowerShell window in that folder.
3. Run the tool:
   ```
   loadrom.exe "Space Manbow.rom" -o space_manbow.uf2
   ```
   Sunrise IDE standalone (Nextor from microSD card):
   ```
   loadrom.exe -s1
   loadrom.exe -s1 -o nextor_sd.uf2
   ```
   Sunrise IDE + 1MB PSRAM mapper (microSD):
   ```
   loadrom.exe -m1
   loadrom.exe -m1 -o nextor_mapper_sd.uf2
   ```
   Sunrise IDE standalone (Nextor from USB flash drive):
   ```
   loadrom.exe -s2
   loadrom.exe -s2 -o nextor_usb.uf2
   ```
   Sunrise IDE + 1MB PSRAM mapper (USB):
   ```
   loadrom.exe -m2
   loadrom.exe -m2 -o nextor_mapper_usb.uf2
   ```
   Carnivore2 RAM-mode loader for `SROM.COM /D15` (microSD):
   ```
   loadrom.exe -c1
   loadrom.exe -c1 -o srom_c2_sd.uf2
   ```
   Carnivore2 RAM-mode loader for `SROM.COM /D15` (USB):
   ```
   loadrom.exe -c2
   loadrom.exe -c2 -o srom_c2_usb.uf2
   ```
   Carnivore2 RAM-mode loader with SCC audio for SROM-loaded Konami SCC ROMs:
   ```
   loadrom.exe -c1 -scc -o srom_c2_sd_scc.uf2
   loadrom.exe -c2 -scc -o srom_c2_usb_scc.uf2
   ```
   SCC/SCC+ examples:
   ```
   loadrom.exe -scc "Space Manbow.rom"
   loadrom.exe -sccplus "Snatcher.KonSCC.rom"
   ```
4. Review the console output (name, size, mapper, and flash offset).
5. Hold BOOTSEL while connecting the PicoVerse 2350 to USB.
6. Copy the generated UF2 to the `RPI-RP2` drive.
7. Insert the cartridge into the MSX and power on.
8. For `-s1`/`-m1` modes, insert a FAT-formatted microSD card into the cartridge's microSD slot before powering the MSX.
9. For `-s2`/`-m2` modes, connect a USB flash drive (via OTG adapter if needed) to the cartridge's USB-C port before powering the MSX.
10. For `-c1`/`-c2` modes, boot Nextor first and then use `SROM.COM /D15` to upload the ROM into the PicoVerse PSRAM mapper. The cartridge is presented as a Carnivore2-compatible RAM target so the uploaded ROM can be launched directly from RAM.

---

## Output layout details

The UF2 image contains:

1. **Firmware blob** – embedded `loadrom` firmware.
2. **Configuration record** (59 bytes):
   - 50 bytes: ROM name (ASCII, padded/truncated).
   - 1 byte : mapper ID and optional SCC/SCC+ flags:
     - Bit 7 (`0x80`) = SCC emulation enabled.
     - Bit 6 (`0x40`) = SCC+ emulation enabled.
     - Bits 0..5 = base mapper ID.
   - 4 bytes: ROM size (little-endian).
   - 4 bytes: ROM flash offset (little-endian).
3. **ROM payload** – raw ROM data appended after the config record.

The UF2 writer sets `UF2_FLAG_FAMILYID_PRESENT` and uses the RP2350 family ID (`0xE48BFF59`) so the bootloader accepts the image.

---

## Troubleshooting

| Symptom | Possible cause | Resolution |
| --- | --- | --- |
| "Invalid ROM size" | ROM < 8 KB or > 16 MB | Use a valid ROM size. |
| "Failed to detect the ROM type" | Mapper heuristics failed | Add a mapper tag (e.g., `.Konami.ROM`). |
| "Sunrise options are mutually exclusive" | More than one of `-s1`/`-m1`/`-s2`/`-m2`/`-c1`/`-c2` passed | Use only one firmware mode at a time. The `-m` variants add mapper RAM; the `-c` variants add Carnivore2 RAM-mode loading on top of the mapper architecture. |
| "Sunrise options do not accept an external ROM file" | ROM file passed with a Sunrise/Carnivore2 option | Remove the ROM file argument when using `-s1`/`-m1`/`-s2`/`-m2`/`-c1`/`-c2`. |
| USB pendrive not detected with `-s2`/`-m2` | VBUS not connected or no OTG adapter | Ensure the USB-C port has VBUS power (use an OTG adapter that supplies VBUS). |
| microSD card not detected with `-s1`/`-m1` | Card not inserted or not FAT-formatted | Insert a FAT16/FAT32-formatted microSD card before powering on. |
| `SROM.COM /D15` does not list the cartridge with `-c1`/`-c2` | Wrong firmware mode or outdated build | Reflash with the correct `-c1` or `-c2` UF2 and use a build that includes Carnivore2 RAM-mode emulation. |
| `SROM.COM /D15` upload succeeds but the launched ROM fails | ROM/runtime compatibility issue in the emulation path | Try the latest firmware build and retest; this mode depends on Carnivore2-compatible bank/window presentation from PSRAM. |
| "Disk driver not found. System halted." with `-m1`/`-m2` | Firmware issue | Rebuild with latest firmware; check that the PIO bus init guard is present. |
| "Warning: -scc flag ignored" | ROM is not Konami SCC or Manbow2 mapper, or the selected Nextor mode is not `-c1`/`-c2` | Use a Konami SCC or Manbow2 ROM, or pair `-scc` with `-c1`/`-c2` for SROM-loaded Konami SCC titles. |
| "Warning: -sccplus flag ignored" | ROM is not Konami SCC or Manbow2 mapper, or the selected Nextor mode is not `-c1`/`-c2` | Use a compatible ROM, or pair `-sccplus` with `-c1`/`-c2` for SROM-loaded SCC+ titles. |
| "Error: -scc and -sccplus are mutually exclusive" | Both options were passed together | Use only one of the two options. |
| UF2 not recognized | Not in BOOTSEL, or wrong file | Enter BOOTSEL and copy the UF2 again. |
| Name truncated in menu | Filename too long | Shorten the filename. |

---

## Known limitations

- Only one ROM per UF2 (use the MultiROM or Explorer tools for multiple titles).
- The `-s1` and `-m1` options require a FAT-formatted microSD card in the cartridge's microSD slot.
- The `-s2` and `-m2` options require a USB flash drive connected to the cartridge's USB-C port (via OTG adapter) for disk access.
- The `-m1` and `-m2` options provide 1MB mapper RAM (64 × 16KB pages) backed by external PSRAM on GPIO47 / QMI CS1.
- The `-c1` and `-c2` options reuse that same 1MB PSRAM-backed mapper RAM and expose it through Carnivore2-compatible RAM-mode behavior intended for `SROM.COM /D15`.
- The `-c1` and `-c2` modes are loader modes, not direct ROM-embedding modes. They boot Nextor first; the ROM is uploaded later from DOS into PSRAM.
- SCC/SCC+ audio for `SROM.COM /D15` uploads is enabled by building the loader UF2 with `-c1/-c2` plus `-scc` or `-sccplus` before flashing the cartridge.
- Linux/macOS binaries are not provided (use Windows or build from source).
- The tool does not verify ROM integrity beyond size and mapper heuristics.
- SCC/SCC+ flags are applied for Konami SCC mapper (type 3) and Manbow2 mapper (type 14) ROMs; otherwise they are ignored with a warning.
- SCC/SCC+ emulation is available in the current RP2350 LoadROM package.
- Excessive flashing can wear out flash memory.

---

## Future improvements

- Cross-platform builds (Linux/macOS).
- Optional ROM integrity checks.
- GUI wrapper for mapper forcing.
- Additional mapper heuristics.

Author: Cristiano Almeida Goncalves  
Last updated: 03/29/2026
