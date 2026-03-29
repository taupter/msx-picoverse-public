# MSX PicoVerse 2040 Features

The MSX PicoVerse 2040 is a Raspberry Pi Pico-based MSX-compatible cartridge that offers a variety of features to enhance the MSX experience. Below is a list of its key features:

- **Multi-ROM Support**: The cartridge can store and run multiple MSX ROMs, allowing users to run different games and applications from a single cartridge.
- **Built-in Menu System**: A user-friendly menu system allows users to select and launch ROMs easily.
- **High Compatibility**: Supports a wide range of MSX ROMs, including games and applications for MSX1, MSX2, and MSX2+ systems.
- **Nextor DOS Support**: Compatible with Nextor DOS, enabling advanced file management and storage options.
- **Nextor Sunrise Modes**: `-s` enables Sunrise IDE Nextor, and `-m` enables Sunrise IDE Nextor plus an additional 192KB memory mapper.
- **Long Name Support**: Supports ROM names up to 50 characters, making it easier to identify games and applications.
- **Support for Various Mappers**: Includes support for multiple ROM mappers, enhancing compatibility with different types of MSX software. Mapper tags supported include: PLA-16, PLA-32, KonSCC, PLN-48, PLN-64, ASC-08, ASC-16, ASC-16X, Konami, NEO-8, NEO-16, and MANBW2.
- **Space Manbow 2 Support**: Dedicated mapper (MANBW2) for Space Manbow 2 ROMs, emulating the AMD AM29F040B flash chip used by the game for save data. Auto-detected for 512 KB Manbow 2 ROMs; save data is volatile (SRAM-backed).
- **openMSX ROM Database**: The LoadROM and MultiROM tools include an embedded SHA-1 database derived from openMSX's `softwaredb.xml`, improving mapper auto-detection accuracy for titles that cannot be reliably identified by heuristic scanning alone.
- **Support for up to 128 ROMs**: Can store and manage up to 128 different ROMs on a single cartridge.
- **Easy ROM Management**: Users can easily add, remove, and organize ROMs using a simple tool on their Windows PC.
- **Fast Loading Times**: Utilizes the capabilities of the Raspberry Pi Pico to ensure quick loading times for games and applications.
- **Firmware Updates**: The cartridge firmware can be updated via USB, allowing users to benefit from new features and improvements over time.
- **USB Mass Storage Support for Nextor**: Allows the use of the USB-C port as a mass storage device when running Nextor DOS.
- **USB Keyboard Support**: Dedicated firmware mode that turns the cartridge into a USB-to-MSX keyboard interface. Plug a standard USB keyboard into the USB-C port and it functions as the native MSX keyboard. Supports USB hubs, full matrix coverage (rows 0–10), and up to 6 simultaneous keys. Generated via `loadrom.exe -k`. Not compatible with FPGA-based MSX systems or MSX computers with an integrated PPI (e.g. T9769 MSX-ENGINE).
- **MSX-MIDI Support**: Dedicated firmware mode that turns the cartridge into a standard MSX-MIDI interface. Plug a USB-MIDI cable (e.g., connected to a Roland SoundCanvas) into the USB-C port and MSX MIDI software can send and receive MIDI data through the standard I/O ports (`0xE8`–`0xE9`). Supports any USB MIDI device. Generated via `loadrom.exe -i`. Tested with MIDRY (`/I5` option).
- **MIDI-PAC Support**: Dedicated firmware mode that turns the cartridge into a passive PSG-to-MIDI converter. Plug a USB MIDI device or module into the USB-C port and the firmware listens to live AY-3-8910 or YM2149 PSG writes, translating MSX music and effects into a General MIDI oriented stream. Generated via `loadrom.exe -p`.
- **USB Joystick Support**: Dedicated firmware mode that turns the cartridge into a USB-to-MSX joystick adapter. Plug a standard USB gamepad into the USB-C port and it appears as one or two native MSX joysticks to the host system, with no MSX-side software or driver required. Supports generic USB HID gamepads, Xbox 360 / XInput controllers, and Xbox One / Series X|S controllers (including via USB hubs). Uses open-drain bus driving to coexist with the real PSG chip — sound output is unaffected. Generated via `loadrom.exe -j`.

Author: Cristiano Goncalves  
Last updated: 03/29/2026