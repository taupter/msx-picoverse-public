// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// explorer.c - PicoVerse 2350 Explorer firmware (ROM loader + SD support)
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "ff.h"
#include "diskio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hw_config.h"
#include "explorer.h"
#include "nextor.h"
#include "mapper_detect.h"
#include "mp3.h"
#include "emu2212.h"
#include "msx_bus.pio.h"
#include "pico/audio_i2s.h"

// config area and buffer for the ROM data
#define ROM_NAME_MAX    60          // Maximum size of the ROM name
#define MAX_ROM_RECORDS 1024        // Maximum ROM files supported (flash + SD)
#define MAX_FLASH_RECORDS 128       // Maximum ROM files stored in flash
#define ROM_RECORD_SIZE (ROM_NAME_MAX + 1 + (sizeof(uint32_t) * 2)) // Name + mapper + size + offset
#define MENU_ROM_SIZE   (32u * 1024u) // Full menu ROM size stored before config records
#define MONITOR_ADDR    (0xBB01)    // Monitor ROM address within image
#define CACHE_SIZE      (256u * 1024u)     // 256KB cache size for ROM data (also SD ROM upper limit)

#define SOURCE_SD_FLAG  0x80 // Flag in the mapper byte indicating the ROM is on SD
#define FOLDER_FLAG     0x40 // Flag in the mapper byte indicating the record is a folder
#define MP3_FLAG        0x20 // Flag in the mapper byte indicating the record is an MP3 file
#define OVERRIDE_FLAG   0x10 // Flag in the mapper byte indicating manual override

// Temporarily disable the in-menu MP3 player. The MP3 stack is being
// reworked into a dedicated MSX ROM (mp3player.rom) so the menu no
// longer needs to coexist with Core 1 / I2S / SD streaming. While
// that is not in place, MP3 files are skipped during SD enumeration
// and Core 1 is never launched, leaving Explorer free to focus on
// flash and SD ROM execution.
#define EXPLORER_MP3_DISABLED 1

#define FILES_PER_PAGE  19 // Maximum files per page on the menu
#define CTRL_BASE_ADDR  0xBFF0 // Control registers base address
#define CTRL_COUNT_L    (CTRL_BASE_ADDR + 0) // Control: total record count low byte
#define CTRL_COUNT_H    (CTRL_BASE_ADDR + 1) // Control: total record count high byte
#define CTRL_PAGE       (CTRL_BASE_ADDR + 2) // Control: current page index
#define CTRL_STATUS     (CTRL_BASE_ADDR + 3) // Control: status register
#define CTRL_CMD        (CTRL_BASE_ADDR + 4) // Control: command register
#define CTRL_MATCH_L    (CTRL_BASE_ADDR + 5) // Control: match index low byte
#define CTRL_MATCH_H    (CTRL_BASE_ADDR + 6) // Control: match index high byte
#define CTRL_MAPPER     (CTRL_BASE_ADDR + 7) // Control: mapper value
#define CTRL_ACK        (CTRL_BASE_ADDR + 8) // Control: command ack
#define CTRL_MAGIC      0xA5 // Control: magic value to indicate valid command
#define CTRL_QUERY_BASE 0xBFC0 // Control: query string base address
#define CTRL_QUERY_SIZE 32 // Control: query string size
#define CMD_APPLY_FILTER 0x01 // Command: apply filter
#define CMD_FIND_FIRST   0x02 // Command: find first match
#define CMD_ENTER_DIR    0x03 // Command: enter directory
#define CMD_DETECT_MAPPER 0x04 // Command: detect mapper on demand
#define CMD_SET_MAPPER    0x05 // Command: set mapper override

#define CTRL_AUDIO      (CTRL_BASE_ADDR + 9) // Control: audio selection (0=None, 1=SCC, 2=SCC+)

#define DATA_BASE_ADDR   0xA000 // Data buffer base address
#define DATA_BUFFER_SIZE (CTRL_QUERY_BASE - DATA_BASE_ADDR) // Data buffer size
#define DATA_MAGIC_0     'P' // Data header magic bytes
#define DATA_MAGIC_1     'V' // Data header magic bytes
#define DATA_MAGIC_2     'E' // Data header magic bytes
#define DATA_MAGIC_3     'X' // Data header magic bytes
#define DATA_VERSION     1 // Data header version
#define DATA_HEADER_SIZE 24 // Data header size in bytes
#define DATA_RECORD_TABLE_ENTRY_SIZE 4 // Size of each record table entry
#define DATA_RECORD_PAYLOAD_SIZE 13 // Minimum size of each record payload
#define TLV_NAME_OFFSET  0x01 // Type-Length-Value: Name offset
#define TLV_MAPPER       0x02 // Type-Length-Value: Mapper
#define TLV_SIZE         0x03 // Type-Length-Value: Size


// MP3 control registers
#define MP3_CTRL_BASE      0xBFE0 // MP3 control registers base address
#define MP3_CTRL_CMD       (MP3_CTRL_BASE + 0)
#define MP3_CTRL_STATUS    (MP3_CTRL_BASE + 1)
#define MP3_CTRL_INDEX_L   (MP3_CTRL_BASE + 2)
#define MP3_CTRL_INDEX_H   (MP3_CTRL_BASE + 3)
#define MP3_CTRL_ELAPSED_L (MP3_CTRL_BASE + 4)
#define MP3_CTRL_ELAPSED_H (MP3_CTRL_BASE + 5)
#define MP3_CTRL_TOTAL_L   (MP3_CTRL_BASE + 6)
#define MP3_CTRL_TOTAL_H   (MP3_CTRL_BASE + 7)
#define MP3_CTRL_MODE      (MP3_CTRL_BASE + 8)

// "Now Playing" buffer: 60-byte name (space-padded) + 4-byte size (LE)
#define MP3_NOW_PLAYING_BASE 0xBFA0
#define MP3_NOW_PLAYING_SIZE 64

// MSX protocol command code for MP3 file selection (local to explorer.c)
#define MP3_CMD_SELECT      0x01

typedef struct {
    PIO pio;
    uint sm_read;
    uint sm_write;
    uint offset_read;
    uint offset_write;
} msx_pio_bus_t;

static msx_pio_bus_t msx_bus;
static bool msx_bus_programs_loaded = false;

typedef struct {
    uint8_t rom_index;
    bool rom_selected;
} menu_select_ctx_t;

typedef struct {
    uint8_t *bank_regs;
} bank8_ctx_t;

typedef struct {
    uint16_t *bank_regs;
} bank16_ctx_t;

// This symbol marks the end of the main program in flash.
// Custom data starts right after it
extern const uint8_t __flash_binary_end[];

// SRAM buffer to cache ROM data
static uint8_t rom_sram[CACHE_SIZE];
static uint32_t active_rom_size = 0;
static uint32_t rom_cached_size = 0; // Added for PIO implementation
// Effective cache capacity used by prepare_rom_source(). Some mappers (e.g.
// Manbow2) reduce this so the tail of rom_sram can be repurposed (writable
// flash sector emulation), matching multirom's behaviour.
static uint32_t rom_cache_capacity = CACHE_SIZE;
static uint8_t page_buffer[DATA_BUFFER_SIZE];

// pointer to the custom data
static const uint8_t *flash_rom = (const uint8_t *)&__flash_binary_end;
static const uint8_t *rom_data = (const uint8_t *)&__flash_binary_end;
static bool rom_data_in_ram = false;

// External PSRAM region used to buffer SD-loaded ROMs. Defined later
// alongside the PSRAM bring-up code; declared here for use by earlier
// functions such as load_rom_from_sd().
static psram_region_t sd_rom_region;
static bool psram_bring_up_once(void);

uint8_t ctr_val = 0xFF;    
BYTE const pdrv = 0;  // Physical drive number
DSTATUS ds = 1; // Disk status (1 = not initialized)

// Structure to represent a ROM record
// The ROM record will contain the name of the ROM, the mapper code, the size of the ROM and the offset in the flash memory
// Name: ROM_NAME_MAX bytes
// Mapper: 1 byte
// Size: 4 bytes
// Offset: 4 bytes
typedef struct {
    char Name[ROM_NAME_MAX];
    unsigned char Mapper;
    unsigned long Size;
    unsigned long Offset;
} ROMRecord;

ROMRecord records[MAX_ROM_RECORDS]; // Array to store the ROM records

#define SD_PATH_MAX        96
#define SD_PATH_BUFFER_SIZE 19000
#define MIN_ROM_SIZE       8192
#define MAX_ROM_SIZE       (15u * 1024u * 1024u)
#define SD_ROM_MAX_SIZE    (2u * 1024u * 1024u) // PSRAM region capacity for SD-loaded ROMs
#define MAPPER_SYSTEM      10

static const char *MAPPER_DESCRIPTIONS[] = {
    "PLA-16", "PLA-32", "KonSCC", "PLN-48", "ASC-08",
    "ASC-16", "Konami", "NEO-8", "NEO-16", "SYSTEM",
    "SYSTEM", "ASC16X", "PLN-64", "MANBW2"
};

#define MAPPER_DESCRIPTION_COUNT (sizeof(MAPPER_DESCRIPTIONS) / sizeof(MAPPER_DESCRIPTIONS[0]))

static char sd_path_buffer[SD_PATH_BUFFER_SIZE];
static uint16_t sd_path_offsets[MAX_ROM_RECORDS];
static uint16_t sd_path_buffer_used = 0;
static uint16_t sd_record_count = 0;
static bool sd_mounted = false;
static sd_card_t *sd_card = NULL;
static uint16_t total_record_count = 0;
static uint16_t full_record_count = 0;
static uint8_t current_page = 0;
static uint16_t filtered_indices[MAX_ROM_RECORDS];
static char filter_query[CTRL_QUERY_SIZE];
static volatile uint8_t ctrl_cmd_state = 0;
static volatile uint8_t ctrl_mapper_value = 0;
static volatile uint8_t ctrl_ack_value = 0;
static uint16_t match_index = 0xFFFF;
static ROMRecord flash_records[MAX_FLASH_RECORDS];
static uint16_t flash_record_count = 0;
static char sd_current_path[SD_PATH_MAX] = "/";
static volatile bool refresh_requested = false;
static volatile bool refresh_in_progress = false;
static bool refresh_worker_started = false;
static volatile bool detect_mapper_pending = false;

// Chunked directory scan state machine (yields back to MP3 loop between chunks)
#define REFRESH_CHUNK_SIZE 8
typedef enum {
    REFRESH_IDLE = 0,
    REFRESH_INIT,
    REFRESH_SCAN_FOLDERS,
    REFRESH_SORT_FOLDERS,
    REFRESH_SCAN_FILES,
    REFRESH_FINALIZE
} refresh_state_t;
static refresh_state_t refresh_state = REFRESH_IDLE;
static DIR refresh_dir;
static uint16_t refresh_record_index = 0;
static uint16_t refresh_folder_count = 0;
static bool refresh_has_parent = false;
static volatile uint16_t detect_mapper_index = 0;
static volatile uint16_t mp3_selected_index = 0;
static volatile bool mp3_pending_select = false;
static volatile uint16_t mp3_pending_index = 0;
static volatile uint8_t mp3_pending_cmd = 0;
static volatile uint8_t mp3_play_mode = MP3_PLAY_MODE_SINGLE;
static uint16_t mp3_playing_filtered_index = 0xFFFF;
static uint8_t mp3_now_playing_buf[MP3_NOW_PLAYING_SIZE];
static volatile uint8_t ctrl_audio_selection = 0; // 0=None, 1=SCC, 2=SCC+

// SCC emulation state + I2S audio
#define SCC_VOLUME_SHIFT 2  // Left-shift SCC output for volume boost (4x)
#define SCC_AUDIO_BUFFER_SAMPLES 256
static SCC scc_instance;
static struct audio_buffer_pool *scc_audio_pool;

static const char *EXCLUDED_SD_FOLDERS[] = {
    "System Volume Information"
};
static const size_t EXCLUDED_SD_FOLDER_COUNT = sizeof(EXCLUDED_SD_FOLDERS) / sizeof(EXCLUDED_SD_FOLDERS[0]);

static void write_u32_le(uint8_t *ptr, uint32_t value);
static void write_u16_le(uint8_t *ptr, uint16_t value);
static void build_page_buffer(uint8_t page_index);
static void refresh_records_for_current_path(void);

static int compare_record_names(const ROMRecord *a, const ROMRecord *b) {
    return strncmp(a->Name, b->Name, ROM_NAME_MAX);
}

static bool is_system_record(const ROMRecord *record) {
    return ((record->Mapper & ~(SOURCE_SD_FLAG | OVERRIDE_FLAG)) == MAPPER_SYSTEM);
}

static bool is_folder_record(const ROMRecord *record) {
    return (record->Mapper & FOLDER_FLAG) != 0;
}

static size_t trim_name_copy(char *dest, const char *src) {
    size_t len = 0;
    for (size_t i = 0; i < ROM_NAME_MAX; i++) {
        dest[i] = src[i];
    }
    dest[ROM_NAME_MAX] = '\0';
    len = ROM_NAME_MAX;
    while (len > 0 && dest[len - 1] == ' ') {
        dest[len - 1] = '\0';
        len--;
    }
    return len;
}

static bool contains_ignore_case(const char *text, const char *query) {
    if (!query || query[0] == '\0') {
        return true;
    }
    size_t text_len = strlen(text);
    size_t query_len = strlen(query);
    if (query_len == 0 || query_len > text_len) {
        return false;
    }
    for (size_t i = 0; i + query_len <= text_len; i++) {
        size_t j = 0;
        while (j < query_len) {
            char a = text[i + j];
            char b = query[j];
            if (toupper((unsigned char)a) != toupper((unsigned char)b)) {
                break;
            }
            j++;
        }
        if (j == query_len) {
            return true;
        }
    }
    return false;
}

static void apply_filter(void) {
    total_record_count = 0;
    if (filter_query[0] == '\0') {
        for (uint16_t i = 0; i < full_record_count; i++) {
            filtered_indices[total_record_count++] = i;
        }
        return;
    }

    char name_buf[ROM_NAME_MAX + 1];
    for (uint16_t i = 0; i < full_record_count; i++) {
        trim_name_copy(name_buf, records[i].Name);
        if (contains_ignore_case(name_buf, filter_query)) {
            filtered_indices[total_record_count++] = i;
        }
    }
}

static void find_first_match(void) {
    match_index = 0xFFFF;
    if (filter_query[0] == '\0') {
        return;
    }

    char name_buf[ROM_NAME_MAX + 1];
    for (uint16_t i = 0; i < full_record_count; i++) {
        trim_name_copy(name_buf, records[i].Name);
        if (contains_ignore_case(name_buf, filter_query)) {
            match_index = i;
            return;
        }
    }
}

static void swap_records(uint16_t a, uint16_t b) {
    if (a == b) {
        return;
    }
    ROMRecord temp = records[a];
    records[a] = records[b];
    records[b] = temp;
    uint16_t path_temp = sd_path_offsets[a];
    sd_path_offsets[a] = sd_path_offsets[b];
    sd_path_offsets[b] = path_temp;
}

static void sort_records_range(uint16_t start, uint16_t count) {
    if (count < 2) {
        return;
    }
    uint16_t end = (uint16_t)(start + count);
    for (uint16_t i = start; i + 1 < end; i++) {
        for (uint16_t j = i + 1; j < end; j++) {
            if (compare_record_names(&records[i], &records[j]) > 0) {
                swap_records(i, j);
            }
        }
    }
}

static void write_config_area(uint8_t *config_area, const ROMRecord *list, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        uint8_t *entry = config_area + (i * ROM_RECORD_SIZE);
        memset(entry, 0xFF, ROM_RECORD_SIZE);
        memset(entry, ' ', ROM_NAME_MAX);
        size_t name_len = strlen(list[i].Name);
        if (name_len > ROM_NAME_MAX) {
            name_len = ROM_NAME_MAX;
        }
        memcpy(entry, list[i].Name, name_len);
        entry[ROM_NAME_MAX] = list[i].Mapper;
        write_u32_le(entry + ROM_NAME_MAX + 1, (uint32_t)list[i].Size);
        write_u32_le(entry + ROM_NAME_MAX + 1 + sizeof(uint32_t), (uint32_t)list[i].Offset);
    }

    if (count < MAX_ROM_RECORDS) {
        memset(config_area + (count * ROM_RECORD_SIZE), 0xFF,
               (MAX_ROM_RECORDS - count) * ROM_RECORD_SIZE);
    }
}

static void build_page_buffer(uint8_t page_index) {
    uint8_t *buffer = page_buffer;
    const uint16_t buffer_size = DATA_BUFFER_SIZE;
    uint16_t start_index = (uint16_t)page_index * FILES_PER_PAGE;
    uint16_t record_count = 0;

    if (start_index < total_record_count) {
        uint16_t remaining = (uint16_t)(total_record_count - start_index);
        record_count = (remaining > FILES_PER_PAGE) ? FILES_PER_PAGE : remaining;
    }

    memset(buffer, 0xFF, buffer_size);

    uint16_t table_offset = DATA_HEADER_SIZE;
    uint16_t table_size = (uint16_t)(record_count * DATA_RECORD_TABLE_ENTRY_SIZE);
    uint16_t payload_size = (uint16_t)(record_count * DATA_RECORD_PAYLOAD_SIZE);
    uint16_t string_pool_offset = (uint16_t)(table_offset + table_size);
    uint16_t max_string_pool = 0;
    if ((uint32_t)string_pool_offset + payload_size <= buffer_size) {
        max_string_pool = (uint16_t)(buffer_size - (string_pool_offset + payload_size));
    }

    uint16_t name_offsets[FILES_PER_PAGE];
    uint16_t string_cursor = 0;
    for (uint16_t i = 0; i < record_count; i++) {
        uint16_t filtered_index = (uint16_t)(start_index + i);
        uint16_t record_index = filtered_indices[filtered_index];
        char name_buf[ROM_NAME_MAX + 1];
        size_t name_len = trim_name_copy(name_buf, records[record_index].Name);
        if (name_len + 1 <= (size_t)(max_string_pool - string_cursor)) {
            name_offsets[i] = string_cursor;
            memcpy(buffer + string_pool_offset + string_cursor, name_buf, name_len);
            buffer[string_pool_offset + string_cursor + name_len] = '\0';
            string_cursor = (uint16_t)(string_cursor + name_len + 1);
        } else {
            name_offsets[i] = 0xFFFF;
        }
    }

    uint16_t string_pool_size = string_cursor;
    uint16_t payload_offset = (uint16_t)(string_pool_offset + string_pool_size);
    uint16_t payload_cursor = 0;

    for (uint16_t i = 0; i < record_count; i++) {
        uint16_t filtered_index = (uint16_t)(start_index + i);
        uint16_t record_index = filtered_indices[filtered_index];
        uint8_t *entry = buffer + table_offset + (i * DATA_RECORD_TABLE_ENTRY_SIZE);
        uint16_t record_offset = (uint16_t)(payload_offset + payload_cursor);
        uint16_t record_length = DATA_RECORD_PAYLOAD_SIZE;

        write_u16_le(entry, record_offset);
        write_u16_le(entry + 2, record_length);

        uint8_t *rec = buffer + record_offset;
        rec[0] = TLV_NAME_OFFSET;
        rec[1] = 2;
        write_u16_le(rec + 2, name_offsets[i]);
        rec[4] = TLV_MAPPER;
        rec[5] = 1;
        rec[6] = records[record_index].Mapper;
        rec[7] = TLV_SIZE;
        rec[8] = 4;
        write_u32_le(rec + 9, (uint32_t)records[record_index].Size);

        payload_cursor = (uint16_t)(payload_cursor + record_length);
    }

    buffer[0] = DATA_MAGIC_0;
    buffer[1] = DATA_MAGIC_1;
    buffer[2] = DATA_MAGIC_2;
    buffer[3] = DATA_MAGIC_3;
    buffer[4] = DATA_VERSION;
    buffer[5] = 0;
    write_u16_le(buffer + 6, DATA_HEADER_SIZE);
    write_u16_le(buffer + 8, total_record_count);
    write_u16_le(buffer + 10, page_index);
    write_u16_le(buffer + 12, record_count);
    write_u16_le(buffer + 14, table_offset);
    write_u16_le(buffer + 16, table_size);
    write_u16_le(buffer + 18, string_pool_offset);
    write_u16_le(buffer + 20, string_pool_size);
    write_u16_le(buffer + 22, payload_offset);
}

static bool equals_ignore_case(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool is_excluded_folder(const char *name) {
    if (!name || name[0] == '\0') {
        return true;
    }
    if (equals_ignore_case(name, ".") || equals_ignore_case(name, "..")) {
        return true;
    }
    for (size_t i = 0; i < EXCLUDED_SD_FOLDER_COUNT; i++) {
        if (equals_ignore_case(name, EXCLUDED_SD_FOLDERS[i])) {
            return true;
        }
    }
    return false;
}

static bool is_root_path(const char *path) {
    if (!path || path[0] == '\0') {
        return true;
    }
    if (path[0] == '/' && path[1] == '\0') {
        return true;
    }
    return false;
}

static void set_root_path(void) {
    sd_current_path[0] = '/';
    sd_current_path[1] = '\0';
}

static void go_up_one_level(void) {
    if (is_root_path(sd_current_path)) {
        set_root_path();
        return;
    }

    size_t len = strlen(sd_current_path);
    if (len == 0) {
        set_root_path();
        return;
    }

    while (len > 1 && sd_current_path[len - 1] == '/') {
        sd_current_path[len - 1] = '\0';
        len--;
    }

    char *slash = strrchr(sd_current_path, '/');
    if (!slash || slash == sd_current_path) {
        set_root_path();
    } else {
        *slash = '\0';
    }
}

static void append_folder_to_path(const char *folder) {
    if (!folder || folder[0] == '\0') {
        return;
    }
    if (equals_ignore_case(folder, "..")) {
        go_up_one_level();
        return;
    }

    size_t base_len = strlen(sd_current_path);
    size_t folder_len = strlen(folder);
    if (base_len + folder_len + 2 >= sizeof(sd_current_path)) {
        return;
    }

    if (!is_root_path(sd_current_path)) {
        sd_current_path[base_len] = '/';
        base_len++;
    }
    memcpy(sd_current_path + base_len, folder, folder_len + 1);
}

static void write_u32_le(uint8_t *ptr, uint32_t value) {
    ptr[0] = (uint8_t)(value & 0xFFu);
    ptr[1] = (uint8_t)((value >> 8) & 0xFFu);
    ptr[2] = (uint8_t)((value >> 16) & 0xFFu);
    ptr[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void write_u16_le(uint8_t *ptr, uint16_t value) {
    ptr[0] = (uint8_t)(value & 0xFFu);
    ptr[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static const char *basename_from_path(const char *path) {
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

static uint8_t mapper_number_from_filename(const char *filename) {
    char name_copy[SD_PATH_MAX];
    strncpy(name_copy, filename, sizeof(name_copy));
    name_copy[sizeof(name_copy) - 1] = '\0';

    char *dot = strrchr(name_copy, '.');
    if (dot) {
        *dot = '\0';
    }

    size_t name_len = strlen(name_copy);
    for (size_t i = 0; i + 1 < MAPPER_DESCRIPTION_COUNT; ++i) {
        const char *tag = MAPPER_DESCRIPTIONS[i];
        size_t tag_len = strlen(tag);
        if (name_len > tag_len + 1 && name_copy[name_len - tag_len - 1] == '.') {
            if (equals_ignore_case(name_copy + name_len - tag_len, tag)) {
                return (uint8_t)(i + 1);
            }
        }
    }
    return 0;
}

static void build_display_name(const char *filename, char *out, size_t out_size) {
    char name_copy[SD_PATH_MAX];
    strncpy(name_copy, filename, sizeof(name_copy));
    name_copy[sizeof(name_copy) - 1] = '\0';

    char *dot = strrchr(name_copy, '.');
    if (dot) {
        *dot = '\0';
    }

    size_t name_len = strlen(name_copy);
    for (size_t i = 0; i + 1 < MAPPER_DESCRIPTION_COUNT; ++i) {
        const char *tag = MAPPER_DESCRIPTIONS[i];
        size_t tag_len = strlen(tag);
        if (name_len > tag_len + 1 && name_copy[name_len - tag_len - 1] == '.') {
            if (equals_ignore_case(name_copy + name_len - tag_len, tag)) {
                name_copy[name_len - tag_len - 1] = '\0';
                break;
            }
        }
    }

    strncpy(out, name_copy, out_size);
    out[out_size - 1] = '\0';
}

static bool has_rom_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    return equals_ignore_case(dot + 1, "ROM");
}

static bool has_mp3_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    return equals_ignore_case(dot + 1, "MP3");
}

// FatFS-backed reader callback for the shared streaming detector.
typedef struct {
    FIL *fil;
    uint32_t cur_offset; // current file pointer (best-effort cache to avoid redundant f_lseek)
    bool failed;
} fatfs_reader_ctx_t;

static uint32_t fatfs_reader_cb(void *user, uint32_t offset, void *buf, uint32_t len) {
    fatfs_reader_ctx_t *ctx = (fatfs_reader_ctx_t *)user;
    if (ctx->failed || !ctx->fil) {
        return 0;
    }
    if (offset != ctx->cur_offset) {
        if (f_lseek(ctx->fil, offset) != FR_OK) {
            ctx->failed = true;
            return 0;
        }
        ctx->cur_offset = offset;
    }
    UINT br = 0;
    if (f_read(ctx->fil, buf, (UINT)len, &br) != FR_OK) {
        ctx->failed = true;
        return 0;
    }
    ctx->cur_offset += br;
    return (uint32_t)br;
}

// Stream a ROM file from the SD card through the shared mapper detector.
// Uses the same SHA1 + heuristic algorithm as the PC tool so that the firmware
// produces identical mapper guesses for ROMs executed straight from microSD.
static uint8_t detect_rom_type_from_file(const char *path, uint32_t size) {
    if (size > MAX_ROM_SIZE || size < MIN_ROM_SIZE) {
        return 0;
    }

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        return 0;
    }

    fatfs_reader_ctx_t ctx = { .fil = &fil, .cur_offset = 0, .failed = false };
    uint8_t mapper = mapper_detect_stream(size, fatfs_reader_cb, &ctx);
    f_close(&fil);
    if (ctx.failed) {
        return 0;
    }
    return mapper;
}

static bool sd_mount_card(void) {
    if (sd_mounted) {
        return true;
    }

    if (!sd_init_driver()) {
        return false;
    }

    sd_card = sd_get_by_num(0);
    if (!sd_card) {
        return false;
    }

    FRESULT fr = f_mount(&sd_card->state.fatfs, "", 1);
    if (fr != FR_OK) {
        return false;
    }

    sd_mounted = true;
    return true;
}

static uint16_t add_folder_record(uint16_t record_index, const char *name) {
    if (record_index >= MAX_ROM_RECORDS) {
        return record_index;
    }
    memset(records[record_index].Name, 0, sizeof(records[record_index].Name));
    strncpy(records[record_index].Name, name, sizeof(records[record_index].Name) - 1);
    records[record_index].Mapper = (unsigned char)(FOLDER_FLAG | SOURCE_SD_FLAG);
    records[record_index].Size = 0;
    records[record_index].Offset = 0;
    sd_path_offsets[record_index] = 0xFFFF;
    return (uint16_t)(record_index + 1);
}

static uint16_t add_sd_rom_record(uint16_t record_index, const char *path, const char *filename, uint32_t size, uint8_t mapper) {
    if (record_index >= MAX_ROM_RECORDS) {
        return record_index;
    }
    size_t path_len = strlen(path);
    if (sd_path_buffer_used + path_len + 1 > SD_PATH_BUFFER_SIZE) {
        return record_index;
    }
    sd_path_offsets[record_index] = sd_path_buffer_used;
    memcpy(sd_path_buffer + sd_path_buffer_used, path, path_len + 1);
    sd_path_buffer_used = (uint16_t)(sd_path_buffer_used + path_len + 1);

    char display_name[ROM_NAME_MAX + 1];
    build_display_name(filename, display_name, sizeof(display_name));

    memset(records[record_index].Name, 0, sizeof(records[record_index].Name));
    strncpy(records[record_index].Name, display_name, sizeof(records[record_index].Name) - 1);
    records[record_index].Mapper = (unsigned char)(mapper | SOURCE_SD_FLAG);
    records[record_index].Size = (unsigned long)size;
    records[record_index].Offset = (unsigned long)record_index;
    return (uint16_t)(record_index + 1);
}

static uint16_t add_sd_mp3_record(uint16_t record_index, const char *path, const char *filename, uint32_t size) {
    if (record_index >= MAX_ROM_RECORDS) {
        return record_index;
    }
    size_t path_len = strlen(path);
    if (sd_path_buffer_used + path_len + 1 > SD_PATH_BUFFER_SIZE) {
        return record_index;
    }
    sd_path_offsets[record_index] = sd_path_buffer_used;
    memcpy(sd_path_buffer + sd_path_buffer_used, path, path_len + 1);
    sd_path_buffer_used = (uint16_t)(sd_path_buffer_used + path_len + 1);

    char display_name[ROM_NAME_MAX + 1];
    build_display_name(filename, display_name, sizeof(display_name));

    memset(records[record_index].Name, 0, sizeof(records[record_index].Name));
    strncpy(records[record_index].Name, display_name, sizeof(records[record_index].Name) - 1);
    records[record_index].Mapper = (unsigned char)(MP3_FLAG | SOURCE_SD_FLAG);
    records[record_index].Size = (unsigned long)size;
    records[record_index].Offset = (unsigned long)record_index;
    return (uint16_t)(record_index + 1);
}

static void refresh_records_for_current_path(void) {
    sd_record_count = 0;
    sd_path_buffer_used = 0;
    for (uint16_t i = 0; i < MAX_ROM_RECORDS; i++) {
        sd_path_offsets[i] = 0xFFFF;
    }

    uint16_t record_index = 0;
    bool has_parent = !is_root_path(sd_current_path);

    if (has_parent) {
        record_index = add_folder_record(record_index, "..");
    }

    uint16_t folder_count = record_index;
    if (sd_mount_card()) {
        DIR dir;
        FILINFO fno;
        FRESULT fr = f_opendir(&dir, sd_current_path);
        if (fr == FR_OK) {
            while (record_index < MAX_ROM_RECORDS) {
                fr = f_readdir(&dir, &fno);
                if (fr != FR_OK || fno.fname[0] == '\0') {
                    break;
                }
                if (!(fno.fattrib & AM_DIR)) {
                    continue;
                }
                if (is_excluded_folder(fno.fname)) {
                    continue;
                }
                record_index = add_folder_record(record_index, fno.fname);
            }
        }
        f_closedir(&dir);

        folder_count = record_index;
        uint16_t folder_sort_start = has_parent ? 1u : 0u;
        if (folder_count > folder_sort_start) {
            sort_records_range(folder_sort_start, (uint16_t)(folder_count - folder_sort_start));
        }

        fr = f_opendir(&dir, sd_current_path);
        if (fr == FR_OK) {
            while (record_index < MAX_ROM_RECORDS) {
                fr = f_readdir(&dir, &fno);
                if (fr != FR_OK || fno.fname[0] == '\0') {
                    break;
                }
                if (fno.fattrib & AM_DIR) {
                    continue;
                }
                bool is_mp3 = has_mp3_extension(fno.fname);
                bool is_rom = has_rom_extension(fno.fname);
#if EXPLORER_MP3_DISABLED
                // MP3 player temporarily disabled — skip MP3 entries.
                if (is_mp3) continue;
#endif

                if (!is_mp3 && !is_rom) {
                    continue;
                }

                char path[SD_PATH_MAX];
                if (is_root_path(sd_current_path)) {
                    snprintf(path, sizeof(path), "/%s", fno.fname);
                } else {
                    snprintf(path, sizeof(path), "%s/%s", sd_current_path, fno.fname);
                }

                if (is_mp3) {
                    if (fno.fsize == 0) {
                        continue;
                    }
                    record_index = add_sd_mp3_record(record_index, path, fno.fname, (uint32_t)fno.fsize);
                    sd_record_count++;
                    continue;
                }

                if (fno.fsize < MIN_ROM_SIZE || fno.fsize > MAX_ROM_SIZE) {
                    continue;
                }
                if (fno.fsize > SD_ROM_MAX_SIZE) {
                    continue;
                }

                uint8_t mapper = mapper_number_from_filename(fno.fname);
                record_index = add_sd_rom_record(record_index, path, fno.fname, (uint32_t)fno.fsize, mapper);
                sd_record_count++;
            }
        }
        f_closedir(&dir);
    }

    if (is_root_path(sd_current_path) && flash_record_count > 0) {
        for (uint16_t i = 0; i < flash_record_count && record_index < MAX_ROM_RECORDS; i++) {
            records[record_index] = flash_records[i];
            sd_path_offsets[record_index] = 0xFFFF;
            record_index++;
        }
    }

    uint16_t rom_count = (uint16_t)(record_index - folder_count);
    if (rom_count > 1) {
        uint16_t system_insert = folder_count;
        for (uint16_t i = folder_count; i < record_index; i++) {
            if (is_system_record(&records[i])) {
                swap_records(i, system_insert);
                system_insert++;
            }
        }

        uint16_t system_count = (uint16_t)(system_insert - folder_count);
        uint16_t non_system_count = (uint16_t)(record_index - system_insert);
        if (system_count > 1) {
            sort_records_range(folder_count, system_count);
        }
        if (non_system_count > 1) {
            sort_records_range(system_insert, non_system_count);
        }
    }

    full_record_count = record_index;
    memset(filter_query, 0, sizeof(filter_query));
    apply_filter();
    current_page = 0;
    build_page_buffer(current_page);
}

// Chunked version of refresh_records_for_current_path.
// Processes up to REFRESH_CHUNK_SIZE SD reads per call, then returns false
// to yield back to the MP3 decode loop. Returns true when done (or idle).
static bool refresh_records_chunked(void) {
    FILINFO fno;
    FRESULT fr;

    switch (refresh_state) {
    case REFRESH_IDLE:
        return true;

    case REFRESH_INIT:
        sd_record_count = 0;
        sd_path_buffer_used = 0;
        for (uint16_t i = 0; i < MAX_ROM_RECORDS; i++) {
            sd_path_offsets[i] = 0xFFFF;
        }
        refresh_record_index = 0;
        refresh_has_parent = !is_root_path(sd_current_path);
        if (refresh_has_parent) {
            refresh_record_index = add_folder_record(refresh_record_index, "..");
        }
        refresh_folder_count = refresh_record_index;
        if (!sd_mount_card()) {
            refresh_state = REFRESH_FINALIZE;
            return false;
        }
        fr = f_opendir(&refresh_dir, sd_current_path);
        if (fr != FR_OK) {
            refresh_state = REFRESH_FINALIZE;
            return false;
        }
        refresh_state = REFRESH_SCAN_FOLDERS;
        return false;

    case REFRESH_SCAN_FOLDERS: {
        int reads = 0;
        while (reads < REFRESH_CHUNK_SIZE && refresh_record_index < MAX_ROM_RECORDS) {
            fr = f_readdir(&refresh_dir, &fno);
            reads++;
            if (fr != FR_OK || fno.fname[0] == '\0') {
                f_closedir(&refresh_dir);
                refresh_state = REFRESH_SORT_FOLDERS;
                return false;
            }
            if (!(fno.fattrib & AM_DIR)) continue;
            if (is_excluded_folder(fno.fname)) continue;
            refresh_record_index = add_folder_record(refresh_record_index, fno.fname);
        }
        if (refresh_record_index >= MAX_ROM_RECORDS) {
            f_closedir(&refresh_dir);
            refresh_state = REFRESH_SORT_FOLDERS;
        }
        return false;
    }

    case REFRESH_SORT_FOLDERS: {
        refresh_folder_count = refresh_record_index;
        uint16_t folder_sort_start = refresh_has_parent ? 1u : 0u;
        if (refresh_folder_count > folder_sort_start) {
            sort_records_range(folder_sort_start, (uint16_t)(refresh_folder_count - folder_sort_start));
        }
        fr = f_opendir(&refresh_dir, sd_current_path);
        if (fr != FR_OK) {
            refresh_state = REFRESH_FINALIZE;
            return false;
        }
        refresh_state = REFRESH_SCAN_FILES;
        return false;
    }

    case REFRESH_SCAN_FILES: {
        int reads = 0;
        while (reads < REFRESH_CHUNK_SIZE && refresh_record_index < MAX_ROM_RECORDS) {
            fr = f_readdir(&refresh_dir, &fno);
            reads++;
            if (fr != FR_OK || fno.fname[0] == '\0') {
                f_closedir(&refresh_dir);
                refresh_state = REFRESH_FINALIZE;
                return false;
            }
            if (fno.fattrib & AM_DIR) continue;
            bool is_mp3 = has_mp3_extension(fno.fname);
            bool is_rom = has_rom_extension(fno.fname);
#if EXPLORER_MP3_DISABLED
            // MP3 player temporarily disabled — skip MP3 entries.
            if (is_mp3) continue;
#endif
            if (!is_mp3 && !is_rom) continue;

            char path[SD_PATH_MAX];
            if (is_root_path(sd_current_path)) {
                snprintf(path, sizeof(path), "/%s", fno.fname);
            } else {
                snprintf(path, sizeof(path), "%s/%s", sd_current_path, fno.fname);
            }

            if (is_mp3) {
                if (fno.fsize == 0) continue;
                refresh_record_index = add_sd_mp3_record(refresh_record_index, path, fno.fname, (uint32_t)fno.fsize);
                sd_record_count++;
                continue;
            }

            if (fno.fsize < MIN_ROM_SIZE || fno.fsize > MAX_ROM_SIZE) continue;
            if (fno.fsize > SD_ROM_MAX_SIZE) continue;

            uint8_t mapper = mapper_number_from_filename(fno.fname);
            refresh_record_index = add_sd_rom_record(refresh_record_index, path, fno.fname, (uint32_t)fno.fsize, mapper);
            sd_record_count++;
        }
        if (refresh_record_index >= MAX_ROM_RECORDS) {
            f_closedir(&refresh_dir);
            refresh_state = REFRESH_FINALIZE;
        }
        return false;
    }

    case REFRESH_FINALIZE: {
        if (is_root_path(sd_current_path) && flash_record_count > 0) {
            for (uint16_t i = 0; i < flash_record_count && refresh_record_index < MAX_ROM_RECORDS; i++) {
                records[refresh_record_index] = flash_records[i];
                sd_path_offsets[refresh_record_index] = 0xFFFF;
                refresh_record_index++;
            }
        }

        uint16_t rom_count = (uint16_t)(refresh_record_index - refresh_folder_count);
        if (rom_count > 1) {
            uint16_t system_insert = refresh_folder_count;
            for (uint16_t i = refresh_folder_count; i < refresh_record_index; i++) {
                if (is_system_record(&records[i])) {
                    swap_records(i, system_insert);
                    system_insert++;
                }
            }
            uint16_t system_count = (uint16_t)(system_insert - refresh_folder_count);
            uint16_t non_system_count = (uint16_t)(refresh_record_index - system_insert);
            if (system_count > 1) {
                sort_records_range(refresh_folder_count, system_count);
            }
            if (non_system_count > 1) {
                sort_records_range(system_insert, non_system_count);
            }
        }

        full_record_count = refresh_record_index;
        memset(filter_query, 0, sizeof(filter_query));
        apply_filter();
        current_page = 0;
        build_page_buffer(current_page);
        refresh_state = REFRESH_IDLE;
        return true;
    }

    default:
        refresh_state = REFRESH_IDLE;
        return true;
    }
}

static void process_detect_mapper_request(uint16_t filtered_index) {
    ctrl_mapper_value = 0;

    if (!sd_mount_card()) {
        ctrl_cmd_state = 0;
        return;
    }

    if (filtered_index >= total_record_count) {
        ctrl_cmd_state = 0;
        return;
    }

    uint16_t record_index = filtered_indices[filtered_index];
    if (record_index >= full_record_count) {
        ctrl_cmd_state = 0;
        return;
    }

    ROMRecord *rec = &records[record_index];
    uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);

    if (!(flags & SOURCE_SD_FLAG) || (flags & (FOLDER_FLAG | MP3_FLAG)) || sd_path_offsets[record_index] == 0xFFFF) {
        ctrl_mapper_value = rec->Mapper & ~(SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG | OVERRIDE_FLAG);
        ctrl_cmd_state = 0;
        return;
    }

    const char *path = sd_path_buffer + sd_path_offsets[record_index];
    const char *filename = basename_from_path(path);

    uint8_t mapper = mapper_number_from_filename(filename);
    if (mapper == 0) {
        mapper = detect_rom_type_from_file(path, (uint32_t)rec->Size);
    }

    ctrl_mapper_value = mapper;
    if (mapper != 0) {
        rec->Mapper = (uint8_t)(flags | mapper);
    } else {
        rec->Mapper = flags;
    }

    ctrl_cmd_state = 0;
}

// Populate the now-playing buffer with name + size from a record.
static void update_mp3_now_playing(uint16_t record_index) {
    if (record_index >= full_record_count) return;
    const ROMRecord *rec = &records[record_index];
    // Copy name (space-padded to ROM_NAME_MAX)
    memcpy(mp3_now_playing_buf, rec->Name, ROM_NAME_MAX);
    // Write size as 4-byte LE at offset 60
    uint32_t sz = (uint32_t)rec->Size;
    mp3_now_playing_buf[ROM_NAME_MAX + 0] = (uint8_t)(sz & 0xFFu);
    mp3_now_playing_buf[ROM_NAME_MAX + 1] = (uint8_t)((sz >> 8) & 0xFFu);
    mp3_now_playing_buf[ROM_NAME_MAX + 2] = (uint8_t)((sz >> 16) & 0xFFu);
    mp3_now_playing_buf[ROM_NAME_MAX + 3] = (uint8_t)((sz >> 24) & 0xFFu);
}

// Find the next MP3 filtered index for auto-advance.
// mode: MP3_PLAY_MODE_ALL = sequential wrap, MP3_PLAY_MODE_RANDOM = random pick.
static uint16_t find_next_mp3_filtered_index(uint16_t current, uint8_t mode) {
    if (total_record_count == 0) return 0xFFFF;

    if (mode == MP3_PLAY_MODE_RANDOM) {
        // Count MP3 entries
        uint16_t mp3_count = 0;
        for (uint16_t i = 0; i < total_record_count; i++) {
            uint16_t ri = filtered_indices[i];
            if (ri < full_record_count && (records[ri].Mapper & MP3_FLAG))
                mp3_count++;
        }
        if (mp3_count == 0) return 0xFFFF;

        // Use microsecond timer as entropy for random pick
        uint16_t target = (uint16_t)(time_us_32() % mp3_count);
        uint16_t ord = 0;
        for (uint16_t i = 0; i < total_record_count; i++) {
            uint16_t ri = filtered_indices[i];
            if (ri < full_record_count && (records[ri].Mapper & MP3_FLAG)) {
                if (ord == target) return i;
                ord++;
            }
        }
        return 0xFFFF;
    }

    // ALL mode: find next MP3 after current, wrapping around
    uint16_t start = (current != 0xFFFF && current + 1 < total_record_count)
                     ? (current + 1) : 0;
    for (uint16_t count = 0; count < total_record_count; count++) {
        uint16_t i = (start + count) % total_record_count;
        uint16_t ri = filtered_indices[i];
        if (ri < full_record_count && (records[ri].Mapper & MP3_FLAG)) {
            return i;
        }
    }
    return 0xFFFF;
}

// Lazy non-blocking launch of Core 1 + MP3 stack. Called from
// core1_bg_work() the first time the MSX dispatches an MP3 command.
// We deliberately do NOT spin Core 0 waiting for Core 1 to finish
// mp3_init() — Core 0 is servicing the MSX PIO bus and any block
// here freezes the cartridge. The cross-core command queue is a
// small ring buffer so SELECT and PLAY (issued ~10 ms apart) both
// fit while Core 1 is still initialising.
static bool mp3_core1_started = false;
static void ensure_mp3_core1_started(void) {
#if EXPLORER_MP3_DISABLED
    // MP3 player disabled: never launch Core 1 from the menu, so the
    // SD card stays single-owner on Core 0 with no risk of cross-core
    // FatFS / I2S / DMA contention.
    return;
#else
    if (mp3_core1_started) return;
    mp3_core1_started = true;
    multicore_launch_core1(mp3_core1_loop);
#endif
}

// Background work pump. Runs on Core 0 from the menu loop idle branch
// (no PIO traffic from the MSX). Single-core ownership of the SD card
// avoids FatFS reentrancy issues and removes the freeze observed when
// folder navigation raced with MP3 work on Core 1.
static void core1_bg_work(void) {
    // Bridge MP3 commands queued by handle_menu_write_explorer() to the
    // mp3.c command queue. Lazy-launch Core 1 the first time an MP3
    // command actually arrives.
    if (mp3_pending_select) {
        uint16_t pending_index = mp3_pending_index;
        mp3_pending_select = false;
        if (pending_index < total_record_count) {
            uint16_t record_index = filtered_indices[pending_index];
            if (record_index < full_record_count) {
                ROMRecord const *rec = &records[record_index];
                if ((rec->Mapper & MP3_FLAG) && (sd_path_offsets[record_index] != 0xFFFF)) {
                    const char *path = sd_path_buffer + sd_path_offsets[record_index];
                    printf("MP3: select path=%s size=%lu\n", path, (unsigned long)rec->Size);
                    ensure_mp3_core1_started();
                    mp3_select_file(path, (uint32_t)rec->Size);
                    mp3_playing_filtered_index = pending_index;
                    update_mp3_now_playing(record_index);
                } else {
                    printf("MP3: select ignored (not mp3 or no path)\n");
                }
            } else {
                printf("MP3: select index out of range\n");
            }
        } else {
            printf("MP3: select index invalid (total=%u)\n", (unsigned int)total_record_count);
        }
    }
    if (mp3_pending_cmd != 0) {
        uint8_t cmd = mp3_pending_cmd;
        mp3_pending_cmd = 0;
        ensure_mp3_core1_started();
        mp3_send_cmd(cmd);
    }

    // Auto-advance: when current track ends and mode is ALL or RANDOM,
    // automatically select and play the next track.
    if (mp3_play_mode != MP3_PLAY_MODE_SINGLE) {
        uint8_t st = mp3_get_status();
        if ((st & MP3_STATUS_EOF) && !(st & MP3_STATUS_PLAYING)) {
            uint16_t next = find_next_mp3_filtered_index(
                mp3_playing_filtered_index, mp3_play_mode);
            if (next != 0xFFFF && next < total_record_count) {
                uint16_t ri = filtered_indices[next];
                if (ri < full_record_count &&
                    (records[ri].Mapper & MP3_FLAG) &&
                    sd_path_offsets[ri] != 0xFFFF) {
                    const char *path = sd_path_buffer + sd_path_offsets[ri];
                    printf("MP3: auto-advance to index %u path=%s\n",
                           (unsigned)next, path);
                    mp3_auto_play(path, (uint32_t)records[ri].Size);
                    mp3_playing_filtered_index = next;
                    mp3_selected_index = next;
                    update_mp3_now_playing(ri);
                }
            }
        }
    }

    // Defer SD-heavy work while MP3 is actively playing to avoid
    // SPI bus contention between directory reads and MP3 file reads.
    if (mp3_get_status() & MP3_STATUS_PLAYING) {
        return;
    }

    // Chunked directory refresh — process one chunk then yield
    if (refresh_state != REFRESH_IDLE) {
        bool done = refresh_records_chunked();
        if (done) {
            refresh_in_progress = false;
            ctrl_cmd_state = 0;
        }
        return;
    }

    if (refresh_requested) {
        refresh_requested = false;
        refresh_in_progress = true;
        refresh_state = REFRESH_INIT;
        return;
    }

    if (detect_mapper_pending && !refresh_in_progress) {
        uint16_t pending_index = detect_mapper_index;
        detect_mapper_pending = false;
        process_detect_mapper_request(pending_index);
    }
}

static bool load_rom_from_sd(uint16_t record_index, uint32_t size) {
    if (!sd_mount_card()) {
        return false;
    }
    if (record_index >= MAX_ROM_RECORDS) {
        return false;
    }
    if (sd_path_offsets[record_index] == 0xFFFF) {
        return false;
    }

    // SD ROMs are streamed into the 2MB PSRAM region so rom_sram remains
    // available for flash-resident mapper caching. Bring PSRAM up lazily.
    if (!psram_bring_up_once()) {
        return false;
    }
    if (size > sd_rom_region.size) {
        return false;
    }

    const char *path = sd_path_buffer + sd_path_offsets[record_index];

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        return false;
    }

    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    uint8_t *dst = sd_rom_region.ptr;
    memset(dst, 0, size);

    UINT total = 0;
    while (total < size) {
        UINT to_read = (UINT)((size - total) > 4096 ? 4096 : (size - total));
        UINT br = 0;
        fr = f_read(&fil, dst + total, to_read, &br);
        if (fr != FR_OK || br == 0) {
            break;
        }
        total += br;
    }

    f_close(&fil);
    gpio_put(PIN_WAIT, 1);

    return total == size;
}

// Initialize GPIO pins
static inline void setup_gpio()
{

    for (int i = 0; i <= 23; i++) {
        gpio_init(i);
        gpio_set_input_hysteresis_enabled(i, true);
    }

    for (int i = 0; i <= 15; i++) {
        gpio_set_dir(i, GPIO_IN);
    }

    // Initialize control pins as input
    gpio_init(PIN_RD); gpio_set_dir(PIN_RD, GPIO_IN);    
    gpio_init(PIN_WR); gpio_set_dir(PIN_WR, GPIO_IN); 
    gpio_init(PIN_IORQ); gpio_set_dir(PIN_IORQ, GPIO_IN); 
    gpio_init(PIN_SLTSL); gpio_set_dir(PIN_SLTSL, GPIO_IN); 
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN); 
}

// read_ulong - Read a 4-byte value from the memory area
// This function will read a 4-byte value from the memory area pointed by ptr and return the value as an unsigned long
// Parameters:
//   ptr - Pointer to the memory area to read the value from
// Returns:
//   The 4-byte value as an unsigned long 
unsigned long __no_inline_not_in_flash_func(read_ulong)(const unsigned char *ptr) {
    return (unsigned long)ptr[0] |
           ((unsigned long)ptr[1] << 8) |
           ((unsigned long)ptr[2] << 16) |
           ((unsigned long)ptr[3] << 24);
}

// isEndOfData - Check if the memory area is the end of the data
// This function will check if the memory area pointed by memory is the end of the data. The end of the data is defined by all bytes being 0xFF.
// Parameters:
//   memory - Pointer to the memory area to check
// Returns:
//   1 if the memory area is the end of the data, 0 otherwise
int __no_inline_not_in_flash_func(isEndOfData)(const unsigned char *memory) {
    for (int i = 0; i < ROM_RECORD_SIZE; i++) {
        if (memory[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

// -----------------------------------------------------------------------
// External PSRAM (QMI CS1) - hardware init + bump allocator.
// Mirrors the implementation in multirom so the same code paths work
// across the two firmwares. Used for SD-cached ROM storage so that
// rom_sram remains fully available for flash-resident mapper caching.
// -----------------------------------------------------------------------
static inline void __not_in_flash_func(psram_delay_cycles)(uint32_t cycles)
{
    for (volatile uint32_t cycle = 0; cycle < cycles; ++cycle)
    {
        __asm volatile ("nop");
    }
}

static inline void __not_in_flash_func(psram_wait_direct_done)(void)
{
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) { }
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) { }
}

static inline void __not_in_flash_func(psram_send_direct_cmd)(uint8_t cmd, bool quad_width)
{
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    if (quad_width)
    {
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                            (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                            cmd;
    }
    else
    {
        qmi_hw->direct_tx = cmd;
    }
    psram_wait_direct_done();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    (void)qmi_hw->direct_rx;
}

static bool __no_inline_not_in_flash_func(psram_init)(void)
{
    gpio_set_function(PIN_PSRAM, GPIO_FUNC_XIP_CS1);

    uint32_t irq_state = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) { }

    // Warm restart path: if PSRAM remained powered it may still be in QPI mode.
    psram_send_direct_cmd(0xF5u, true);
    psram_delay_cycles(128u);

    psram_send_direct_cmd(0x66u, false);
    psram_delay_cycles(128u);
    psram_send_direct_cmd(0x99u, false);
    psram_delay_cycles(50000u);
    psram_send_direct_cmd(0x35u, false);
    psram_delay_cycles(128u);
    psram_send_direct_cmd(0xC0u, false);
    psram_delay_cycles(128u);

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    // PSRAM QMI timing - tuned for 210 MHz system clock (CLKDIV=2 -> ~52.5 MHz).
    qmi_hw->m[1].timing =
        (QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB) |
        (1u << QMI_M0_TIMING_SELECT_HOLD_LSB) |
        (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
        (2u << QMI_M0_TIMING_RXDELAY_LSB) |
        (26u << QMI_M0_TIMING_MAX_SELECT_LSB) |
        (5u << QMI_M0_TIMING_MIN_DESELECT_LSB) |
        (2u << QMI_M0_TIMING_CLKDIV_LSB);
    qmi_hw->m[1].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_LEN_VALUE_24 << QMI_M0_RFMT_DUMMY_LEN_LSB) |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB) |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB) |
        (QMI_M0_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = (0xEBu << QMI_M0_RCMD_PREFIX_LSB);
    qmi_hw->m[1].wfmt =
        (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M0_WFMT_DUMMY_LEN_LSB) |
        (QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB) |
        (QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB) |
        (QMI_M0_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = (0x38u << QMI_M0_WCMD_PREFIX_LSB);

    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    restore_interrupts(irq_state);

    // Verify PSRAM via uncached window (bypass XIP cache).
    volatile uint32_t *psram_test = (volatile uint32_t *)0x15000000u;
    psram_test[0] = 0x12345678u;
    return psram_test[0] == 0x12345678u;
}

// -----------------------------------------------------------------------
// PSRAM Memory Manager - bump allocator for 8MB external PSRAM.
// -----------------------------------------------------------------------
static struct {
    uint32_t next_free;
    bool initialised;
} psram_mgr;

static void psram_mem_init(void)
{
    psram_mgr.next_free = 0;
    memset(&sd_rom_region, 0, sizeof(sd_rom_region));
}

static bool psram_alloc(uint32_t size, psram_region_t *region)
{
    if (psram_mgr.next_free + size > PSRAM_TOTAL_SIZE)
        return false;
    region->offset = psram_mgr.next_free;
    region->size   = size;
    region->ptr    = (uint8_t *)(PSRAM_BASE_ADDR + psram_mgr.next_free);
    psram_mgr.next_free += size;
    return true;
}

// One-time PSRAM bring-up + region carve-out. Idempotent. Returns false if
// the hardware probe fails or any allocation cannot be satisfied.
static bool psram_bring_up_once(void)
{
    if (psram_mgr.initialised) return true;
    if (!psram_init()) return false;
    psram_mem_init();
    // 2MB region for SD-loaded ROMs (covers 99% of MSX ROMs).
    if (!psram_alloc(2u * 1024u * 1024u, &sd_rom_region)) return false;
    psram_mgr.initialised = true;
    return true;
}

static inline void __not_in_flash_func(prepare_rom_source)(
    uint32_t offset,
    bool cache_enable,
    uint32_t preferred_size,
    const uint8_t **rom_base_out,
    uint32_t *available_length_out)
{
    const uint8_t *rom_base = rom_data + offset;
    uint32_t available_length = active_rom_size;

    // For SD-loaded ROMs rom_data points at PSRAM (sd_rom_region.ptr) with
    // offset 0; for flash-resident ROMs it points at QSPI flash. In both
    // cases we copy the leading rom_cache_capacity bytes into rom_sram for
    // fast access, leaving the tail to be served from the source via XIP.
    if (preferred_size != 0u && (available_length == 0u || available_length > preferred_size))
    {
        available_length = preferred_size;
    }

    if (cache_enable && available_length > 0u)
    {
        uint32_t cap = rom_cache_capacity;
        if (cap > sizeof(rom_sram)) cap = sizeof(rom_sram);
        uint32_t bytes_to_cache = (available_length > cap) ? cap : available_length;

        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);
        memcpy(rom_sram, rom_base, bytes_to_cache);
        gpio_put(PIN_WAIT, 1);

        rom_cached_size = bytes_to_cache;
        // If we cached everything, we can just point to SRAM
        if (active_rom_size <= cap)
        {
            rom_base = rom_sram;
        }
    }
    else
    {
        rom_cached_size = 0;
    }

    *rom_base_out = rom_base;
    *available_length_out = available_length;
}


static void msx_pio_bus_init(void)
{
    msx_bus.pio = pio1;
    msx_bus.sm_read  = 0;
    msx_bus.sm_write = 1;

    if (!msx_bus_programs_loaded)
    {
        msx_bus.offset_read  = pio_add_program(msx_bus.pio, &msx_read_responder_program);
        msx_bus.offset_write = pio_add_program(msx_bus.pio, &msx_write_captor_program);
        msx_bus_programs_loaded = true;
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_read);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_write);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_read);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_write);

    pio_sm_config cfg_read = msx_read_responder_program_get_default_config(msx_bus.offset_read);
    sm_config_set_in_pins(&cfg_read, PIN_A0);
    sm_config_set_in_shift(&cfg_read, false, false, 16);
    sm_config_set_out_pins(&cfg_read, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_read, true, false, 32);
    sm_config_set_sideset_pins(&cfg_read, PIN_WAIT);
    sm_config_set_jmp_pin(&cfg_read, PIN_RD);
    sm_config_set_clkdiv(&cfg_read, 1.0f);
    pio_sm_init(msx_bus.pio, msx_bus.sm_read, msx_bus.offset_read, &cfg_read);

    pio_sm_config cfg_write = msx_write_captor_program_get_default_config(msx_bus.offset_write);
    sm_config_set_in_pins(&cfg_write, PIN_A0);
    sm_config_set_in_shift(&cfg_write, false, false, 32);
    sm_config_set_fifo_join(&cfg_write, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_write, 1.0f);
    pio_sm_init(msx_bus.pio, msx_bus.sm_write, msx_bus.offset_write, &cfg_write);

    pio_gpio_init(msx_bus.pio, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_WAIT, 1, true);

    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        pio_gpio_init(msx_bus.pio, pin);
    }

    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_D0, 8, false);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_write, PIN_D0, 8, false);

    gpio_put(PIN_WAIT, 1);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, true);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, true);
}

static inline uint8_t __not_in_flash_func(read_rom_byte)(const uint8_t *rom_base, uint32_t rel)
{
    return (rel < rom_cached_size) ? rom_sram[rel] : rom_base[rel];
}

static inline uint16_t __not_in_flash_func(pio_build_token)(bool drive, uint8_t data)
{
    uint8_t dir_mask = drive ? 0xFFu : 0x00u;
    return (uint16_t)data | ((uint16_t)dir_mask << 8);
}

static inline bool __not_in_flash_func(pio_try_get_write)(uint16_t *addr_out, uint8_t *data_out)
{
    if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_write))
        return false;

    uint32_t sample = pio_sm_get(msx_bus.pio, msx_bus.sm_write);
    *addr_out = (uint16_t)(sample & 0xFFFFu);
    *data_out = (uint8_t)((sample >> 16) & 0xFFu);
    return true;
}

static inline void __not_in_flash_func(pio_drain_writes)(void (*handler)(uint16_t addr, uint8_t data, void *ctx), void *ctx)
{
    uint16_t addr;
    uint8_t data;
    while (pio_try_get_write(&addr, &data))
    {
        handler(addr, data, ctx);
    }
}

static inline void __not_in_flash_func(handle_menu_write)(uint16_t addr, uint8_t data, void *ctx)
{
    menu_select_ctx_t *menu_ctx = (menu_select_ctx_t *)ctx;
    
    // Original explorer menu handling logic needs to be adapted here
    // However, the original menu loop was complex handling many registers.
    // The handle_menu_write in multirom only checks MONITOR_ADDR.
    // Explorer needs to handle CTRL registers and MP3 registers too.

    // For now, I'll only put the basic MONITOR_ADDR check here 
    // AND I will have to implement a more complex handler or just use the main loop 
    // inside loadrom_msx_menu which will be refactored anyway.
    
    if (addr == MONITOR_ADDR)
    {
        menu_ctx->rom_index = data;
        menu_ctx->rom_selected = true;
    }
}

static inline void __not_in_flash_func(handle_konamiscc_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x5000u && addr <= 0x57FFu) regs[0] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu) regs[1] = data;
    else if (addr >= 0x9000u && addr <= 0x97FFu) regs[2] = data;
    else if (addr >= 0xB000u && addr <= 0xB7FFu) regs[3] = data;
}

static inline void __not_in_flash_func(handle_konami_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x6000u && addr <= 0x67FFu) regs[1] = data;
    else if (addr >= 0x8000u && addr <= 0x87FFu) regs[2] = data;
    else if (addr >= 0xA000u && addr <= 0xA7FFu) regs[3] = data;
}

static inline void __not_in_flash_func(handle_ascii8_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x6000u && addr <= 0x67FFu) regs[0] = data;
    else if (addr >= 0x6800u && addr <= 0x6FFFu) regs[1] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu) regs[2] = data;
    else if (addr >= 0x7800u && addr <= 0x7FFFu) regs[3] = data;
}

static inline void __not_in_flash_func(handle_ascii16_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x6000u && addr <= 0x67FFu) regs[0] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu) regs[1] = data;
}

static inline void __not_in_flash_func(handle_neo8_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint16_t *regs = ((bank16_ctx_t *)ctx)->bank_regs;
    uint16_t base_addr = addr & 0xF800u;
    uint8_t bank_index = 6;

    switch (base_addr)
    {
        case 0x5000: case 0x1000: case 0x9000: case 0xD000: bank_index = 0; break;
        case 0x5800: case 0x1800: case 0x9800: case 0xD800: bank_index = 1; break;
        case 0x6000: case 0x2000: case 0xA000: case 0xE000: bank_index = 2; break;
        case 0x6800: case 0x2800: case 0xA800: case 0xE800: bank_index = 3; break;
        case 0x7000: case 0x3000: case 0xB000: case 0xF000: bank_index = 4; break;
        case 0x7800: case 0x3800: case 0xB800: case 0xF800: bank_index = 5; break;
    }

    if (bank_index < 6u)
    {
        if (addr & 0x01u)
            regs[bank_index] = (regs[bank_index] & 0x00FFu) | ((uint16_t)data << 8);
        else
            regs[bank_index] = (regs[bank_index] & 0xFF00u) | data;
        regs[bank_index] &= 0x0FFFu;
    }
}

static inline void __not_in_flash_func(handle_neo16_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint16_t *regs = ((bank16_ctx_t *)ctx)->bank_regs;
    uint16_t base_addr = addr & 0xF800u;
    uint8_t bank_index = 3;

    switch (base_addr)
    {
        case 0x5000: case 0x1000: case 0x9000: case 0xD000: bank_index = 0; break;
        case 0x6000: case 0x2000: case 0xA000: case 0xE000: bank_index = 1; break;
        case 0x7000: case 0x3000: case 0xB000: case 0xF000: bank_index = 2; break;
    }

    if (bank_index < 3u)
    {
        if (addr & 0x01u)
            regs[bank_index] = (regs[bank_index] & 0x00FFu) | ((uint16_t)data << 8);
        else
            regs[bank_index] = (regs[bank_index] & 0xFF00u) | data;
        regs[bank_index] &= 0x0FFFu;
    }
}

static void __no_inline_not_in_flash_func(banked8_loop)(
    const uint8_t *rom_base,
    uint32_t available_length,
    uint8_t *bank_regs,
    void (*write_handler)(uint16_t, uint8_t, void *))
{
    bank8_ctx_t ctx = { .bank_regs = bank_regs };

    while (true)
    {
        pio_drain_writes(write_handler, &ctx);

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(write_handler, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint32_t rel = ((uint32_t)bank_regs[(addr - 0x4000u) >> 13] * 0x2000u) + (addr & 0x1FFFu);
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

typedef struct {
    bool rom_selected;
    uint8_t rom_index;
} explorer_menu_ctx_t;

static inline void __not_in_flash_func(handle_menu_write_explorer)(uint16_t addr, uint8_t data, void *ctx)
{
    explorer_menu_ctx_t *menu_ctx = (explorer_menu_ctx_t *)ctx;

    if (addr >= CTRL_QUERY_BASE && addr < (CTRL_QUERY_BASE + CTRL_QUERY_SIZE))
    {
        filter_query[addr - CTRL_QUERY_BASE] = (char)data;
        return;
    }

    if (addr == CTRL_CMD)
    {
        ctrl_cmd_state = data;
        if (data == CMD_APPLY_FILTER)
        {
            filter_query[CTRL_QUERY_SIZE - 1] = '\0';
            apply_filter();
            current_page = 0;
            build_page_buffer(current_page);
        }
        else if (data == CMD_FIND_FIRST)
        {
            filter_query[CTRL_QUERY_SIZE - 1] = '\0';
            find_first_match();
        }
        else if (data == CMD_ENTER_DIR)
        {
            filter_query[CTRL_QUERY_SIZE - 1] = '\0';
            if (filter_query[0] == '\0') {
                go_up_one_level();
            } else {
                append_folder_to_path(filter_query);
            }
            refresh_requested = true;
            // Keep ctrl_cmd_state as CMD_ENTER_DIR to signal busy/done?
            // Original code: ctrl_cmd_state = CMD_ENTER_DIR;
            // Then it checked if (cmd != CMD_ENTER_DIR...) ctrl_cmd_state = 0;
            // Here data is CMD_ENTER_DIR.
        }
        else if (data == CMD_DETECT_MAPPER)
        {
             uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
             detect_mapper_index = index;
             detect_mapper_pending = true;
             ctrl_mapper_value = 0;
        }
        else if (data == CMD_SET_MAPPER)
        {
             uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
             uint8_t mapper = (uint8_t)filter_query[2];
             
             ctrl_ack_value = 0;
             if (index < total_record_count && mapper != 0 && mapper != MAPPER_SYSTEM && mapper < MAPPER_DESCRIPTION_COUNT) {
                 uint16_t record_index = filtered_indices[index];
                 if (record_index < full_record_count) {
                    ROMRecord *rec = &records[record_index];
                    uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);
                    if ((flags & (FOLDER_FLAG | MP3_FLAG)) == 0) {
                        rec->Mapper = (uint8_t)(flags | OVERRIDE_FLAG | mapper);
                        ctrl_ack_value = 1;
                    }
                 }
             }
        }
        
        if (data != CMD_ENTER_DIR && data != CMD_DETECT_MAPPER) {
             ctrl_cmd_state = 0;
        }
        return;
    }

    if (addr == MP3_CTRL_INDEX_L) {
        mp3_selected_index = (uint16_t)((mp3_selected_index & 0xFF00u) | data);
        return;
    }
    if (addr == MP3_CTRL_INDEX_H) {
        mp3_selected_index = (uint16_t)((mp3_selected_index & 0x00FFu) | ((uint16_t)data << 8));
        return;
    }

    if (addr == MP3_CTRL_CMD) {
        if (data == MP3_CMD_SELECT) {
             if (mp3_selected_index < total_record_count) {
                 mp3_pending_index = mp3_selected_index;
                 mp3_pending_select = true;
             }
        } else if (data == MP3_CMD_PLAY || data == MP3_CMD_STOP || data == MP3_CMD_TOGGLE_MUTE) {
             mp3_pending_cmd = data;
        }
        return;
    }

    if (addr == MP3_CTRL_MODE) {
        if (data <= MP3_PLAY_MODE_RANDOM) {
            mp3_play_mode = data;
        }
        return;
    }

    if (addr == CTRL_AUDIO) {
        ctrl_audio_selection = data;
        return;
    }

    if (addr == CTRL_PAGE) {
        if (data != current_page) {
            current_page = data;
            build_page_buffer(current_page);
        }
        return;
    }

    if (addr == MONITOR_ADDR) {
        uint8_t selected_index = data;
        if (selected_index < total_record_count) {
            menu_ctx->rom_index = (uint8_t)filtered_indices[selected_index];
        } else {
            menu_ctx->rom_index = 0;
        }
        menu_ctx->rom_selected = true;
        return;
    }
}

//load the MSX Menu ROM into the MSX
int __no_inline_not_in_flash_func(loadrom_msx_menu)(uint32_t offset)
{
    //setup the rom_sram buffer for the 32KB ROM
    gpio_init(PIN_WAIT); // Init wait signal pin
    gpio_set_dir(PIN_WAIT, GPIO_OUT); // Set the WAIT signal as output
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, MENU_ROM_SIZE); // Clear the SRAM buffer
    memcpy(rom_sram, flash_rom + offset, MENU_ROM_SIZE); // Load full 32KB menu ROM
    gpio_put(PIN_WAIT, 1); // Lets go!

    mp3_set_external_buffer(rom_sram + MENU_ROM_SIZE, sizeof(rom_sram) - MENU_ROM_SIZE);

    int record_count = 0; // Record count
    const uint8_t *record_ptr = flash_rom + offset + MENU_ROM_SIZE; // Pointer to the ROM records
    for (int i = 0; i < MAX_FLASH_RECORDS; i++)      // Read the ROMs from the configuration area
    {
        if (isEndOfData(record_ptr)) {
            break; // Stop if end of data is reached
        }
        char flash_name[ROM_NAME_MAX + 1];
        memcpy(flash_name, record_ptr, ROM_NAME_MAX);
        flash_name[ROM_NAME_MAX] = '\0';
        memset(flash_records[record_count].Name, 0, sizeof(flash_records[record_count].Name));
        trim_name_copy(flash_records[record_count].Name, flash_name);
        record_ptr += ROM_NAME_MAX; // Move the pointer to the next field
        flash_records[record_count].Mapper = *record_ptr++; // Read the mapper code
        flash_records[record_count].Size = read_ulong(record_ptr); // Read the ROM size
        record_ptr += sizeof(unsigned long); // Move the pointer to the next field
        flash_records[record_count].Offset = read_ulong(record_ptr); // Read the ROM offset
        record_ptr += sizeof(unsigned long); // Move the pointer to the next record
        record_count++; // Increment the record count
    }
    flash_record_count = (uint16_t)record_count;
    set_root_path();
    refresh_records_for_current_path();

    // Core 1 is intentionally NOT launched here. Folder navigation,
    // mapper detection, and SD enumeration all run on Core 0 from the
    // menu loop's idle branch, so the SD card has a single owner and
    // Core 1 cannot contend on shared buses. Core 1 is lazy-started
    // by ensure_mp3_core1_started() the first time an MP3 command is
    // dispatched. The cross-core command queue is a small FIFO so
    // SELECT+PLAY (issued ~10 ms apart by the menu) both fit while
    // mp3_init() is still running on Core 1.

    uint8_t rom_index = 0;
    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
    bool rom_selected = false; // ROM selected flag
    rom_cached_size = MENU_ROM_SIZE;
    msx_pio_bus_init();

    explorer_menu_ctx_t menu_ctx = {0};

    while (true)
    {
        pio_drain_writes(handle_menu_write_explorer, &menu_ctx);

        if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read)) {
            // PIO idle (SLTSL high).  On MSX1 the BIOS never selects
            // the cartridge at addr 0x0000 after rst 0x00, so detect
            // the reboot on the raw bus via GPIO.
            if (menu_ctx.rom_selected &&
                !gpio_get(PIN_RD) &&
                ((gpio_get_all() & 0xFFFFu) == 0x0000u))
            {
                return menu_ctx.rom_index;
            }
            // Background work runs on Core 0 during MSX bus idle: chunked
            // SD directory refresh, mapper detection, and bridging MP3
            // commands to Core 1 (lazy-launched on first MP3 command).
            core1_bg_work();
            tight_loop_contents();
            continue;
        }

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(handle_menu_write_explorer, &menu_ctx);

        bool in_window = false;
        uint8_t data = 0xFFu;
        bool drive = false;

        if (menu_ctx.rom_selected && addr == 0x0000u)
        {
            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(false, 0xFFu));
            return menu_ctx.rom_index;
        }

        if (addr >= 0x4000u && addr <= 0xBFFFu)
        {
            in_window = true;
            drive = true;

            if (addr >= CTRL_BASE_ADDR && addr <= (CTRL_BASE_ADDR + 0x0F))
            {
                switch (addr)
                {
                    case CTRL_COUNT_L: data = (uint8_t)(total_record_count & 0xFFu); break;
                    case CTRL_COUNT_H: data = (uint8_t)((total_record_count >> 8) & 0xFFu); break;
                    case CTRL_PAGE:    data = current_page; break;
                    case CTRL_STATUS:  data = CTRL_MAGIC; break;
                    case CTRL_CMD:     data = ctrl_cmd_state; break;
                    case CTRL_MATCH_L: data = (uint8_t)(match_index & 0xFFu); break;
                    case CTRL_MATCH_H: data = (uint8_t)((match_index >> 8) & 0xFFu); break;
                    case CTRL_MAPPER:  data = ctrl_mapper_value; break;
                    case CTRL_ACK:     data = ctrl_ack_value; break;
                    case CTRL_AUDIO:   data = ctrl_audio_selection; break;
                }
            }
            else if (addr >= MP3_NOW_PLAYING_BASE && addr < (MP3_NOW_PLAYING_BASE + MP3_NOW_PLAYING_SIZE))
            {
                data = mp3_now_playing_buf[addr - MP3_NOW_PLAYING_BASE];
            }
            else if (addr >= MP3_CTRL_BASE && addr <= (MP3_CTRL_BASE + 0x0F))
            {
                uint16_t elapsed = mp3_get_elapsed_seconds();
                uint16_t total = mp3_get_total_seconds();
                switch (addr)
                {
                    case MP3_CTRL_STATUS:    data = mp3_get_status(); break;
                    case MP3_CTRL_ELAPSED_L: data = (uint8_t)(elapsed & 0xFFu); break;
                    case MP3_CTRL_ELAPSED_H: data = (uint8_t)((elapsed >> 8) & 0xFFu); break;
                    case MP3_CTRL_TOTAL_L:   data = (uint8_t)(total & 0xFFu); break;
                    case MP3_CTRL_TOTAL_H:   data = (uint8_t)((total >> 8) & 0xFFu); break;
                    case MP3_CTRL_INDEX_L:   data = (uint8_t)(mp3_selected_index & 0xFFu); break;
                    case MP3_CTRL_INDEX_H:   data = (uint8_t)((mp3_selected_index >> 8) & 0xFFu); break;
                    case MP3_CTRL_MODE:      data = mp3_play_mode; break;
                }
            }
            else if (addr >= DATA_BASE_ADDR && addr < (DATA_BASE_ADDR + DATA_BUFFER_SIZE))
            {
                data = page_buffer[addr - DATA_BASE_ADDR];
            }
            else
            {
                uint32_t rel = addr - 0x4000u;
                if (rel < MENU_ROM_SIZE)
                {
                    data = rom_sram[rel];
                }
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(drive, data));
    }
    // Old implementation disabled
    /*
    while (true)
    {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)

        uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
        //uint16_t addr = sio_hw->gpio_in & 0x00FFFF;
        if (sltsl) 
        {
            bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low, not used)

            if (wr && addr >= CTRL_QUERY_BASE && addr < (CTRL_QUERY_BASE + CTRL_QUERY_SIZE))
            {
                    uint8_t value = (gpio_get_all() >> 16) & 0xFF;
                    filter_query[addr - CTRL_QUERY_BASE] = (char)value;
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == CTRL_CMD)
            {
                    uint8_t cmd = (gpio_get_all() >> 16) & 0xFF;
                    ctrl_cmd_state = cmd;
                    if (cmd == CMD_APPLY_FILTER)
                    {
                        filter_query[CTRL_QUERY_SIZE - 1] = '\0';
                        apply_filter();
                        current_page = 0;
                        build_page_buffer(current_page);
                    }
                    else if (cmd == CMD_FIND_FIRST)
                    {
                        filter_query[CTRL_QUERY_SIZE - 1] = '\0';
                        find_first_match();
                    }
                    else if (cmd == CMD_ENTER_DIR)
                    {
                        filter_query[CTRL_QUERY_SIZE - 1] = '\0';
                        if (filter_query[0] == '\0') {
                            go_up_one_level();
                        } else {
                            append_folder_to_path(filter_query);
                        }
                        refresh_requested = true;
                        ctrl_cmd_state = CMD_ENTER_DIR;
                    }
                    else if (cmd == CMD_DETECT_MAPPER)
                    {
                        uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
                        detect_mapper_index = index;
                        detect_mapper_pending = true;
                        ctrl_mapper_value = 0;
                        ctrl_cmd_state = CMD_DETECT_MAPPER;
                    }
                    else if (cmd == CMD_SET_MAPPER)
                    {
                        uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
                        uint8_t mapper = (uint8_t)filter_query[2];
                        ctrl_ack_value = 0;

                        if (index < total_record_count && mapper != 0 && mapper != MAPPER_SYSTEM && mapper < MAPPER_DESCRIPTION_COUNT) {
                            uint16_t record_index = filtered_indices[index];
                            if (record_index < full_record_count) {
                                ROMRecord *rec = &records[record_index];
                                uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);
                                if ((flags & (FOLDER_FLAG | MP3_FLAG)) == 0) {
                                    rec->Mapper = (uint8_t)(flags | OVERRIDE_FLAG | mapper);
                                    ctrl_ack_value = 1;
                                }
                            }
                        }
                        ctrl_cmd_state = 0;
                    }
                    if (cmd != CMD_ENTER_DIR && cmd != CMD_DETECT_MAPPER) {
                        ctrl_cmd_state = 0;
                    }
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == MP3_CTRL_INDEX_L)
            {
                    uint8_t value = (gpio_get_all() >> 16) & 0xFF;
                    mp3_selected_index = (uint16_t)((mp3_selected_index & 0xFF00u) | value);
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == MP3_CTRL_INDEX_H)
            {
                    uint8_t value = (gpio_get_all() >> 16) & 0xFF;
                    mp3_selected_index = (uint16_t)((mp3_selected_index & 0x00FFu) | ((uint16_t)value << 8));
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == MP3_CTRL_CMD)
            {
                    uint8_t cmd = (gpio_get_all() >> 16) & 0xFF;

                    if (cmd == MP3_CMD_SELECT)
                    {
                        if (mp3_selected_index < total_record_count)
                        {
                            mp3_pending_index = mp3_selected_index;
                            mp3_pending_select = true;
                        }
                    }
                    else if (cmd == MP3_CMD_PLAY || cmd == MP3_CMD_STOP || cmd == MP3_CMD_TOGGLE_MUTE)
                    {
                        mp3_pending_cmd = cmd;
                    }

                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == CTRL_PAGE)
            {
                    uint8_t page = (gpio_get_all() >> 16) & 0xFF;
                    if (page != current_page)
                    {
                        current_page = page;
                        build_page_buffer(current_page);
                    }
                    while (!(gpio_get(PIN_WR))) { // Wait until the write cycle completes (WR goes high)
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
            }

            if (wr && addr == MONITOR_ADDR) // Monitor ROM address to select the ROM
            {   
                    uint8_t selected_index = (gpio_get_all() >> 16) & 0xFF;
                    if (selected_index < total_record_count) {
                        rom_index = (uint8_t)filtered_indices[selected_index];
                    } else {
                        rom_index = 0;
                    }
                    while (!(gpio_get(PIN_WR))) { // Wait until the write cycle completes (WR goes high){
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                    rom_selected = true;    // ROM selected
            }

            if (addr >= 0x4000 && addr <= 0xBFFF) // Check if the address is within the ROM range
            {   
                if (rd)
                {
                    if (addr >= CTRL_BASE_ADDR && addr <= (CTRL_BASE_ADDR + 0x0F))
                    {
                        uint8_t ctrl_value = 0xFF;
                        switch (addr)
                        {
                            case CTRL_COUNT_L:
                                ctrl_value = (uint8_t)(total_record_count & 0xFFu);
                                break;
                            case CTRL_COUNT_H:
                                ctrl_value = (uint8_t)((total_record_count >> 8) & 0xFFu);
                                break;
                            case CTRL_PAGE:
                                ctrl_value = current_page;
                                break;
                            case CTRL_STATUS:
                                ctrl_value = CTRL_MAGIC;
                                break;
                            case CTRL_CMD:
                                ctrl_value = ctrl_cmd_state;
                                break;
                            case CTRL_MATCH_L:
                                ctrl_value = (uint8_t)(match_index & 0xFFu);
                                break;
                            case CTRL_MATCH_H:
                                ctrl_value = (uint8_t)((match_index >> 8) & 0xFFu);
                                break;
                            case CTRL_MAPPER:
                                ctrl_value = ctrl_mapper_value;
                                break;
                            case CTRL_ACK:
                                ctrl_value = ctrl_ack_value;
                                break;
                        }
                        gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                        gpio_put_masked(0xFF0000, (uint32_t)ctrl_value << 16);
                        while (!(gpio_get(PIN_RD))) {
                            tight_loop_contents();
                        }
                        gpio_set_dir_in_masked(0xFF << 16);
                        continue;
                    }

                    if (addr >= MP3_CTRL_BASE && addr <= (MP3_CTRL_BASE + 0x0F))
                    {
                        uint8_t ctrl_value = 0xFF;
                        uint16_t elapsed = mp3_get_elapsed_seconds();
                        uint16_t total = mp3_get_total_seconds();

                        switch (addr)
                        {
                            case MP3_CTRL_STATUS:
                                ctrl_value = mp3_get_status();
                                break;
                            case MP3_CTRL_ELAPSED_L:
                                ctrl_value = (uint8_t)(elapsed & 0xFFu);
                                break;
                            case MP3_CTRL_ELAPSED_H:
                                ctrl_value = (uint8_t)((elapsed >> 8) & 0xFFu);
                                break;
                            case MP3_CTRL_TOTAL_L:
                                ctrl_value = (uint8_t)(total & 0xFFu);
                                break;
                            case MP3_CTRL_TOTAL_H:
                                ctrl_value = (uint8_t)((total >> 8) & 0xFFu);
                                break;
                            case MP3_CTRL_INDEX_L:
                                ctrl_value = (uint8_t)(mp3_selected_index & 0xFFu);
                                break;
                            case MP3_CTRL_INDEX_H:
                                ctrl_value = (uint8_t)((mp3_selected_index >> 8) & 0xFFu);
                                break;
                        }
                        gpio_set_dir_out_masked(0xFF << 16);
                        gpio_put_masked(0xFF0000, (uint32_t)ctrl_value << 16);
                        while (!(gpio_get(PIN_RD))) {
                            tight_loop_contents();
                        }
                        gpio_set_dir_in_masked(0xFF << 16);
                        continue;
                    }

                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint8_t data;
                    if (addr >= DATA_BASE_ADDR && addr < (DATA_BASE_ADDR + DATA_BUFFER_SIZE)) {
                        data = page_buffer[addr - DATA_BASE_ADDR];
                    } else {
                        uint32_t rom_addr = (uint32_t)(addr - 0x4000); // Offset within 32KB menu ROM
                        data = rom_sram[rom_addr];
                    }
                    gpio_put_masked(0xFF0000, (uint32_t)data << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD))) { // Wait until the read cycle completes (RD goes high)
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                }
            } 
        }

        if (rd && addr == 0x0000 && rom_selected)   // lets return the rom_index and load the selected ROM
        {
            return rom_index;
        }
    }
    */
}

// loadrom_plain32 - Load a simple 32KB (or less) ROM into the MSX directly from the pico flash
// 32KB ROMS have two pages of 16Kb each in the following areas:
// 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x0000, 0x0001
// 16KB ROMS have only one page in the 0x4000-0x7FFF area
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 32768u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint32_t rel = addr - 0x4000u;
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_linear48 - Load a simple 48KB Linear0 ROM into the MSX directly from the pico flash
// Those ROMs have three pages of 16Kb each in the following areas:
// 0x0000-0x3FFF, 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x4000, 0x4001
void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 49152u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        bool in_window = (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;
        if (in_window && (available_length == 0u || addr < available_length))
            data = read_rom_byte(rom_base, addr);

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_konamiscc - Load a any Konami SCC ROM into the MSX directly from the pico flash
// The KonamiSCC ROMs are divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments 
// into the MSX's address space. Since the size of the mapper is 8Kb, the memory banks are:
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the address to change banks are:
// Bank 1: 5000h - 57FFh (5000h used), Bank 2: 7000h - 77FFh (7000h used), Bank 3: 9000h - 97FFh (9000h used), Bank 4: B000h - B7FFh (B000h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_konamiscc)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_konamiscc_write);
}

// -----------------------------------------------------------------------
// Core 1 audio entry: generates SCC samples and pushes to I2S
// -----------------------------------------------------------------------
static void __no_inline_not_in_flash_func(core1_scc_audio)(void)
{
    while (true)
    {
        struct audio_buffer *buffer = take_audio_buffer(scc_audio_pool, true);
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
        {
            int16_t raw = SCC_calc(&scc_instance);
            int32_t boosted = (int32_t)raw << SCC_VOLUME_SHIFT;
            if (boosted > 32767) boosted = 32767;
            else if (boosted < -32768) boosted = -32768;
            int16_t s = (int16_t)boosted;
            samples[i * 2]     = s;  // left
            samples[i * 2 + 1] = s;  // right
        }
        buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
        give_audio_buffer(scc_audio_pool, buffer);
    }
}

// -----------------------------------------------------------------------
// I2S audio initialisation for SCC emulation
// -----------------------------------------------------------------------
static void i2s_audio_init_scc(void)
{
    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 0);

    static audio_format_t scc_audio_format = {
        .sample_freq = SCC_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format scc_producer_format = {
        .format = &scc_audio_format,
        .sample_stride = 4,
    };

    scc_audio_pool = audio_new_producer_pool(&scc_producer_format, 3, SCC_AUDIO_BUFFER_SAMPLES);

    // Find a free DMA channel (SD SPI driver claims channels at init)
    int scc_dma_channel = -1;
    for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            scc_dma_channel = ch;
            break;
        }
    }
    if (scc_dma_channel < 0) {
        return;
    }

    static struct audio_i2s_config scc_i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = 0,
    };
    scc_i2s_config.dma_channel = (uint)scc_dma_channel;

    audio_i2s_setup(&scc_audio_format, &scc_i2s_config);
    audio_i2s_connect(scc_audio_pool);
    audio_i2s_set_enabled(true);
}

// -----------------------------------------------------------------------
// Konami SCC mapper with SCC sound emulation via I2S
// -----------------------------------------------------------------------
void __no_inline_not_in_flash_func(loadrom_konamiscc_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    // Hold WAIT to freeze Z80 during audio subsystem initialization,
    // preventing BIOS from scanning the slot before we are ready.
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    // Initialise SCC emulator (static allocation, no malloc)
    memset(&scc_instance, 0, sizeof(SCC));
    scc_instance.clk  = SCC_CLOCK;
    scc_instance.rate = SCC_SAMPLE_RATE;
    SCC_set_quality(&scc_instance, 1);
    scc_instance.type = scc_type;
    SCC_reset(&scc_instance);

    // Start I2S audio subsystem, then launch audio on core 1
    i2s_audio_init_scc();
    multicore_launch_core1(core1_scc_audio);

    msx_pio_bus_init();

    while (true)
    {
        // --- drain pending writes ---
        uint16_t waddr;
        uint8_t  wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            if      (waddr >= 0x5000u && waddr <= 0x57FFu) bank_registers[0] = wdata;
            else if (waddr >= 0x7000u && waddr <= 0x77FFu) bank_registers[1] = wdata;
            else if (waddr >= 0x9000u && waddr <= 0x97FFu) bank_registers[2] = wdata;
            else if (waddr >= 0xB000u && waddr <= 0xB7FFu) bank_registers[3] = wdata;

            // Forward to SCC emulator (handles enable + register writes)
            SCC_write(&scc_instance, waddr, wdata);
        }

        // --- handle read request ---
        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        // drain writes that arrived while waiting
        while (pio_try_get_write(&waddr, &wdata))
        {
            if      (waddr >= 0x5000u && waddr <= 0x57FFu) bank_registers[0] = wdata;
            else if (waddr >= 0x7000u && waddr <= 0x77FFu) bank_registers[1] = wdata;
            else if (waddr >= 0x9000u && waddr <= 0x97FFu) bank_registers[2] = wdata;
            else if (waddr >= 0xB000u && waddr <= 0xB7FFu) bank_registers[3] = wdata;
            SCC_write(&scc_instance, waddr, wdata);
        }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            // Check if address falls in active SCC register read region.
            bool is_scc_read = false;
            if (scc_instance.active)
            {
                uint32_t scc_reg_start = scc_instance.base_adr + 0x800u;
                if (addr >= scc_reg_start && addr <= (scc_reg_start + 0xFFu))
                    is_scc_read = true;
            }
            // SCC+ mode register at 0xBFFE-0xBFFF
            if (scc_type == SCC_ENHANCED && (addr & 0xFFFEu) == 0xBFFEu)
                is_scc_read = true;

            if (is_scc_read)
            {
                data = (uint8_t)SCC_read(&scc_instance, addr);
            }
            else
            {
                uint32_t rel = ((uint32_t)bank_registers[(addr - 0x4000u) >> 13] * 0x2000u)
                             + (addr & 0x1FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_konami - Load a Konami (without SCC) ROM into the MSX directly from the pico flash
// The Konami (without SCC) ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
//  Bank 1: 4000h - 5FFFh, Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the addresses to change banks are:
//	Bank 1: <none>, Bank 2: 6000h - 67FFh (6000h used), Bank 3: 8000h - 87FFh (8000h used), Bank 4: A000h - A7FFh (A000h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_konami_write);
}

// loadrom_ascii8 - Load an ASCII8 ROM into the MSX directly from the pico flash
// The ASCII8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the address to change banks are:
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 6800h - 6FFFh (6800h used), Bank 3: 7000h - 77FFh (7000h used), Bank 4: 7800h - 7FFFh (7800h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_ascii8_write);
}

// loadrom_ascii16 - Load an ASCII16 ROM into the MSX directly from the pico flash
// The ASCII16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 16Kb, the memory banks are:
// Bank 1: 4000h - 7FFFh , Bank 2: 8000h - BFFFh
// And the address to change banks are:
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 7000h - 77FFh (7000h and 77FFh used)
void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[2] = {0, 1};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();

    bank8_ctx_t ctx = { .bank_regs = bank_registers };

    while (true)
    {
        pio_drain_writes(handle_ascii16_write, &ctx);

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(handle_ascii16_write, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank = (addr >> 15) & 1;
            uint32_t rel = ((uint32_t)bank_registers[bank] << 14) + (addr & 0x3FFFu);
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}


void __no_inline_not_in_flash_func(loadrom_nextor_sd_io)(uint32_t offset)
{
    //runs the IO code in the second core
    multicore_launch_core1(nextor_sd_io);    // Launch core 1

    //Test copying to RAM to check performance gains
    gpio_init(PIN_WAIT); // Init wait signal pin
    gpio_set_dir(PIN_WAIT, GPIO_OUT); // Set the WAIT signal as output
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    if (!rom_data_in_ram) {
        memset(rom_sram, 0, 131072); // Clear the SRAM buffer
        memcpy(rom_sram, rom_data + offset, 131072); //for 32KB ROMs we start at 0x4000
    }
    gpio_put(PIN_WAIT, 1); // Lets go!

    // Using PIO
    rom_cached_size = 131072; // Full Nextor ROM cached
    msx_pio_bus_init();

    uint8_t bank_registers[2] = {0, 1}; // Initial banks 0 and 1 mapped
    bank8_ctx_t ctx = { .bank_regs = bank_registers };

    while (true)
    {
        pio_drain_writes(handle_ascii16_write, &ctx);

        if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read)) {
             tight_loop_contents();
             continue;
        }

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(handle_ascii16_write, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank = (addr >> 15) & 1;
            uint32_t rel = ((uint32_t)bank_registers[bank] << 14) + (addr & 0x3FFFu);
            data = rom_sram[rel]; // Direct read since we know it's cached
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_neo8 - Load an NEO8 ROM into the MSX directly from the pico flash
// The NEO8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 8 KB
// Segment switching addresses:
// Bank 0: 0000h~1FFFh, Bank 1: 2000h~3FFFh, Bank 2: 4000h~5FFFh, Bank 3: 6000h~7FFFh, Bank 4: 8000h~9FFFh, Bank 5: A000h~BFFFh
// Switching address: 
// 5000h (mirror at 1000h, 9000h and D000h), 
// 5800h (mirror at 1800h, 9800h and D800h), 
// 6000h (mirror at 2000h, A000h and E000h), 
// 6800h (mirror at 2800h, A800h and E800h), 
// 7000h (mirror at 3000h, B000h and F000h), 
// 7800h (mirror at 3800h, B800h and F800h)
void __no_inline_not_in_flash_func(loadrom_neo8)(uint32_t offset)
{
    uint16_t bank_registers[6] = {0};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, false, 0u, &rom_base, &available_length);

    msx_pio_bus_init();

    bank16_ctx_t ctx = { .bank_regs = bank_registers };

    while (true)
    {
        pio_drain_writes(handle_neo8_write, &ctx);

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(handle_neo8_write, &ctx);

        bool in_window = (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank_index = addr >> 13;
            if (bank_index < 6u)
            {
                uint32_t segment = bank_registers[bank_index] & 0x0FFFu;
                uint32_t rel = (segment << 13) + (addr & 0x1FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_neo16 - Load an NEO16 ROM into the MSX directly from the pico flash
// The NEO16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 16 KB
// Segment switching addresses:
// Bank 0: 0000h~3FFFh, Bank 1: 4000h~7FFFh, Bank 2: 8000h~BFFFh
// Switching address:
// 5000h (mirror at 1000h, 9000h and D000h),
// 6000h (mirror at 2000h, A000h and E000h),
// 7000h (mirror at 3000h, B000h and F000h)
void __no_inline_not_in_flash_func(loadrom_neo16)(uint32_t offset)
{
    uint16_t bank_registers[3] = {0};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, false, 0u, &rom_base, &available_length);

    msx_pio_bus_init();

    bank16_ctx_t ctx = { .bank_regs = bank_registers };

    while (true)
    {
        pio_drain_writes(handle_neo16_write, &ctx);

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(handle_neo16_write, &ctx);

        bool in_window = (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank_index = addr >> 14;
            if (bank_index < 3u)
            {
                uint32_t segment = bank_registers[bank_index] & 0x0FFFu;
                uint32_t rel = (segment << 14) + (addr & 0x3FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// Bank cache infrastructure for mapper-aware LRU caching
// Used by ASCII16-X to handle ROMs larger than the 128KB rom_sram cache.
// -----------------------------------------------------------------------
#define BANK_CACHE_MAX_SLOTS 16
#define BANK_CACHE_MAX_PINS   6
#define BANK_EMPTY           0xFFFFu

typedef struct {
    uint16_t      slot_bank[BANK_CACHE_MAX_SLOTS];
    uint8_t       slot_lru[BANK_CACHE_MAX_SLOTS];
    int8_t        page_slot[BANK_CACHE_MAX_PINS];
    const uint8_t *flash_base;
    uint32_t      rom_length;
    uint8_t       num_slots;
    uint8_t       num_pins;
    uint16_t      slot_size;
    uint8_t       slot_shift;
} bank_cache_t;

static inline void __not_in_flash_func(bcache_init)(
    bank_cache_t *c, uint16_t slot_size, uint8_t num_pins,
    const uint8_t *flash_base, uint32_t rom_length)
{
    uint8_t num_slots = (uint8_t)(CACHE_SIZE / slot_size);
    if (num_slots > BANK_CACHE_MAX_SLOTS) num_slots = BANK_CACHE_MAX_SLOTS;
    c->num_slots  = num_slots;
    c->num_pins   = num_pins;
    c->slot_size  = slot_size;
    c->slot_shift = (slot_size == 8192u) ? 13 : 14;
    c->flash_base = flash_base;
    c->rom_length = rom_length;
    for (int i = 0; i < BANK_CACHE_MAX_SLOTS; i++)
    {
        c->slot_bank[i] = BANK_EMPTY;
        c->slot_lru[i]  = (uint8_t)i;
    }
    for (int i = 0; i < BANK_CACHE_MAX_PINS; i++)
        c->page_slot[i] = -1;
}

static inline void __not_in_flash_func(bcache_touch)(bank_cache_t *c, int8_t slot)
{
    uint8_t prev = c->slot_lru[slot];
    if (prev == 0) return;
    uint8_t n = c->num_slots;
    for (uint8_t i = 0; i < n; i++)
        if (c->slot_lru[i] < prev) c->slot_lru[i]++;
    c->slot_lru[slot] = 0;
}

static inline int8_t __not_in_flash_func(bcache_find)(bank_cache_t *c, uint16_t bank)
{
    uint8_t n = c->num_slots;
    for (uint8_t i = 0; i < n; i++)
        if (c->slot_bank[i] == bank) return (int8_t)i;
    return -1;
}

static inline int8_t __not_in_flash_func(bcache_evict)(bank_cache_t *c)
{
    int8_t best = -1;
    uint8_t best_lru = 0;
    uint8_t n = c->num_slots;
    uint8_t np = c->num_pins;
    for (uint8_t i = 0; i < n; i++)
    {
        bool pinned = false;
        for (uint8_t p = 0; p < np; p++)
            if ((int8_t)i == c->page_slot[p]) { pinned = true; break; }
        if (pinned) continue;
        if (best < 0 || c->slot_lru[i] >= best_lru)
        {
            best = (int8_t)i;
            best_lru = c->slot_lru[i];
        }
    }
    return best;
}

static inline int8_t __not_in_flash_func(bcache_ensure)(bank_cache_t *c, uint16_t bank)
{
    if (c->rom_length > 0u)
    {
        uint16_t total_banks = (uint16_t)(c->rom_length >> c->slot_shift);
        if (total_banks > 0u)
            bank = bank % total_banks;
    }

    int8_t slot = bcache_find(c, bank);
    if (slot >= 0) { bcache_touch(c, slot); return slot; }

    slot = bcache_evict(c);
    uint32_t src_off = (uint32_t)(bank & 0x0FFFu) << c->slot_shift;
    uint8_t *dst = &rom_sram[(uint32_t)slot * c->slot_size];
    uint16_t ss = c->slot_size;

    if (c->rom_length == 0u || src_off < c->rom_length)
    {
        uint32_t n = ss;
        if (c->rom_length > 0u && src_off + n > c->rom_length)
            n = c->rom_length - src_off;
        memcpy(dst, c->flash_base + src_off, n);
        if (n < ss) memset(dst + n, 0xFFu, ss - n);
    }
    else
    {
        memset(dst, 0xFFu, ss);
    }

    c->slot_bank[slot] = bank;
    bcache_touch(c, slot);
    return slot;
}

static inline void __not_in_flash_func(bcache_prefill)(bank_cache_t *c)
{
    uint16_t max_banks = c->num_slots;
    if (c->rom_length > 0u)
    {
        uint16_t rom_banks = (uint16_t)((c->rom_length + c->slot_size - 1u) / c->slot_size);
        if (rom_banks < max_banks) max_banks = rom_banks;
    }
    for (uint16_t b = 0; b < max_banks; b++)
        bcache_ensure(c, b);
}

// -----------------------------------------------------------------------
// ASCII16-X mapper (Padial) - 16KB banks at 0x4000-0x7FFF / 0x8000-0xBFFF
// with extended addressing (12-bit bank number) and AMD-compatible flash
// command emulation (program/erase) on the cached SRAM banks.
// Bank-switch writes use Konami-like layout but the high nibble of the
// register address is OR'd into the bank number to allow >256 banks.
// -----------------------------------------------------------------------
typedef enum {
    FLASH_IDLE = 0,
    FLASH_UNLOCK1,
    FLASH_UNLOCK2,
    FLASH_BYTE_PGM,
    FLASH_ERASE_SETUP,
    FLASH_ERASE_UNLOCK1,
    FLASH_ERASE_UNLOCK2,
} flash_cmd_state_t;

typedef struct {
    uint16_t          bank_regs[2];
    bank_cache_t      cache;
    flash_cmd_state_t flash_state;
} ascii16x_state_t;

static inline void __not_in_flash_func(flash_process_write)(
    ascii16x_state_t *st, uint16_t addr, uint8_t data)
{
    uint16_t cmd_addr = addr & 0x0FFFu;

    switch (st->flash_state)
    {
        case FLASH_IDLE:
            if (cmd_addr == 0x0AAAu && data == 0xAAu)
                st->flash_state = FLASH_UNLOCK1;
            else if (data == 0xF0u)
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_UNLOCK1:
            if (cmd_addr == 0x0555u && data == 0x55u)
                st->flash_state = FLASH_UNLOCK2;
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_UNLOCK2:
            if (cmd_addr == 0x0AAAu)
            {
                switch (data)
                {
                    case 0xA0u: st->flash_state = FLASH_BYTE_PGM;    break;
                    case 0x80u: st->flash_state = FLASH_ERASE_SETUP;  break;
                    default:    st->flash_state = FLASH_IDLE;         break;
                }
            }
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_BYTE_PGM:
        {
            uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
            int8_t slot = st->cache.page_slot[page_idx];
            if (slot >= 0)
            {
                uint32_t off = (uint32_t)slot * st->cache.slot_size + (addr & 0x3FFFu);
                rom_sram[off] &= data;
            }
            st->flash_state = FLASH_IDLE;
            break;
        }
        case FLASH_ERASE_SETUP:
            if (cmd_addr == 0x0AAAu && data == 0xAAu)
                st->flash_state = FLASH_ERASE_UNLOCK1;
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_ERASE_UNLOCK1:
            if (cmd_addr == 0x0555u && data == 0x55u)
                st->flash_state = FLASH_ERASE_UNLOCK2;
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_ERASE_UNLOCK2:
            if (data == 0x30u)
            {
                uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
                int8_t slot = st->cache.page_slot[page_idx];
                if (slot >= 0)
                    memset(&rom_sram[(uint32_t)slot * st->cache.slot_size], 0xFFu, st->cache.slot_size);
            }
            st->flash_state = FLASH_IDLE;
            break;
    }
}

static inline void __not_in_flash_func(handle_ascii16x_write_cached)(uint16_t addr, uint8_t data, void *ctx)
{
    ascii16x_state_t *st = (ascii16x_state_t *)ctx;
    flash_process_write(st, addr, data);

    uint8_t high_nibble = (uint8_t)((addr >> 8) & 0x0Fu);
    uint16_t bank = ((uint16_t)high_nibble << 8) | data;
    uint8_t page;

    switch (addr & 0xF000u)
    {
        case 0x2000u: case 0x6000u: case 0xA000u: case 0xE000u: page = 0; break;
        case 0x3000u: case 0x7000u: case 0xB000u: case 0xF000u: page = 1; break;
        default: return;
    }

    st->bank_regs[page] = bank;
    st->cache.page_slot[page] = bcache_ensure(&st->cache, bank);
}

void __no_inline_not_in_flash_func(loadrom_ascii16x)(uint32_t offset, bool cache_enable)
{
    (void)cache_enable;

    const uint8_t *rom_base;
    uint32_t available_length;
    // Don't preload via prepare_rom_source — bcache manages SRAM directly.
    prepare_rom_source(offset, false, 0u, &rom_base, &available_length);

    ascii16x_state_t state;
    memset(&state, 0, sizeof(state));

    bcache_init(&state.cache, 16384u, 2, rom_data + offset, active_rom_size);
    bcache_prefill(&state.cache);

    // Initial bank 0 visible at both windows.
    state.cache.page_slot[0] = bcache_find(&state.cache, 0);
    state.cache.page_slot[1] = state.cache.page_slot[0];

    // bcache reads from rom_sram via slot index, bypassing read_rom_byte.
    rom_cached_size = 0;

    msx_pio_bus_init();

    while (true)
    {
        pio_drain_writes(handle_ascii16x_write_cached, &state);

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(handle_ascii16x_write_cached, &state);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
            int8_t slot = state.cache.page_slot[page_idx];
            if (slot >= 0)
                data = rom_sram[(uint32_t)slot * state.cache.slot_size + (addr & 0x3FFFu)];
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// loadrom_planar64 - 64KB Planar ROM (no mapper)
// Linear 64KB ROM mapped at 0x0000-0xFFFF (typical MSX2 boot games like
// Game Master 2). AB header is at 0x4000.
// -----------------------------------------------------------------------
void __no_inline_not_in_flash_func(loadrom_planar64)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 65536u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        bool in_window = true; // 64KB linear: drive all reads
        uint8_t data = 0xFFu;
        if (available_length == 0u || addr < available_length)
            data = read_rom_byte(rom_base, addr);

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// Manbow2 mapper - Konami SCC banking + AMD AM29F040B flash emulation
// 8KB banks at 0x4000-0x5FFF / 0x6000-0x7FFF / 0x8000-0x9FFF / 0xA000-0xBFFF.
// Bank-switch writes land at 0x?000-0x?7FF for each page (Konami SCC layout).
// The last 64KB of the ROM (flash offset 0x70000-0x7FFFF) is treated as a
// writable sector emulating the AMD AM29F040B flash chip used by Manbow2 to
// persist save data via the standard JEDEC unlock+program/erase sequence.
// -----------------------------------------------------------------------
enum manbow2_flash_state {
    MBW2_READ = 0,
    MBW2_CMD1,
    MBW2_CMD2,
    MBW2_AUTOSELECT,
    MBW2_PROGRAM,
    MBW2_ERASE_CMD1,
    MBW2_ERASE_CMD2,
    MBW2_ERASE_CMD3
};

typedef struct {
    uint8_t bank_regs[4];
    enum manbow2_flash_state state;
    uint8_t *writable_sram;
    uint32_t writable_offset;
    uint32_t writable_size;
} manbow2_ctx_t;

static inline void __not_in_flash_func(handle_manbow2_write)(uint16_t addr, uint8_t data, void *ctx)
{
    manbow2_ctx_t *mb = (manbow2_ctx_t *)ctx;

    if (addr < 0x4000u || addr > 0xBFFFu)
        return;

    uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
    uint32_t flash_addr = (uint32_t)mb->bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

    // Konami SCC-style bank latches at 0x?000-0x?7FF of each 8KB page.
    if ((addr & 0x1800u) == 0x1000u)
        mb->bank_regs[page] = data & 0x3Fu;

    // AMD reset
    if (data == 0xF0u)
    {
        mb->state = MBW2_READ;
        return;
    }

    switch (mb->state)
    {
        case MBW2_READ:
            if ((flash_addr & 0x7FFu) == 0x555u && data == 0xAAu)
                mb->state = MBW2_CMD1;
            break;
        case MBW2_CMD1:
            if ((flash_addr & 0x7FFu) == 0x2AAu && data == 0x55u)
                mb->state = MBW2_CMD2;
            else
                mb->state = MBW2_READ;
            break;
        case MBW2_CMD2:
            if      (data == 0xA0u) mb->state = MBW2_PROGRAM;
            else if (data == 0x90u) mb->state = MBW2_AUTOSELECT;
            else if (data == 0x80u) mb->state = MBW2_ERASE_CMD1;
            else                     mb->state = MBW2_READ;
            break;
        case MBW2_PROGRAM:
            if (flash_addr >= mb->writable_offset &&
                flash_addr <  mb->writable_offset + mb->writable_size)
            {
                uint32_t sram_off = flash_addr - mb->writable_offset;
                mb->writable_sram[sram_off] &= data;
            }
            mb->state = MBW2_READ;
            break;
        case MBW2_ERASE_CMD1:
            if ((flash_addr & 0x7FFu) == 0x555u && data == 0xAAu)
                mb->state = MBW2_ERASE_CMD2;
            else
                mb->state = MBW2_READ;
            break;
        case MBW2_ERASE_CMD2:
            if ((flash_addr & 0x7FFu) == 0x2AAu && data == 0x55u)
                mb->state = MBW2_ERASE_CMD3;
            else
                mb->state = MBW2_READ;
            break;
        case MBW2_ERASE_CMD3:
            if (data == 0x30u)
            {
                if (flash_addr >= mb->writable_offset &&
                    flash_addr <  mb->writable_offset + mb->writable_size)
                    memset(mb->writable_sram, 0xFF, mb->writable_size);
            }
            mb->state = MBW2_READ;
            break;
        case MBW2_AUTOSELECT:
            break;
    }
}

// Common Manbow2 setup: caches the first (CACHE_SIZE - writable_size) bytes of
// the ROM into rom_sram (mirrors multirom's strategy) and reserves the tail of
// rom_sram for the emulated 64KB writable AMD flash sector. Returns rom_base /
// writable_sram / available_length to the caller.
static inline void __not_in_flash_func(loadrom_manbow2_setup)(
    uint32_t offset,
    const uint8_t **rom_base_out,
    uint32_t *available_length_out,
    uint8_t **writable_sram_out,
    uint32_t writable_offset,
    uint32_t writable_size)
{
    uint32_t reduced_cache = CACHE_SIZE - writable_size;

    // Limit prepare_rom_source's caching to `reduced_cache` so the tail of
    // rom_sram is left untouched for the writable flash sector.
    rom_cache_capacity = reduced_cache;
    prepare_rom_source(offset, true, 0u, rom_base_out, available_length_out);
    rom_cache_capacity = CACHE_SIZE; // restore default for next ROM type

    uint8_t *writable_sram = &rom_sram[reduced_cache];

    // Seed the writable AMD flash sector mirror from the ROM payload.
    // rom_data points to flash for flash-resident ROMs and to PSRAM for
    // SD-resident ROMs, so a single source path works for both.
    {
        const uint8_t *src = rom_data + offset;

        if (*available_length_out >= writable_offset + writable_size)
        {
            memcpy(writable_sram, src + writable_offset, writable_size);
        }
        else if (*available_length_out > writable_offset)
        {
            uint32_t partial = *available_length_out - writable_offset;
            memcpy(writable_sram, src + writable_offset, partial);
            memset(writable_sram + partial, 0xFF, writable_size - partial);
        }
        else
        {
            memset(writable_sram, 0xFF, writable_size);
        }
    }

    *writable_sram_out = writable_sram;
}

// loadrom_manbow2 - Manbow2 (Konami SCC-banked + AMD flash) without SCC audio
void __no_inline_not_in_flash_func(loadrom_manbow2)(uint32_t offset, bool cache_enable)
{
    (void)cache_enable;

    static const uint32_t WRITABLE_SECTOR_SIZE   = 0x10000u;  // 64KB
    static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u;  // 448KB

    const uint8_t *rom_base;
    uint32_t available_length;
    uint8_t *writable_sram;
    loadrom_manbow2_setup(offset, &rom_base, &available_length, &writable_sram,
                           WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);

    manbow2_ctx_t mb = {
        .bank_regs       = {0, 1, 2, 3},
        .state           = MBW2_READ,
        .writable_sram   = writable_sram,
        .writable_offset = WRITABLE_SECTOR_OFFSET,
        .writable_size   = WRITABLE_SECTOR_SIZE,
    };

    msx_pio_bus_init();

    while (true)
    {
        // Busy-poll write FIFO + read FIFO together. Manbow2's AMD flash
        // command sequences burst multiple writes (0x555=0xAA, 0x2AA=0x55,
        // 0xA0/0x80, then data) without intervening reads — using a blocking
        // read here would let the write FIFO overflow and stall the bus.
        uint16_t addr;
        while (true)
        {
            uint16_t waddr; uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
                handle_manbow2_write(waddr, wdata, &mb);
            if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }
        }

        { uint16_t waddr; uint8_t wdata;
          while (pio_try_get_write(&waddr, &wdata))
              handle_manbow2_write(waddr, wdata, &mb); }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
            uint32_t rel = (uint32_t)mb.bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

            if (mb.state == MBW2_AUTOSELECT)
            {
                // AM29F040B device IDs (manufacturer 0x01 AMD, device 0xA4)
                uint32_t id_addr = rel & 0x03u;
                if      (id_addr == 0x00u) data = 0x01u;
                else if (id_addr == 0x01u) data = 0xA4u;
                else if (id_addr == 0x02u)
                    data = (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size) ? 0x00u : 0x01u;
                else                       data = 0x00u;
            }
            else
            {
                if (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size)
                    data = writable_sram[rel - mb.writable_offset];
                else if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel); // SRAM cache up to rom_cached_size, then flash
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_manbow2_scc - Manbow2 mapper with SCC/SCC+ audio emulation
void __no_inline_not_in_flash_func(loadrom_manbow2_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type)
{
    (void)cache_enable;

    static const uint32_t WRITABLE_SECTOR_SIZE   = 0x10000u;
    static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u;

    const uint8_t *rom_base;
    uint32_t available_length;
    uint8_t *writable_sram;
    loadrom_manbow2_setup(offset, &rom_base, &available_length, &writable_sram,
                           WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);

    manbow2_ctx_t mb = {
        .bank_regs       = {0, 1, 2, 3},
        .state           = MBW2_READ,
        .writable_sram   = writable_sram,
        .writable_offset = WRITABLE_SECTOR_OFFSET,
        .writable_size   = WRITABLE_SECTOR_SIZE,
    };

    // Hold WAIT to freeze Z80 during audio subsystem init.
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    memset(&scc_instance, 0, sizeof(SCC));
    scc_instance.clk  = SCC_CLOCK;
    scc_instance.rate = SCC_SAMPLE_RATE;
    SCC_set_quality(&scc_instance, 1);
    scc_instance.type = scc_type;
    SCC_reset(&scc_instance);

    i2s_audio_init_scc();
    multicore_launch_core1(core1_scc_audio);

    msx_pio_bus_init();

    while (true)
    {
        // Busy-poll write FIFO + read FIFO together (see loadrom_manbow2 for
        // rationale — AMD flash command sequences burst writes without reads).
        uint16_t addr;
        while (true)
        {
            uint16_t waddr; uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                handle_manbow2_write(waddr, wdata, &mb);
                SCC_write(&scc_instance, waddr, wdata);
            }
            if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }
        }

        { uint16_t waddr; uint8_t wdata;
          while (pio_try_get_write(&waddr, &wdata))
          {
              handle_manbow2_write(waddr, wdata, &mb);
              SCC_write(&scc_instance, waddr, wdata);
          } }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            // SCC register read window
            bool is_scc_read = false;
            if (scc_instance.active)
            {
                uint32_t scc_reg_start = scc_instance.base_adr + 0x800u;
                if (addr >= scc_reg_start && addr <= (scc_reg_start + 0xFFu))
                    is_scc_read = true;
            }
            if (scc_type == SCC_ENHANCED && (addr & 0xFFFEu) == 0xBFFEu)
                is_scc_read = true;

            if (is_scc_read)
            {
                data = (uint8_t)SCC_read(&scc_instance, addr);
            }
            else
            {
                uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
                uint32_t rel = (uint32_t)mb.bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

                if (mb.state == MBW2_AUTOSELECT)
                {
                    uint32_t id_addr = rel & 0x03u;
                    if      (id_addr == 0x00u) data = 0x01u;
                    else if (id_addr == 0x01u) data = 0xA4u;
                    else if (id_addr == 0x02u)
                        data = (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size) ? 0x00u : 0x01u;
                    else                       data = 0x00u;
                }
                else
                {
                    if (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size)
                        data = writable_sram[rel - mb.writable_offset];
                    else if (available_length == 0u || rel < available_length)
                        data = read_rom_byte(rom_base, rel);
                }
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}


// Main function running on core 0
int __no_inline_not_in_flash_func(main)()
{
    qmi_hw->m[0].timing = 0x40000202; // Set the QMI timing for the MSX bus
    set_sys_clock_khz(210000, true);     // Set system clock to 210Mhz

    stdio_init_all();     // Initialize stdio
    setup_gpio();     // Initialize GPIO

    int rom_index = loadrom_msx_menu(0x0000); //load the first 32KB ROM into the MSX (The MSX PICOVERSE MENU)

    mp3_stop_core1();       // Signal Core 1 MP3 loop to exit
    multicore_reset_core1();
    mp3_deinit();           // Release I2S/DMA/PIO resources claimed by MP3 player

    ROMRecord const *selected = &records[rom_index];
    active_rom_size = selected->Size;

    uint32_t rom_offset = selected->Offset;
    rom_data = flash_rom;
    rom_data_in_ram = false;

    bool is_sd_rom = (selected->Mapper & SOURCE_SD_FLAG) != 0;
    if (is_sd_rom) {
        if (!load_rom_from_sd((uint16_t)rom_index, (uint32_t)selected->Size)) {
            printf("Debug: Failed to load ROM from SD card\n");
            while (true) { tight_loop_contents(); }
        }
        rom_data = sd_rom_region.ptr;
        rom_data_in_ram = true;
        rom_offset = 0;
    }

    // Cache the leading window into rom_sram for both flash- and PSRAM-resident
    // ROMs (SD ROMs are staged to PSRAM, so caching them into SRAM mirrors the
    // flash path and keeps the mapper hot loop fast).
    bool cache_enable = true;
    uint8_t mapper = (uint8_t)(selected->Mapper & ~(SOURCE_SD_FLAG | OVERRIDE_FLAG | FOLDER_FLAG | MP3_FLAG));
    uint8_t audio_sel = ctrl_audio_selection;

    // Load the selected ROM into the MSX according to the mapper
    switch (mapper) {
       
        case 1:
        case 2:
            loadrom_plain32(rom_offset, cache_enable);
            break;
        case 3:
            if (audio_sel == 1 || audio_sel == 2)
                loadrom_konamiscc_scc(rom_offset, cache_enable, audio_sel == 2 ? SCC_ENHANCED : SCC_STANDARD);
            else
                loadrom_konamiscc(rom_offset, cache_enable);
            break;
        case 4:
            loadrom_linear48(rom_offset, cache_enable);
            break;
        case 5:
            loadrom_ascii8(rom_offset, cache_enable); 
            break;
        case 6:
            loadrom_ascii16(rom_offset, cache_enable); 
            break;
        case 7:
            loadrom_konami(rom_offset, cache_enable); 
            break;
        case 8:
            loadrom_neo8(rom_offset); 
            break;
        case 9:
            loadrom_neo16(rom_offset); 
            break;
        case 10:
            loadrom_nextor_sd_io(rom_offset);
            break;
        case 12:
            loadrom_ascii16x(rom_offset, cache_enable);
            break;
        case 13:
            loadrom_planar64(rom_offset, cache_enable);
            break;
        case 14:
            if (audio_sel == 1 || audio_sel == 2)
                loadrom_manbow2_scc(rom_offset, cache_enable, audio_sel == 2 ? SCC_ENHANCED : SCC_STANDARD);
            else
                loadrom_manbow2(rom_offset, cache_enable);
            break;
        default:
                printf("Debug: Unsupported ROM mapper: %d\n", mapper);
            break;
    }
    
}
