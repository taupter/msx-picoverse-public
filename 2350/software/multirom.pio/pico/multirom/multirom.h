// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// multirom.h - This is the Raspberry Pico firmware that will be used to load ROMs into the MSX
//
// This firmware is responsible for loading the multirom menu and the ROMs selected by the user into the MSX. When flashed through the 
// multirom tool, it will be stored on the pico flash memory followed by the MSX MENU ROM (with the config) and all the ROMs processed by the 
// multirom tool. The sofware in this firmware will load the first 32KB ROM that contains the menu into the MSX and it will allow the user
// to select a ROM to be loaded into the MSX. The selected ROM will be loaded into the MSX and the MSX will be reseted to run the selected ROM.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MULTIROM_H
#define MULTIROM_H

#define ROM_NAME_MAX    50   // Maximum size of the ROM name
#define ROM_RECORD_SIZE (ROM_NAME_MAX + 1 + (sizeof(uint32_t) * 2)) // Name + mapper + size + offset
#define CACHE_SIZE      262144     // 256KB cache size for ROM data
#define MAPPER_SIZE     1048576    // 1 MB memory mapper RAM in external PSRAM
#define MAPPER_PAGES    64         // 1 MB / 16 KB = 64 pages
#define MAPPER_PAGE_SIZE 16384     // 16 KB per mapper page
#define PSRAM_BASE_ADDR 0x11000000u // Cached CS1 QMI window for external PSRAM (write-through)
#define PSRAM_TOTAL_SIZE 0x800000u  // 8 MB total external PSRAM

// PSRAM region descriptor returned by the memory manager.
typedef struct {
    uint32_t offset;    // Offset from PSRAM base
    uint32_t size;      // Size of this region in bytes
    uint8_t *ptr;       // Memory-mapped pointer (PSRAM_BASE_ADDR + offset)
} psram_region_t;

#define PIN_A0     0 
#define PIN_A1     1
#define PIN_A2     2
#define PIN_A3     3
#define PIN_A4     4
#define PIN_A5     5
#define PIN_A6     6
#define PIN_A7     7
#define PIN_A8     8
#define PIN_A9     9
#define PIN_A10    10
#define PIN_A11    11
#define PIN_A12    12
#define PIN_A13    13
#define PIN_A14    14
#define PIN_A15    15
#define ADDR_PINS   0    // Address bus (A0-A15)

// Data lines (D0-D7)
#define PIN_D0     16
#define PIN_D1     17
#define PIN_D2     18
#define PIN_D3     19
#define PIN_D4     20
#define PIN_D5     21
#define PIN_D6     22
#define PIN_D7     23
#define DATA_PINS   16   // Data bus (D0-D7)

// Control signals
#define PIN_RD     24   // Read strobe from MSX
#define PIN_WR     25   // Write strobe from MSX
#define PIN_IORQ   26   // IO Request line from MSX
#define PIN_SLTSL  27   // Slot Select for this cartridge slot
#define PIN_WAIT    28  // WAIT line to MSX 
#define PIN_BUSSDIR 37  // Bus direction line 
#define PIN_PSRAM   47  // PSRAM select line

// I2S DAC pins
#define I2S_DATA_PIN  29   // I2S serial data
#define I2S_BCLK_PIN  30   // I2S bit clock
#define I2S_WSEL_PIN  31   // I2S word select (LRCLK)
#define I2S_MUTE_PIN  32   // I2S DAC mute control

// SCC emulation constants
#define SCC_SAMPLE_RATE 44100
#define SCC_CLOCK       3579545
#define SCC_FLAG        0x80u   // Bit flag in mapper byte for SCC emulation
#define SCC_PLUS_FLAG   0x40u   // Bit flag in mapper byte for SCC+ (enhanced) emulation

// Sunrise WiFi (ESP-01 over hardware UART1) constants
#define WIFI_FLAG       0x20u   // Bit flag in mapper byte for Sunrise WiFi system ROM + memio UART
#define WIFI_ROM_SIZE   16384u  // ESP8266P system ROM size appended after the Sunrise ROM payload
#define PIN_ESP_UART_TX 38
#define PIN_ESP_UART_RX 39

static inline void setup_gpio();
unsigned long __no_inline_not_in_flash_func(read_ulong)(const unsigned char *ptr);
int isEndOfData(const unsigned char *memory);

int __no_inline_not_in_flash_func(loadrom_msx_menu)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_konamiscc)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_konamiscc_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type);
void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_neo8)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_neo16)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_sunrise)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii16x)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_planar64)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_manbow2)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_manbow2_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type);
void __no_inline_not_in_flash_func(loadrom_sunrise_sd)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_sd)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_wifi)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_wifi_sd)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_wifi)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_wifi_sd)(uint32_t offset, bool cache_enable);

#endif