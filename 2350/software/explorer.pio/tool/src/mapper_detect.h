// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// mapper_detect.h - Shared SHA1 + heuristic ROM mapper detection.
//
// This header is shared between the Explorer PC build tool (which loads ROMs
// from disk) and the Explorer Pico firmware (which scans ROMs from microSD).
// It implements the same openMSX-derived algorithm used by the multirom.pio
// tool so that detection results stay consistent across both code paths.
//
// The detector exposes a callback-based streaming API so callers can feed the
// ROM bytes from any source (file pointer, FatFS file handle, in-memory
// buffer). The algorithm performs a single linear pass that simultaneously:
//   * accumulates SHA-1 over the full ROM payload;
//   * captures key header bytes (offsets 0x00, 0x10, 0x4000, 0x28000);
//   * counts mapper-write opcode patterns (`ld (nnnn),a` -> 0x32 ...);
//   * counts raw 16-bit immediate patterns (0x77FF / 0x6800 / 0x7800).
// After the pass it consults the openMSX SHA1 database first, then falls back
// to header-based and heuristic rules — matching the multirom.pio tool.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MAPPER_DETECT_H
#define MAPPER_DETECT_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "sha1.h"
#include "romdb.h"

#ifndef MAPPER_DETECT_MIN_ROM_SIZE
#define MAPPER_DETECT_MIN_ROM_SIZE  8192u
#endif

#ifndef MAPPER_DETECT_MAX_ROM_SIZE
#define MAPPER_DETECT_MAX_ROM_SIZE  (15u * 1024u * 1024u)
#endif

// Mapper byte values returned by the detector. Must stay in sync with the
// firmware's switch dispatch and the tool's MAPPER_DESCRIPTIONS table.
#define MAPPER_DETECT_PLAIN16        1u
#define MAPPER_DETECT_PLAIN32        2u
#define MAPPER_DETECT_KONAMI_SCC     3u
#define MAPPER_DETECT_PLANAR48       4u
#define MAPPER_DETECT_ASCII8         5u
#define MAPPER_DETECT_ASCII16        6u
#define MAPPER_DETECT_KONAMI         7u
#define MAPPER_DETECT_NEO8           8u
#define MAPPER_DETECT_NEO16          9u
#define MAPPER_DETECT_ASCII16X      12u
#define MAPPER_DETECT_PLANAR64      13u
#define MAPPER_DETECT_MANBOW2       14u

// Streaming reader callback. Reads up to `len` bytes from absolute `offset`
// into `buf`. Returns the number of bytes actually copied (`< len` only on
// EOF or error). A return value of 0 before reaching `size` aborts detection.
typedef uint32_t (*mapper_detect_reader_t)(void *user, uint32_t offset,
                                           void *buf, uint32_t len);

// Internal scoring helper — kept as static inline so the table stays in this
// header. Mirrors openMSX guessRomType() unit-weight scoring.
static inline void mapper_detect_score_addr(uint16_t addr,
    unsigned int *konami,
    unsigned int *konami_scc,
    unsigned int *ascii8,
    unsigned int *ascii16)
{
    switch (addr) {
        case 0x4000: case 0x8000: case 0xA000:
            (*konami)++;
            break;
        case 0x5000: case 0x9000: case 0xB000:
            (*konami_scc)++;
            break;
        case 0x6800: case 0x7800:
            (*ascii8)++;
            break;
        case 0x77FF:
            (*ascii16)++;
            break;
        case 0x6000:
            (*konami)++; (*ascii8)++; (*ascii16)++;
            break;
        case 0x7000:
            (*konami_scc)++; (*ascii8)++; (*ascii16)++;
            break;
        default:
            break;
    }
}

// Update raw-pattern counters used by the no-mapper-writes fallback.
static inline void mapper_detect_count_raw(uint16_t raw,
    unsigned int *raw_77ff,
    unsigned int *raw_6800,
    unsigned int *raw_7800)
{
    if (raw == 0x77FFu) (*raw_77ff)++;
    else if (raw == 0x6800u) (*raw_6800)++;
    else if (raw == 0x7800u) (*raw_7800)++;
}

// Run the full detector against a streaming reader. `size` is the total ROM
// length in bytes. Returns the mapper byte (1..14, never 10/11/15..18 which
// are reserved for system slots) or 0 if the format could not be identified.
static inline uint8_t mapper_detect_stream(uint32_t size,
                                           mapper_detect_reader_t reader,
                                           void *user)
{
    if (size > MAPPER_DETECT_MAX_ROM_SIZE || size < MAPPER_DETECT_MIN_ROM_SIZE) {
        return 0;
    }
    if (reader == 0) {
        return 0;
    }

    sha1_ctx sctx;
    sha1_init(&sctx);

    unsigned int konami_score = 0;
    unsigned int konami_scc_score = 0;
    unsigned int ascii8_score = 0;
    unsigned int ascii16_score = 0;
    unsigned int raw_77ff = 0;
    unsigned int raw_6800 = 0;
    unsigned int raw_7800 = 0;

    // Captured header bytes used by post-pass rules.
    uint8_t  hdr0[2]      = {0, 0};       // bytes at [0x0000..0x0001]
    uint8_t  hdr16[8]     = {0};          // bytes at [0x0010..0x0017]
    uint8_t  hdr_4000[2]  = {0, 0};       // bytes at [0x4000..0x4001]
    uint8_t  hdr_28000[8] = {0};          // bytes at [0x28000..0x28007]

    bool have_hdr0     = false;
    bool have_hdr16    = false;
    bool have_hdr_4000 = false;
    bool have_hdr_28000 = false;

    // Sliding-window state for cross-chunk opcode scans.
    uint8_t prev_tail[2] = {0, 0};
    uint8_t prev_tail_n  = 0;     // 0/1/2 bytes carried over from previous chunk

    uint8_t chunk[1024];
    uint32_t off = 0;

    while (off < size) {
        uint32_t want = size - off;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        uint32_t got = reader(user, off, chunk, want);
        if (got == 0) {
            // Reader signalled error/EOF before reaching size.
            return 0;
        }
        if (got > want) got = want;

        sha1_update(&sctx, chunk, got);

        // ---- Capture header bytes that fall inside this chunk window ----
        // [0x0000..0x0001]
        if (off == 0u && got >= 2u) {
            hdr0[0] = chunk[0]; hdr0[1] = chunk[1];
            have_hdr0 = true;
        }
        // [0x0010..0x0017]
        if (off <= 0x0010u && off + got > 0x0010u) {
            uint32_t local = 0x0010u - off;
            uint32_t avail = got - local;
            uint32_t copy = avail < 8u ? avail : 8u;
            memcpy(hdr16, chunk + local, copy);
            // Mark complete only when all 8 bytes are captured.
            if (off + got >= 0x0010u + 8u) have_hdr16 = true;
        }
        // [0x4000..0x4001]
        if (off <= 0x4000u && off + got > 0x4000u) {
            uint32_t local = 0x4000u - off;
            uint32_t avail = got - local;
            if (avail >= 2u) {
                hdr_4000[0] = chunk[local];
                hdr_4000[1] = chunk[local + 1u];
                have_hdr_4000 = true;
            } else {
                hdr_4000[0] = chunk[local];
                // We'll catch the second byte at the next chunk boundary.
            }
        } else if (off <= 0x4001u && off + got > 0x4001u && !have_hdr_4000) {
            uint32_t local = 0x4001u - off;
            hdr_4000[1] = chunk[local];
            have_hdr_4000 = true;
        }
        // [0x28000..0x28007]
        if (off <= 0x28000u && off + got > 0x28000u) {
            uint32_t local = 0x28000u - off;
            uint32_t avail = got - local;
            uint32_t copy = avail < 8u ? avail : 8u;
            memcpy(hdr_28000, chunk + local, copy);
            if (off + got >= 0x28000u + 8u) have_hdr_28000 = true;
        }

        // ---- Cross-chunk window scoring (handles last 1-2 bytes of prior chunk) ----
        if (prev_tail_n > 0u && got >= 1u) {
            // Boundary windows of size 3: anchored at offsets (off - prev_tail_n) ..
            // up to but not including the start of the body scan inside this chunk.
            // Because we copy at most 2 carry-over bytes the boundary scan covers
            // at most two windows.
            if (prev_tail_n == 2u) {
                // Window: prev_tail[0], prev_tail[1], chunk[0]
                if (prev_tail[0] == 0x32u) {
                    uint16_t addr = (uint16_t)prev_tail[1] |
                                    ((uint16_t)chunk[0] << 8);
                    mapper_detect_score_addr(addr,
                        &konami_score, &konami_scc_score,
                        &ascii8_score, &ascii16_score);
                }
                // Raw pair: prev_tail[1], chunk[0]
                {
                    uint16_t raw = (uint16_t)prev_tail[1] |
                                   ((uint16_t)chunk[0] << 8);
                    mapper_detect_count_raw(raw,
                        &raw_77ff, &raw_6800, &raw_7800);
                }
                if (got >= 2u) {
                    if (prev_tail[1] == 0x32u) {
                        uint16_t addr = (uint16_t)chunk[0] |
                                        ((uint16_t)chunk[1] << 8);
                        mapper_detect_score_addr(addr,
                            &konami_score, &konami_scc_score,
                            &ascii8_score, &ascii16_score);
                    }
                }
            } else if (prev_tail_n == 1u) {
                // Window: prev_tail[0], chunk[0], chunk[1]
                if (got >= 2u && prev_tail[0] == 0x32u) {
                    uint16_t addr = (uint16_t)chunk[0] |
                                    ((uint16_t)chunk[1] << 8);
                    mapper_detect_score_addr(addr,
                        &konami_score, &konami_scc_score,
                        &ascii8_score, &ascii16_score);
                }
                // Raw pair: prev_tail[0], chunk[0]
                {
                    uint16_t raw = (uint16_t)prev_tail[0] |
                                   ((uint16_t)chunk[0] << 8);
                    mapper_detect_count_raw(raw,
                        &raw_77ff, &raw_6800, &raw_7800);
                }
            }
        }

        // ---- Body scan within this chunk ----
        if (got >= 3u) {
            uint32_t end = got - 2u;     // last `i` accessing chunk[i+2]
            for (uint32_t i = 0u; i < end; ++i) {
                if (chunk[i] == 0x32u) {
                    uint16_t addr = (uint16_t)chunk[i + 1u] |
                                    ((uint16_t)chunk[i + 2u] << 8);
                    mapper_detect_score_addr(addr,
                        &konami_score, &konami_scc_score,
                        &ascii8_score, &ascii16_score);
                }
            }
        }
        if (got >= 2u) {
            uint32_t end = got - 1u;     // last `i` accessing chunk[i+1]
            for (uint32_t i = 0u; i < end; ++i) {
                uint16_t raw = (uint16_t)chunk[i] |
                               ((uint16_t)chunk[i + 1u] << 8);
                mapper_detect_count_raw(raw,
                    &raw_77ff, &raw_6800, &raw_7800);
            }
        }

        // ---- Carry the trailing 2 bytes for the next iteration ----
        if (got >= 2u) {
            prev_tail[0] = chunk[got - 2u];
            prev_tail[1] = chunk[got - 1u];
            prev_tail_n  = 2u;
        } else if (got == 1u) {
            // Shift any existing tail byte left and append the single new one.
            if (prev_tail_n == 2u) {
                prev_tail[0] = prev_tail[1];
            } else if (prev_tail_n == 0u) {
                // No previous tail; we need at least 1 byte slot.
                prev_tail[0] = 0u;
            }
            prev_tail[1] = chunk[0];
            prev_tail_n  = 2u;
        }

        off += got;
    }

    uint8_t sha1[20];
    sha1_final(&sctx, sha1);

    // ---- Step 1: openMSX SHA1 database lookup ----
    uint8_t db_type = romdb_lookup(sha1);
    if (db_type) {
        return db_type;
    }

    // openMSX-style: subtract 1 from ASCII8 if non-zero. Applied here so the
    // post-pass rules use the corrected scores without affecting raw counts.
    if (ascii8_score) {
        ascii8_score--;
    }

    // ---- Step 2: header-driven rules ----
    bool ab0    = have_hdr0    && (hdr0[0]    == 'A' && hdr0[1]    == 'B');
    bool ab4000 = have_hdr_4000 && (hdr_4000[0] == 'A' && hdr_4000[1] == 'B');

    if (ab0 && size == 16384u) {
        return MAPPER_DETECT_PLAIN16;
    }

    if (ab0 && size <= 32768u) {
        if (ab4000) return MAPPER_DETECT_PLANAR48; // Planar32 layout
        return MAPPER_DETECT_PLAIN32;
    }

    if (ab0 && have_hdr16) {
        // ASCII16-X signature: "ASCII16X" at offset 0x10
        static const char ascii16x_sig[] = "ASCII16X";
        if (memcmp(hdr16, ascii16x_sig, 8) == 0) {
            return MAPPER_DETECT_ASCII16X;
        }
        // NEO-8 / NEO-16 signatures at offset 0x10
        static const char neo8_sig[]  = "ROM_NEO8";
        static const char neo16_sig[] = "ROM_NE16";
        if (memcmp(hdr16, neo8_sig, 8) == 0)  return MAPPER_DETECT_NEO8;
        if (memcmp(hdr16, neo16_sig, 8) == 0) return MAPPER_DETECT_NEO16;
    }

    // Manbow2: 512KB ROM with "AB" header and "Manbow 2" string at 0x28000.
    if (size == 524288u && ab0 && have_hdr_28000 &&
        memcmp(hdr_28000, "Manbow 2", 8) == 0) {
        return MAPPER_DETECT_MANBOW2;
    }

    // Planar48: AB at 0x4000 and total ROM <= 48KB (with optional 32KB).
    if (ab4000 && size <= 49152u) {
        return MAPPER_DETECT_PLANAR48;
    }

    // 64KB planar ROMs may only expose AB at 0x4000.
    if (size == 65536u && ab4000) {
        return MAPPER_DETECT_PLANAR64;
    }

    // ---- Step 3: heuristic scoring on larger ROMs ----
    if (size > 32768u) {
        struct { unsigned int score; uint8_t type; } cands[4] = {
            { konami_scc_score, MAPPER_DETECT_KONAMI_SCC },
            { konami_score,     MAPPER_DETECT_KONAMI     },
            { ascii8_score,     MAPPER_DETECT_ASCII8     },
            { ascii16_score,    MAPPER_DETECT_ASCII16    }
        };
        uint8_t best_type = 0;
        unsigned int best_score = 0;
        for (int c = 0; c < 4; ++c) {
            if (cands[c].score && cands[c].score >= best_score) {
                best_score = cands[c].score;
                best_type = cands[c].type;
            }
        }
        if (best_type) return best_type;

        // Fallback: no mapper writes detected — disambiguate based on raw
        // 16-bit immediate counts and the observed AB headers.
        if (konami_score == 0u && konami_scc_score == 0u &&
            ascii8_score == 0u && ascii16_score == 0u)
        {
            if (size == 65536u && (ab0 || ab4000)) {
                return MAPPER_DETECT_PLANAR64;
            }
            if (size > 65536u && ab0 && ((size % 16384u) == 0u)) {
                return (raw_77ff > (raw_6800 + raw_7800))
                       ? MAPPER_DETECT_ASCII16
                       : MAPPER_DETECT_ASCII8;
            }
        }
    }

    return 0;
}

// Convenience wrapper: detect from an in-memory buffer (whole ROM).
typedef struct {
    const uint8_t *data;
    uint32_t       size;
} mapper_detect_membuf_t;

static inline uint32_t mapper_detect_membuf_reader(void *user, uint32_t offset,
                                                   void *buf, uint32_t len)
{
    mapper_detect_membuf_t *src = (mapper_detect_membuf_t *)user;
    if (offset >= src->size) return 0u;
    uint32_t avail = src->size - offset;
    if (len > avail) len = avail;
    memcpy(buf, src->data + offset, len);
    return len;
}

static inline uint8_t mapper_detect_buffer(const uint8_t *data, uint32_t size)
{
    mapper_detect_membuf_t mb = { data, size };
    return mapper_detect_stream(size, mapper_detect_membuf_reader, &mb);
}

#endif // MAPPER_DETECT_H
