# MSX PicoVerse 2040 — MegaROM Mapper Reference

This document describes all ROM mapper types supported by the **loadrom.pio** firmware (`2040/software/loadrom.pio`), including their memory layout, bank-switching behaviour, and caching strategies.

## Table of Contents

- [MSX PicoVerse 2040 — MegaROM Mapper Reference](#msx-picoverse-2040--megarom-mapper-reference)
  - [Table of Contents](#table-of-contents)
  - [Architecture Overview](#architecture-overview)
    - [PIO Bus Engine](#pio-bus-engine)
    - [Memory Layout and SRAM Pool](#memory-layout-and-sram-pool)
    - [Read/Write Flow](#readwrite-flow)
    - [Caching Infrastructure](#caching-infrastructure)
      - [Strategy 1 — Linear DMA Cache (`prepare_rom_source`)](#strategy-1--linear-dma-cache-prepare_rom_source)
      - [Strategy 2 — Mapper-Aware LRU Cache (ASCII16-X, NEO8, NEO16)](#strategy-2--mapper-aware-lru-cache-ascii16-x-neo8-neo16)
  - [Supported Mapper Types](#supported-mapper-types)
    - [Type 1/2 — Planar 16KB / 32KB (no mapper)](#type-12--planar-16kb--32kb-no-mapper)
    - [Type 3 — Konami SCC](#type-3--konami-scc)
    - [Type 4 — Planar 48KB (no mapper)](#type-4--planar-48kb-no-mapper)
    - [Type 5 — ASCII8](#type-5--ascii8)
    - [Type 6 — ASCII16](#type-6--ascii16)
    - [Type 7 — Konami (without SCC)](#type-7--konami-without-scc)
    - [Type 8 — NEO8](#type-8--neo8)
    - [Type 9 — NEO16](#type-9--neo16)
    - [Type 10 — Sunrise IDE Nextor](#type-10--sunrise-ide-nextor)
    - [Type 11 — Sunrise IDE Nextor + Memory Mapper](#type-11--sunrise-ide-nextor--memory-mapper)
    - [Type 12 — ASCII16-X](#type-12--ascii16-x)
    - [Type 13 — Planar 64KB (no mapper)](#type-13--planar-64kb-no-mapper)
    - [Type 14 — Manbow2 (Konami SCC + AMD Flash)](#type-14--manbow2-konami-scc--amd-flash)
  - [Cache Strategy Summary](#cache-strategy-summary)

---

## Architecture Overview

### PIO Bus Engine

The loadrom.pio firmware uses the RP2040's PIO (Programmable I/O) hardware to interface with the MSX bus at the signal level. Two PIO state machines on PIO0 handle all bus interactions:

| State Machine | Role | Description |
|---|---|---|
| **SM0** — `msx_read_responder` | Memory reads | Waits for `/SLTSL=0` then `/RD=0`, immediately asserts `/WAIT=0` to freeze the Z80, captures A0–A15, pushes the address to the RX FIFO, stalls on `pull` until the CPU pushes a data token, drives D0–D7, releases `/WAIT=1`, then waits for `/RD=1` before tri-stating the bus. |
| **SM1** — `msx_write_captor` | Memory writes | Waits for `/SLTSL=0` then `/WR=0`, allows signals to settle (3 PIO cycles), snapshots A0–A15 + D0–D7 into a single 24-bit word, pushes to RX FIFO, waits for `/WR=1`. Uses joined RX FIFO for 8-entry deep buffering. |

For mappers that need I/O port access (Sunrise + Memory Mapper), two additional state machines run on PIO1:

| State Machine | Role | Description |
|---|---|---|
| **SM0** — `msx_io_read_responder` | I/O reads | Same pattern as memory read but triggers on `/IORQ=0` instead of `/SLTSL=0`. |
| **SM1** — `msx_io_write_captor` | I/O writes | Same pattern as memory write but triggers on `/IORQ=0`. |

The system clock is set to **250 MHz**, providing substantial headroom for the CPU to look up data while the Z80 is frozen via `/WAIT`.

### Memory Layout and SRAM Pool

The ROM image is concatenated after the program binary in flash. At runtime, data is served from either SRAM (fast) or flash XIP (slower, requires `/WAIT` hold). The RP2040 provides a **192 KB SRAM pool**, used as a union:

```
┌─────────────────────────────────────────┐
│          sram_pool (192 KB)             │
│                                         │
│  Normal modes:   rom_sram[192KB]        │
│                  Full ROM cache          │
│                                         │
│  Mapper mode:    mapper_ram[192KB]       │
│                  12 pages × 16KB RAM     │
└─────────────────────────────────────────┘
```

Key constants:

| Constant | Value | Description |
|---|---|---|
| `CACHE_SIZE` | 196,608 (192 KB) | Total SRAM available for caching |
| `MAPPER_SIZE` | 196,608 (192 KB) | Memory mapper RAM capacity |
| `MAPPER_PAGES` | 12 | Number of 16 KB mapper pages |
| `MAPPER_PAGE_SIZE` | 16,384 (16 KB) | Size of one mapper page |

### Read/Write Flow

Every mapper follows the same general loop pattern:

1. **Drain writes** — Poll the write captor FIFO and invoke the mapper-specific write handler to update bank registers.
2. **Get read address** — Block on `pio_sm_get_blocking()` for the next address from the read responder SM (the Z80 is frozen via `/WAIT` during this time).
3. **Drain writes again** — Catch any writes that arrived alongside the read.
4. **Compute ROM offset** — Use bank registers to translate the MSX address into a ROM byte offset.
5. **Read data** — Fetch from SRAM cache or flash XIP.
6. **Respond** — Push a 16-bit token to the TX FIFO containing the data byte and pin direction mask.

The token format is:

```
bits [7:0]  = data byte (D0–D7)
bits [15:8] = pindirs mask (0xFF = drive bus, 0x00 = tri-state)
```

### Caching Infrastructure

The firmware provides two distinct caching strategies, chosen per mapper:

#### Strategy 1 — Linear DMA Cache (`prepare_rom_source`)

Used by most mappers. On init, the ROM data is bulk-copied from flash XIP to SRAM using DMA:

- **ROM ≤ 192 KB**: Entire ROM is copied to `rom_sram[]`. All reads come from SRAM. `rom_base` is redirected to `rom_sram`.
- **ROM > 192 KB**: The first 192 KB is cached in `rom_sram[]`. `rom_base` stays pointing to flash. The `read_rom_byte()` function transparently serves from SRAM for offsets within the cached region and falls back to flash XIP for the rest.

```c
static inline uint8_t read_rom_byte(const uint8_t *rom_base, uint32_t rel)
{
    return (rel < rom_cached_size) ? rom_sram[rel] : rom_base[rel];
}
```

During the DMA copy, `/WAIT` is asserted via GPIO to freeze the MSX bus. DMA uses byte-sized transfers (`DMA_SIZE_8`) because `rom_base` may not be 4-byte aligned.

This strategy works well for mappers whose active banks tend to reside in the low addresses of the ROM — the first 192 KB is always fast. Banks beyond that boundary fall back to flash XIP, which requires longer `/WAIT` hold per read.

#### Strategy 2 — Mapper-Aware LRU Cache (ASCII16-X, NEO8, NEO16)

For mappers with large ROM capacity (up to 64 MB for ASCII16-X and NEO16, 32 MB for NEO8), the linear strategy is ineffective because the active working set can span any region of the ROM. The mapper-aware cache (`bank_cache_t`) divides the 192 KB SRAM into fixed-size slots matching the bank size and loads banks on demand using LRU eviction:

| Mapper | Slot Size | Slots | Pinned Pages |
|---|---|---|---|
| ASCII16-X | 16 KB | 12 | 2 |
| NEO8 | 8 KB | 24 | 6 |
| NEO16 | 16 KB | 12 | 3 |

Flash latency is concentrated at bank-switch time (one `memcpy` per cache miss, ≈0.5–1 ms) rather than being spread across every individual byte read. Once cached, all reads are served purely from SRAM with consistent, minimal `/WAIT` hold time.

See [Type 12 — ASCII16-X](#type-12--ascii16-x) for full details on the cache and LRU algorithm.

---

## Supported Mapper Types

### Type 1/2 — Planar 16KB / 32KB (no mapper)

**Function**: `loadrom_planar32()`

**Memory window**: `4000h–BFFFh` (32 KB). 16 KB ROMs occupy `4000h–7FFFh` only.

**Bank switching**: None — pure address-to-data lookup with no registers.

**Address mapping**:
```
MSX address → ROM offset = addr - 4000h
```

**Cache**: Linear DMA cache via `prepare_rom_source()` with `preferred_size = 32768`. The entire ROM fits in SRAM (max 32 KB vs. 192 KB cache). All reads are direct pointer dereferences into `rom_base[]` — no `read_rom_byte()` indirection needed.

**Read path**:
```c
uint8_t data = in_window ? rom_base[addr - 0x4000u] : 0xFFu;
```

**Notes**: Types 1 and 2 share the same implementation. The BIOS auto-detects 16 KB vs 32 KB based on the ROM header.

---

### Type 3 — Konami SCC

**Function**: `loadrom_konamiscc()` → `banked8_loop()`

**Memory window**: `4000h–BFFFh` (32 KB visible), divided into four 8 KB banks.

**Bank layout**:

| Bank | Address Range | Register Window | Default |
|---|---|---|---|
| 0 | `4000h–5FFFh` | `5000h–57FFh` | 0 |
| 1 | `6000h–7FFFh` | `7000h–77FFh` | 1 |
| 2 | `8000h–9FFFh` | `9000h–97FFh` | 2 |
| 3 | `A000h–BFFFh` | `B000h–B7FFh` | 3 |

**Bank register width**: 8-bit. The data byte written to the register window selects the 8 KB bank number.

**Address mapping**:
```
bank_index = (addr - 4000h) >> 13       (0..3)
ROM offset = bank_registers[bank_index] × 2000h + (addr & 1FFFh)
```

**Write handler** (`handle_konamiscc_write`): Decodes the write address and updates the corresponding bank register:
```
5000h–57FFh → regs[0]
7000h–77FFh → regs[1]
9000h–97FFh → regs[2]
B000h–B7FFh → regs[3]
```

**Cache**: Linear DMA cache via `prepare_rom_source()`. ROMs up to 192 KB are fully cached in SRAM. Larger ROMs have partial caching with flash XIP fallback via `read_rom_byte()`.

**Notes**: The SCC sound chip itself is not emulated — this mapper handles the banking behaviour only.

---

### Type 4 — Planar 48KB (no mapper)

**Function**: `loadrom_planar48()`

**Memory window**: `0000h–BFFFh` (48 KB), spanning three 16 KB pages.

**Bank switching**: None.

**Address mapping**:
```
MSX address → ROM offset = addr      (for addr ≤ BFFFh)
```

**Cache**: Linear DMA cache via `prepare_rom_source()` with `preferred_size = 49152`. The entire 48 KB ROM fits in the 192 KB SRAM cache. All reads are direct pointer dereferences.

---

### Type 5 — ASCII8

**Function**: `loadrom_ascii8()` → `banked8_loop()`

**Memory window**: `4000h–BFFFh` (32 KB visible), divided into four 8 KB banks.

**Bank layout**:

| Bank | Address Range | Register Window | Default |
|---|---|---|---|
| 0 | `4000h–5FFFh` | `6000h–67FFh` | 0 |
| 1 | `6000h–7FFFh` | `6800h–6FFFh` | 1 |
| 2 | `8000h–9FFFh` | `7000h–77FFh` | 2 |
| 3 | `A000h–BFFFh` | `7800h–7FFFh` | 3 |

**Bank register width**: 8-bit. Supports up to 256 banks × 8 KB = 2 MB.

**Address mapping**:
```
bank_index = (addr - 4000h) >> 13       (0..3)
ROM offset = bank_registers[bank_index] × 2000h + (addr & 1FFFh)
```

**Write handler** (`handle_ascii8_write`): All register windows are in the `6000h–7FFFh` range:
```
6000h–67FFh → regs[0]
6800h–6FFFh → regs[1]
7000h–77FFh → regs[2]
7800h–7FFFh → regs[3]
```

**Cache**: Linear DMA cache via `prepare_rom_source()`. ROMs up to 192 KB are fully cached. Larger ROMs use partial caching with flash XIP fallback.

---

### Type 6 — ASCII16

**Function**: `loadrom_ascii16()`

**Memory window**: `4000h–BFFFh` (32 KB visible), divided into two 16 KB banks.

**Bank layout**:

| Bank | Address Range | Register Window | Default |
|---|---|---|---|
| 0 (page 1) | `4000h–7FFFh` | `6000h–67FFh` | 0 |
| 1 (page 2) | `8000h–BFFFh` | `7000h–77FFh` | 1 |

**Bank register width**: 8-bit. Supports up to 256 banks × 16 KB = 4 MB.

**Address mapping**:
```
bank = (addr >> 15) & 1                 (0 or 1)
ROM offset = bank_registers[bank] × 4000h + (addr & 3FFFh)
```

**Write handler** (`handle_ascii16_write`):
```
6000h–67FFh → regs[0]
7000h–77FFh → regs[1]
```

**Cache**: Linear DMA cache via `prepare_rom_source()`. ROMs up to 192 KB are fully cached in SRAM. For larger ROMs the linear cache covers banks 0–11 from SRAM and falls back to flash XIP beyond that.

**Notes**: Unlike ASCII16-X, this mapper does not mirror pages to all quadrants and uses simple 8-bit bank numbers.

---

### Type 7 — Konami (without SCC)

**Function**: `loadrom_konami()` → `banked8_loop()`

**Memory window**: `4000h–BFFFh` (32 KB visible), divided into four 8 KB banks.

**Bank layout**:

| Bank | Address Range | Register Window | Default |
|---|---|---|---|
| 0 | `4000h–5FFFh` | *(fixed, not switchable)* | 0 |
| 1 | `6000h–7FFFh` | `6000h–67FFh` | 1 |
| 2 | `8000h–9FFFh` | `8000h–87FFh` | 2 |
| 3 | `A000h–BFFFh` | `A000h–A7FFh` | 3 |

**Bank register width**: 8-bit.

**Address mapping**: Same as Konami SCC — `bank_registers[(addr - 4000h) >> 13] × 2000h + (addr & 1FFFh)`.

**Write handler** (`handle_konami_write`): Bank 0 is fixed and cannot be switched. Banks 1–3 are switchable:
```
6000h–67FFh → regs[1]
8000h–87FFh → regs[2]
A000h–A7FFh → regs[3]
```

**Cache**: Linear DMA cache via `prepare_rom_source()`, identical to Konami SCC.

**Notes**: The key difference from Konami SCC is that bank 0 at `4000h–5FFFh` is permanently mapped and the bank register windows are at different addresses.

---

### Type 8 — NEO8

**Function**: `loadrom_neo8()`

**Memory window**: `0000h–BFFFh` (48 KB visible), divided into six 8 KB banks.

**Bank layout**:

| Bank | Address Range | Primary Register | Mirrors |
|---|---|---|---|
| 0 | `0000h–1FFFh` | `5000h` | `1000h`, `9000h`, `D000h` |
| 1 | `2000h–3FFFh` | `5800h` | `1800h`, `9800h`, `D800h` |
| 2 | `4000h–5FFFh` | `6000h` | `2000h`, `A000h`, `E000h` |
| 3 | `6000h–7FFFh` | `6800h` | `2800h`, `A800h`, `E800h` |
| 4 | `8000h–9FFFh` | `7000h` | `3000h`, `B000h`, `F000h` |
| 5 | `A000h–BFFFh` | `7800h` | `3800h`, `B800h`, `F800h` |

**Bank register width**: 16-bit (12-bit segment number). Each bank register address selects the register, and even/odd addresses write the low/high byte:
- Even address → writes bits 0–7 of the bank register
- Odd address → writes bits 8–15 of the bank register
- Result is masked to 12 bits (`& 0x0FFF`)

**Address mapping**:
```
bank_index = addr >> 13                  (0..5)
segment = bank_registers[bank_index] & 0FFFh
ROM offset = segment × 2000h + (addr & 1FFFh)
```

**Cache: Mapper-Aware LRU Bank Cache**

The NEO8 mapper uses the same LRU bank cache system as ASCII16-X, adapted for 8 KB segments. The 192 KB SRAM is divided into **24 slots × 8 KB**. Each bank-register write triggers `bcache_ensure()` to guarantee the segment is resident before the next read. The read path is always a direct SRAM access.

**Notes**: NEO8 supports ROMs up to 4096 segments × 8 KB = 32 MB. The 16-bit register write protocol (split across even/odd addresses) allows the full 12-bit segment number to be set from the Z80's 8-bit data bus.

---

### Type 9 — NEO16

**Function**: `loadrom_neo16()`

**Memory window**: `0000h–BFFFh` (48 KB visible), divided into three 16 KB banks.

**Bank layout**:

| Bank | Address Range | Primary Register | Mirrors |
|---|---|---|---|
| 0 | `0000h–3FFFh` | `5000h` | `1000h`, `9000h`, `D000h` |
| 1 | `4000h–7FFFh` | `6000h` | `2000h`, `A000h`, `E000h` |
| 2 | `8000h–BFFFh` | `7000h` | `3000h`, `B000h`, `F000h` |

**Bank register width**: 16-bit (12-bit segment number), same even/odd byte protocol as NEO8.

**Address mapping**:
```
bank_index = addr >> 14                  (0..2)
segment = bank_registers[bank_index] & 0FFFh
ROM offset = segment × 4000h + (addr & 3FFFh)
```

**Cache: Mapper-Aware LRU Bank Cache**

The NEO16 mapper uses the same LRU bank cache system as ASCII16-X, with **12 slots × 16 KB**. Each bank-register write triggers `bcache_ensure()` to guarantee the segment is resident before the next read.

**Notes**: NEO16 supports ROMs up to 4096 segments × 16 KB = 64 MB.

---

### Type 10 — Sunrise IDE Nextor

**Function**: `loadrom_sunrise()`

**Memory window**: `4000h–7FFFh` (16 KB single window). No ROM at `8000h–BFFFh`.

**Bank switching**: Page selected via the Sunrise IDE control register at `4104h`. Bits 7:5 of the control register select the ROM segment (0–7).

**Address mapping**:
```
segment = ide.segment                    (cReg bits 7:5, range 0–7)
ROM offset = segment × 4000h + (addr & 3FFFh)
```

**IDE register overlay**: When bit 0 of the control register is set, addresses `7C00h–7EFFh` are intercepted as IDE registers instead of ROM:

| Address Range | Function |
|---|---|
| `7C00h–7DFFh` | 16-bit data register (low/high byte latch) |
| `7E00h–7EFFh` | ATA task-file registers (mirrored every 16 bytes) |
| `7F00h–7FFFh` | ROM data (not intercepted) |

ATA commands are translated to USB MSC operations running on **Core 1** of the RP2040 via TinyUSB host mode.

**Cache**: Linear DMA cache via `prepare_rom_source()`. Nextor ROMs are typically small (128–256 KB) and fit entirely in the 192 KB SRAM cache.

**Write handling**: Unlike other mappers, the Sunrise IDE uses a **continuous polling loop** that drains write events even while waiting for reads. This is critical because each ATA command involves a burst of 8–9 writes (bank switch + IDE_ON + 6 task-file registers + command) with no intervening reads. The write captor FIFO is 8 entries deep; without continuous draining it would overflow, silently dropping writes and causing data corruption.

---

### Type 11 — Sunrise IDE Nextor + Memory Mapper

**Function**: `loadrom_sunrise_mapper()`

**Memory window**: Full 64 KB address space via expanded slot.

This is the most complex mapper type, combining Sunrise IDE with a memory mapper using a two-phase boot process.

**Expanded slot layout**:

| Sub-slot | Content | Address Range |
|---|---|---|
| 0 | Nextor ROM (Sunrise IDE) | `4000h–7FFFh` |
| 1 | Memory Mapper RAM (192 KB) | `0000h–FFFFh` (all 4 pages) |
| 2 | Unused | — |
| 3 | Unused | — |

**Sub-slot register**: Address `FFFFh` (read returns `NOT(subslot_reg)`). Bits `[1:0]` select page 0's sub-slot, `[3:2]` page 1, `[5:4]` page 2, `[7:6]` page 3.

**Memory mapper registers**: I/O ports `FCh–FFh` select which 16 KB page of mapper RAM appears in each quadrant:

| Port | Address Range | Reset Value |
|---|---|---|
| `FCh` | `0000h–3FFFh` | 3 |
| `FDh` | `4000h–7FFFh` | 2 |
| `FEh` | `8000h–BFFFh` | 1 |
| `FFh` | `C000h–FFFFh` | 0 |

Page registers are 8-bit values normalized modulo 12 to fit within the 192 KB / 12 page mapper RAM.

**Two-phase boot**:

1. **Phase 1 — Bootstrap**: A minimal ROM (18 bytes) with an INIT routine that sets port `F4h` bit 7 (forces cold-boot on MSX2+) and executes `RST 00h`. This guarantees the MSX performs a clean restart so the Pico can set up the expanded slot mapper before the BIOS probes slots.

2. **Phase 2 — Mapper initialisation**: After detecting the restart, the firmware asserts `/WAIT` to freeze the MSX, initialises all mapper state (RAM, IDE, sub-slot register), and starts the PIO I/O bus for port `FC–FF` handling before releasing `/WAIT`.

**Cache**: **No ROM cache** — `rom_cache_capacity` is set to 0 and the entire 192 KB SRAM pool is repurposed as mapper RAM. The Nextor ROM is served directly from flash XIP. This is acceptable because the ROM is only accessed in sub-slot 0's `4000h–7FFFh` window (16 KB visible), and the Z80 is frozen via `/WAIT` during each flash read.

**Four-FIFO polling loop**: The main loop services four FIFOs continuously:
- PIO0 SM0 RX — memory read requests
- PIO0 SM1 RX — memory writes (IDE registers + mapper RAM + sub-slot register)
- PIO1 SM0 RX — I/O read requests (mapper page register values)
- PIO1 SM1 RX — I/O writes (mapper page register updates)

---

### Type 12 — ASCII16-X

**Function**: `loadrom_ascii16x()`

**Memory window**: Full 64 KB address space via two mirrored 16 KB pages.

**Page layout**:

| Page | Primary Range | Mirror |
|---|---|---|
| Page 1 | `4000h–7FFFh` | `C000h–FFFFh` |
| Page 2 | `8000h–BFFFh` | `0000h–3FFFh` |

ROM is visible in all four 16 KB quadrants. Address bit 14 selects the page: when bit 14 = 1 → page 1, when bit 14 = 0 → page 2.

**Bank register layout** (with full mirroring):

| Page | Register Windows |
|---|---|
| Page 1 | `2000h–2FFFh`, `6000h–6FFFh`, `A000h–AFFFh`, `E000h–EFFFh` |
| Page 2 | `3000h–3FFFh`, `7000h–7FFFh`, `B000h–BFFFh`, `F000h–FFFFh` |

**Bank register width**: 12-bit. The bank number is composed from two sources:
- **Bits 0–7**: from the data bus (D0–D7)
- **Bits 8–11**: from address lines A8–A11

```
Address:  XX1P MMMM XXXX XXXX
Data:     LLLL LLLL

Bank number = (MMMM << 8) | LLLL LLLL     (12 bits, 0–4095)
P = 0 → page 1, P = 1 → page 2
```

This allows up to 4096 banks × 16 KB = **64 MB** addressable capacity (designs exist for 8 MB with 512 banks, 9-bit bank numbers).

**Initial state**: Both bank registers are 0 after reset. The firmware pre-fills bank 0 in the cache for both pages.

**Cache: Mapper-Aware LRU Bank Cache**

The ASCII16-X mapper uses a specialised LRU bank cache (`bank_cache_t`) instead of the generic linear DMA cache. The same cache system is also used by NEO8 and NEO16. This is necessary because:

1. ASCII16-X ROMs can be up to 64 MB — far larger than the 192 KB SRAM.
2. The active working set can span any region of the ROM (not just the first 192 KB).
3. Every read must be served from SRAM to keep `/WAIT` hold time consistent and minimal.

**Cache layout**: The 192 KB SRAM is divided into **12 slots × 16 KB**, exactly matching the ASCII16-X bank size:

```
rom_sram[0 .. 16383]       = slot 0
rom_sram[16384 .. 32767]   = slot 1
rom_sram[32768 .. 49151]   = slot 2
...
rom_sram[180224 .. 196607] = slot 11
```

**Cache metadata** (`bank_cache_t`):

| Field | Type | Description |
|---|---|---|
| `slot_bank[]` | `uint16_t` | Bank number currently loaded in each slot (`0xFFFF` = empty) |
| `slot_lru[]` | `uint8_t` | LRU counter per slot (0 = most recently used) |
| `page_slot[]` | `int8_t` | Which cache slot is pinned to each active page |
| `flash_base` | `const uint8_t *` | Pointer to ROM data in flash |
| `rom_length` | `uint32_t` | Total ROM size in bytes |
| `num_slots` | `uint8_t` | Actual slot count (12 for 16 KB banks, 24 for 8 KB banks) |
| `num_pins` | `uint8_t` | Number of simultaneously pinned pages |
| `slot_size` | `uint16_t` | Bytes per slot (8192 or 16384) |
| `slot_shift` | `uint8_t` | log₂(slot_size): 13 or 14 |

**LRU eviction algorithm**: When a bank register write triggers a cache miss:

1. **Search** (`bcache_find`): Scan all 12 slots for the requested bank number. If found → cache hit, promote to MRU.
2. **Evict** (`bcache_evict`): If not found, select the slot with the highest LRU counter that is **not pinned** to any active page. The slots mapped to active pages are never evicted.
3. **Fill** (`bcache_ensure`): Copy 16 KB from flash (`memcpy`) into the victim slot. If the bank extends beyond the ROM size, the remainder is filled with `0xFF`.
4. **Promote** (`bcache_touch`): Set the new slot's LRU counter to 0 and increment all lower counters.

**Timing**: Flash latency occurs only at bank-switch time. A 16 KB `memcpy` from flash XIP at 250 MHz takes approximately 0.5–1 ms. This happens once per cache miss, then all subsequent reads from that bank are pure SRAM accesses.

**Write handler** (`handle_ascii16x_write_cached`): On every bank register write, the handler immediately calls `bcache_ensure()` to guarantee the bank is resident before the next read. This means the read path never touches flash:

```c
// Read path — always SRAM, never flash
uint8_t data = rom_sram[(uint32_t)slot * A16X_SLOT_SIZE + (addr & 0x3FFFu)];
```

**AMD Flash Command Emulation**

Some ASCII16-X ROMs (e.g. Neon Horizon) implement a cartridge detection routine that performs an in-place flash byte-program and read-back, or use flash write/erase commands for save data. To support this, the write handler feeds **every** write through an AMD flash command state machine (`flash_cmd_state_t`) before processing bank registers.

The state machine tracks the standard AMD unlock + command sequences used by common flash chips (AM29F040, MX29LV640, SST39SF040, etc.):

| State | Transition Condition | Next State |
|---|---|---|
| `FLASH_IDLE` | Write `AAh` to `*AAAh` | `FLASH_UNLOCK1` |
| `FLASH_UNLOCK1` | Write `55h` to `*555h` | `FLASH_UNLOCK2` |
| `FLASH_UNLOCK2` | Write `A0h` to `*AAAh` | `FLASH_BYTE_PGM` |
| `FLASH_UNLOCK2` | Write `80h` to `*AAAh` | `FLASH_ERASE_SETUP` |
| `FLASH_BYTE_PGM` | Write data to target address | `FLASH_IDLE` |
| `FLASH_ERASE_SETUP` | Write `AAh` to `*AAAh` | `FLASH_ERASE_UNLOCK1` |
| `FLASH_ERASE_UNLOCK1` | Write `55h` to `*555h` | `FLASH_ERASE_UNLOCK2` |
| `FLASH_ERASE_UNLOCK2` | Write `30h` to sector address | `FLASH_IDLE` |

Command addresses use the lower 12 bits of the MSX bus address (`addr & 0x0FFF`), matching the `0xAAAh` / `0x555h` convention. Any unexpected write resets the state machine to `FLASH_IDLE`.

**Byte-program** (`FLASH_BYTE_PGM`): The target byte in the SRAM cache is AND-masked with the written value — flash memory can only clear bits, never set them. This is applied directly to `rom_sram[]` so the change is immediately visible on the next read:

```c
rom_sram[slot_offset + (addr & 0x3FFFu)] &= data;
```

**Sector-erase** (`FLASH_ERASE_UNLOCK2` + `30h`): The entire cache slot for the page containing the target address is filled with `0xFF`:

```c
memset(&rom_sram[slot_offset], 0xFF, slot_size);
```

Both operations modify only the SRAM cache — the original ROM data in flash is never altered. If the modified bank is evicted and later reloaded, the changes are lost and the original flash data is restored. This is consistent with the volatile nature of the emulation.

**Worst-case scenario**: A game that rapidly alternates between more than 12 distinct banks would thrash the cache, with each switch costing ~1 ms of flash access. In practice, the 12-slot capacity comfortably holds the working set of most games since they typically use a few code banks plus a few data banks simultaneously.

**Comparison with linear cache**: For a 2 MB ASCII16-X ROM:
- Linear cache: banks 0–11 (first 192 KB) are fast, banks 12–127 are slow (flash XIP per byte).
- LRU cache: **any** 12 banks are fast, regardless of ROM offset.

---

### Type 13 — Planar 64KB (no mapper)

**Function**: `loadrom_planar64()`

**Memory window**: `0000h–FFFFh` (full 64 KB address space, all four quadrants).

**Bank switching**: None.

**Address mapping**:
```
MSX address → ROM offset = addr
```

The firmware always drives the bus (`in_window = true`) for all addresses, covering pages 0–3.

**Cache**: Linear DMA cache via `prepare_rom_source()` with `preferred_size = 65536`. Since the maximum ROM size is 64 KB and the cache is 192 KB, the entire ROM always fits in SRAM. However, because the ROM may exceed `rom_cache_capacity` in certain configurations, reads still go through `read_rom_byte()` for safety.

---

### Type 14 — Manbow2 (Konami SCC + AMD Flash)

**Function**: `loadrom_manbow2()`

**Memory window**: `4000h–BFFFh` (32 KB visible), divided into four 8 KB banks — identical to Konami SCC.

**Bank layout**:

| Bank | Address Range | Select Register |
|---|---|---|
| 0 | `4000h–5FFFh` | `5000h–57FFh` |
| 1 | `6000h–7FFFh` | `7000h–77FFh` |
| 2 | `8000h–9FFFh` | `9000h–97FFh` |
| 3 | `A000h–BFFFh` | `B000h–B7FFh` |

**Bank register width**: 8-bit, masked to 6 bits (values 0–63 for 512 KB / 8 KB = 64 blocks).

**Address mapping**:
```
page = (addr − 4000h) >> 13              (0..3)
segment = bank_registers[page] & 3Fh
ROM offset = segment × 2000h + (addr & 1FFFh)
```

**Hardware model**: The Manbow2 cartridge (Space Manbow 2) uses a standard Konami SCC bank-switching scheme backed by an **AMD AM29F040B** 512 KB flash chip instead of a mask ROM. The last 64 KB sector (`70000h–7FFFFh`) is writable and used by the game for save data. All other sectors are write-protected.

Every write to `4000h–BFFFh` goes through **both** a bank-switch decoder and the flash command state machine, matching the real hardware where bank-select lines and flash chip-enable are active simultaneously. The flash address for each write is computed from the **current** bank register value *before* the bank register is updated — this ordering matches the real cartridge where the flash data input arrives on the same bus cycle that sets the bank latch.

**AMD flash state machine**:

The firmware implements the following subset of AMD flash commands:

| Command | Sequence | Effect |
|---|---|---|
| Reset | `F0h` → any address | Returns to read mode |
| Auto-select | `AAh` → `[x555h]`, `55h` → `[x2AAh]`, `90h` → `[x555h]` | Enters ID mode |
| Program byte | `AAh` → `[x555h]`, `55h` → `[x2AAh]`, `A0h` → `[x555h]`, data → target | AND-programs one byte |
| Sector erase | `AAh` → `[x555h]`, `55h` → `[x2AAh]`, `80h` → `[x555h]`, `AAh` → `[x555h]`, `55h` → `[x2AAh]`, `30h` → sector | Fills sector with `FFh` |

Command address matching uses `flash_addr & 7FFh` to check for `555h` and `2AAh`, where `flash_addr = bank_reg × 2000h + (addr & 1FFFh)`.

In auto-select mode, reads return:

| Offset & 03h | Value | Meaning |
|---|---|---|
| `00h` | `01h` | AMD manufacturer ID |
| `01h` | `A4h` | AM29F040B device ID |
| `02h` | `00h` / `01h` | Sector protection (0 = writable, 1 = protected) |
| `03h` | `00h` | Extra code |

**SRAM layout**:

```
┌───────────────────────────┐  rom_sram[0]
│  ROM cache (128 KB)       │  DMA-copied from flash; serves banks 0–15
├───────────────────────────┤  rom_sram[131072]
│  Writable sector (64 KB)  │  Initialised from ROM sector 7 (70000h–7FFFFh)
│  Save data (volatile)     │  AND-programmed / erased by flash commands
└───────────────────────────┘  rom_sram[196607]
```

The first 128 KB of the 192 KB SRAM pool is allocated as a linear ROM cache via `prepare_rom_source()`. The remaining 64 KB backs the writable flash sector in SRAM. ROM data beyond the 128 KB cache is served from flash XIP via `read_rom_byte()`.

**Save data is volatile** — it is lost when the MSX is powered off. The writable SRAM sector is initialised at boot from the ROM image stored in flash.

**Write handling**: Like the Sunrise IDE mapper Manbow2 uses a **continuous polling loop** that drains write events even while waiting for reads. The game writes heavily to the flash chip through addresses `5000h–57FFh` (which overlap with bank-select space), generating bursts of 100+ writes without intervening reads. Without continuous draining the 8-entry PIO write captor FIFO overflows, silently losing bank-switch and flash command writes.

**Auto-detection**: The tool identifies Manbow2 ROMs by checking for a 512 KB size, an `AB` header at offset 0, and the string `Manbow 2` at offset `28000h`. The filename tag `MANBW2` (aliases: `MANBOW2`, `MBW-2`) forces this mapper type.

**Notes**: SCC sound registers (`9800h–9FFFh`) are visible but the PicoVerse does not emulate SCC audio — only the bank switching and flash behaviour are supported.

---

## Cache Strategy Summary

| Type | Mapper | Bank Size | Max ROM | Cache Strategy | Cache Hit Guarantee |
|---|---|---|---|---|---|
| 1/2 | Planar 16/32KB | — | 32 KB | Linear DMA (full) | Always (ROM ≤ cache) |
| 3 | Konami SCC | 8 KB | 2 MB | Linear DMA (partial) | Banks in first 192 KB |
| 4 | Planar 48KB | — | 48 KB | Linear DMA (full) | Always (ROM ≤ cache) |
| 5 | ASCII8 | 8 KB | 2 MB | Linear DMA (partial) | Banks in first 192 KB |
| 6 | ASCII16 | 16 KB | 4 MB | Linear DMA (partial) | Banks in first 192 KB |
| 7 | Konami | 8 KB | 2 MB | Linear DMA (partial) | Banks in first 192 KB |
| 8 | NEO8 | 8 KB | 32 MB | LRU bank cache (24 slots) | Any 24 active segments |
| 9 | NEO16 | 16 KB | 64 MB | LRU bank cache (12 slots) | Any 12 active segments |
| 10 | Sunrise IDE | 16 KB | 128 KB | Linear DMA (full) | Always (typical ROMs ≤ cache) |
| 11 | Sunrise + Mapper | 16 KB | 128 KB | None (SRAM → mapper RAM) | Never (ROM from flash) |
| 12 | ASCII16-X | 16 KB | 64 MB | LRU bank cache (12 slots) | Any 12 active banks |
| 13 | Planar 64KB | — | 64 KB | Linear DMA (full) | Always (ROM ≤ cache) |
| 14 | Manbow2 | 8 KB | 512 KB | Linear DMA (128 KB) + 64 KB SRAM | Banks in first 128 KB |

Author: Cristiano  Goncalves
Last updated: 03/28/2026