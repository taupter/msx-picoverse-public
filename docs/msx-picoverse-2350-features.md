# MSX PicoVerse 2350 Features

The MSX PicoVerse 2350 is a Raspberry Pi Pico 2350-based MSX-compatible cartridge that offers a variety of features to enhance the MSX experience. Below is a list of its key features:

- **Multi-ROM Support**: The cartridge can store and run multiple MSX ROMs, allowing users to run different games and applications from a single cartridge.
- **Built-in Menu System**: A user-friendly menu system allows users to select and launch ROMs easily.
- **Explorer Firmware**: Unified menu that merges ROMs from flash and microSD in a single list.
- **Source Labels (FL/SD)**: Each ROM entry shows whether it comes from flash or microSD.
- **High Compatibility**: Supports a wide range of MSX ROMs, including games and applications for MSX1, MSX2, and MSX2+ systems.
- **Nextor DOS Support**: Compatible with Nextor DOS, enabling advanced file management and storage options. Currently supports Nextor OS 2.1.4 on the SD card. You can run DSK files using Nextor.
- **Long Name Support**: Supports ROM names up to 60 characters, making it easier to identify games and applications.
- **Support for Various Mappers**: Includes support for multiple ROM mappers, enhancing compatibility with different types of MSX software. Mapper tags supported include: PLA-16, PLA-32, KonSCC, PLN-48, PLN-64, ASC-08, ASC-16, ASC-16X, Konami, NEO-8, NEO-16, and MANBW2.
- **Space Manbow 2 Support**: Dedicated mapper (MANBW2) for Space Manbow 2 ROMs, emulating the AMD AM29F040B flash chip used by the game for save data. Auto-detected for 512 KB Manbow 2 ROMs; save data is volatile (SRAM-backed).
- **openMSX ROM Database**: The LoadROM and MultiROM tools include an embedded SHA-1 database derived from openMSX's `softwaredb.xml`, improving mapper auto-detection accuracy for titles that cannot be reliably identified by heuristic scanning alone.
- **Sunrise IDE Emulation (USB)**: The LoadROM and MultiROM tools support Sunrise IDE emulation via the `-s2` option, embedding the Nextor 2.1.4 Sunrise IDE kernel and bridging USB mass storage (flash drives, USB-to-SD adapters) through the cartridge's USB-C port. The MSX sees a standard Sunrise IDE hard disk. In MultiROM, the Sunrise options can be combined to add multiple SYSTEM entries to the menu.
- **Sunrise IDE Emulation (microSD)**: The `-s1` option provides the same Sunrise IDE emulation but uses the on-board microSD card slot instead of USB. The microSD card is accessed via SPI at 31.25 MHz. Real card identification (OEM, product name, revision) is read from the SD CID register and shown during Nextor boot.
- **Sunrise IDE + 1MB PSRAM Memory Mapper (USB)**: The `-m2` option combines Sunrise IDE emulation via USB with a 1MB memory mapper (64 × 16KB pages) in an expanded sub-slot architecture. The mapper RAM is backed by external PSRAM on GPIO47 / QMI CS1. Sub-slot 0 serves the Nextor ROM, sub-slot 1 provides the mapper RAM across all four 16KB pages, and mapper page registers (I/O ports FC–FF) are intercepted via PIO1.
- **Sunrise IDE + 1MB PSRAM Memory Mapper (microSD)**: The `-m1` option provides the same mapper architecture as `-m2` but uses the on-board microSD card for storage instead of USB.
- **ROM Loading to PSRAM Mapper via `SROM.COM` (USB)**: The `-c2` option exposes the cartridge as a Carnivore2-compatible RAM target on top of the 1MB PSRAM mapper, allowing `SROM.COM /D15` to upload ROM files into PSRAM and execute them from RAM while using USB mass storage as the Nextor backend.
- **ROM Loading to PSRAM Mapper via `SROM.COM` (microSD)**: The `-c1` option provides the same Carnivore2-compatible PSRAM loading workflow as `-c2`, but uses the on-board microSD card as the Nextor backend.
- **Support for up to 1024 entries (Explorer)**: Per folder view, including ROMs and MP3s; the root view can also include flash entries.
- **Paged ROM List**: The menu loads pages on demand to keep the ROM list responsive with large libraries.
- **Search by Name (Explorer)**: Press `/`, type a query, and press Enter to jump to the first match.
- **On-device 40/80 Column Toggle**: Press `C` to switch between compact 40-column and wide 80-column menu layouts (when supported by the MSX model).
- **MP3 Player**: MP3 files on microSD appear in the menu and open a player screen with play/stop, mute, and visualizer controls.
- **ROM Detail Screen**: ROM entries open a details screen with mapper detection/override and audio profile selection before running.
- **SYSTEM ROM Priority**: System ROM entries remain at the top of the list, separate from name sorting.
- **microSD ROM Size Limit**: microSD ROM files are limited to 256 KB each (temporary limitation for prototype versions).
- **Easy ROM Management**: Users can easily add, remove, and organize ROMs using a simple tool on their PC.
- **Fast Loading Times**: Utilizes the high-speed capabilities of the Raspberry Pi Pico to ensure quick loading times for games and applications.
- **Firmware Updates**: The cartridge firmware can be updated via USB, allowing users to benefit from new features and improvements over time.
- **Compact Design**: The cartridge is designed to fit seamlessly into MSX systems without adding bulk.
- **SD Card Slot**: Equipped with an SD card slot for easy storage and transfer of ROMs and files.
- **Audio Profile Selection**: Allows users to select different audio profiles for enhanced sound compatibility with various MSX models. 
- **SCC/SCC+ Emulation**: When enabled for Konami SCC or Manbow2 mapper ROMs, the cartridge can emulate the SCC and SCC+ sound chips in hardware, providing accurate audio output through an I2S DAC connected to the RP2350. This allows games that use SCC or SCC+ sound to have their full soundtrack without requiring an original SCC cartridge. For details on supported registers and behavior, see the [SCC/SCC+ documentation](docs/msx-picoverse-2350-scc.md).