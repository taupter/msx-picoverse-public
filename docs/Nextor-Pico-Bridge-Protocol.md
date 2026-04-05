# NEXTOR <-> PicoBridge Protocol

Author: Cristiano Goncalves
Last updated: November 29, 2025

This document describes the control/data protocol implemented by the Raspberry Pico bridge firmware in `2040/software/multirom.pio/pico/multirom/nextor.c`.
The bridge provides an interface between an MSX running NEXTOR and a USB Mass Storage Device (MSC) attached to the Pico. The MSX communicates with the Pico via two I/O ports (control and data) using simple commands, addresses and payload transfers.

This specification documents the transport, command set, data formats, read/write sequences, return codes, error handling and implementation notes so other implementers can reproduce the protocol behavior.

## Terminology
- **MSX**: The host (NEXTOR) that talks to the Pico over the MSX I/O bus.
- **Pico / Bridge**: The Raspberry Pico running the firmware in `nextor.c`.
- **MSC**: USB Mass Storage Class device (USB flash, SD card reader, etc.).
- **LBA**: Logical Block Addressing (sector index).

## Ports
- **Control port**: `0x9E` (named `PORT_CONTROL` in firmware). Writes select commands, reads return the control status byte.
- **Data port**: `0x9F` (named `PORT_DATAREG` in firmware). Used to transfer payload bytes (addresses, descriptors, block payloads).

**Control response values (status byte)**
- `0x00` : Success / Ready
- `0x01` : Busy (operation in progress)
- `0xFF` : Error / Invalid / Not available

Control reads return the last status byte placed in `control_response`.

## Transport model
- All command-selection is performed by writing a single command byte to the control port (`0x9E`).
- Additional bytes required by the command are transferred via the data port (`0x9F`) using MSX writes.
- When a command produces a multi-byte response, the bridge populates an internal `data_response_buffer` and sets `data_response_pending = true`. The MSX reads response bytes from the data port (`0x9F`) until the bridge clears `data_response_pending`.
- For read and write operations on the MSC, the bridge uses asynchronous TinyUSB MSC requests. While USB MSC requests are active the bridge reports `0x01` (Busy) on the control port.

## Important global constraints
- The firmware caches `usb_device_info_valid` to indicate a valid MSC device is attached and probed. Many commands fail with `0xFF` until a device is mounted, probed and `usb_device_info_valid` is true.
- `usb_block_size` must be non-zero and <= 512 (the code uses 512-sized buffers). If device block size is larger than the buffer the operation will be rejected.

## Command list

1) Initialize USB Host
- Command byte: `0x01` (Control Write)
- Description: Start/init the USB host stack on the Pico so attached MSCs can be enumerated.
- Behavior:
  - If no device info is present, the firmware sets `usb_task_running = true`, reports intermediate `control_response = 0x01` then attempts to init TinyUSB/TUH (`tusb_init()`, `board_init_after_tusb()`, `tuh_init(0)`).
  - On success the command sets `control_response = 0x00`.
  - If a device is already known the command simply returns `0x00`.

2) Get USB VID (Vendor ID)
- Command byte: `0x02` (Control Write)
- Response: two bytes returned via data port, little-endian: low byte then high byte of `usb_device_info.vid`.
- Behavior:
  - If `usb_device_info_valid` is false, the command returns `control_response = 0xFF`.
  - Otherwise it preloads `data_response_buffer[0..1]` and sets `data_response_length = 2` and `control_response = 0x00`.

3) Get USB Vendor/Product strings and Revision
- Command byte: `0x03` (Control Write)
- Response via data port: concatenated strings
  - Vendor ID string: up to 8 bytes (padded or filled; the firmware writes the string into an 8-byte field)
  - Product ID string: up to 16 bytes
  - Product revision string: up to 4 bytes
  - Total response length is 8 + 16 + 4 = 28 bytes (or less depending on null-terminated string lengths, but unused bytes have value `0xFF`).
- Behavior: returns `0xFF` if `usb_device_info_valid` is false.

4) Get USB sectors (block count)
- Command byte: `0x04` (Control Write)
- Response: 4 bytes little-endian representing `usb_block_count` (32-bit unsigned).
- Behavior: returns `0xFF` if `usb_device_info_valid` is false.

5) Get USB block size
- Command byte: `0x05` (Control Write)
- Response: 4 bytes little-endian representing `usb_block_size` (32-bit unsigned). Note: firmware expects this to be <= 512.
- Behavior: returns `0xFF` if `usb_device_info_valid` is false.

6) Read a single 512-byte block (explicit LBA)
- Command byte: `0x06` (Control Write)
- Sequence:
  1. MSX writes `0x06` to control port. Bridge checks device present and other invariants. If OK, it enters an address-collection state and sets `control_response = 0x01` (Busy).
  2. MSX writes four bytes (little-endian) to the data port (`0x9F`): LBA low, LBA+1, LBA+2, LBA+3.
  3. After the 4th address byte, the bridge calls `start_block_read(latched_read_lba)` which issues `tuh_msc_read10()` for 1 block. While the USB read is outstanding the control port will return `0x01` (Busy).
  4. When the USB read completes successfully the firmware copies the block into `data_response_buffer`, pads/truncates to 512 bytes and sets `data_response_length = 512` and `control_response = 0x00`.
  5. MSX reads 512 bytes from the data port; the bridge returns bytes from `data_response_buffer` one at a time while `data_response_pending` is true. Once all bytes are read the bridge clears `data_response_pending` and sets `control_response = 0x00`.
- Error conditions:
  - If no device/invalid block size, or a read already pending, the initial command returns `0xFF`.
  - If `start_block_read()` fails or the USB request fails, the bridge will eventually set `control_response = 0xFF` and `read_sequence_valid = false`.

7) Read next sequential 512-byte block
- Command byte: `0x07` (Control Write)
- Sequence:
  - This command requires `read_sequence_valid == true`; typically set after a successful `0x06` read. The firmware will attempt to start a block read of `read_next_lba`.
  - If the requested LBA >= `usb_block_count` the command fails (`control_response = 0xFF`) and `read_sequence_valid` is cleared.
  - On success it calls `start_block_read(requested_lba)`, updates `read_next_lba = requested_lba + 1` and sets `control_response = 0x01` (Busy) until the block arrives.

8) Write a single 512-byte block with explicit address
- Command byte: `0x08` (Control Write)
- Sequence:
  1. MSX writes `0x08` to control port. The bridge enters an address-collection state and sets `control_response = 0x01`.
  2. MSX writes 4 little-endian address bytes to the data port to specify LBA.
  3. Bridge validates LBA < `usb_block_count`. If valid it sets `current_write_lba = pending_write_lba`, `write_data_pending = true` and starts collecting payload bytes.
  4. MSX writes `usb_block_size` bytes to data port. Each write stores one byte into `block_write_buffer`.
  5. After the full payload is collected the firmware calls `start_block_write(current_write_lba)` which issues `tuh_msc_write10()` for 1 block. While the USB write is outstanding the control port returns `0x01` (Busy).
  6. On completion the bridge sets `control_response = 0x00` (success) or `0xFF` (failure) depending on the MSC result.
- Error conditions:
  - If device not present, invalid block size, write already in progress, or invalid LBA, the command returns `0xFF`.

9) Write next sequential 512-byte block
- Command byte: `0x09` (Control Write)
- Sequence:
  - Requires `write_sequence_valid == true` (set by a successful prior write).
  - The bridge checks `write_next_lba < usb_block_count`. If OK it sets `pending_write_lba = write_next_lba`, `current_write_lba = pending_write_lba`, `write_data_pending = true` and `control_response = 0x01`.
  - MSX sends `usb_block_size` payload bytes to the data port, after which the bridge calls `start_block_write(current_write_lba)` and returns `0x01` until write completes.

## Data framing and byte order
- Multi-byte integers transferred between MSX and bridge are always little-endian in this implementation.
- LBAs are 32-bit unsigned values encoded as 4 consecutive bytes (LSB first).
- Block count and block size responses are 32-bit little-endian integers (4 bytes).

## Data payloads
- Block reads: Bridge guarantees `data_response_length` will be set to 512 after a successful read. If the returned block is shorter the firmware pads the remainder with `0x00`.
- Block writes: MSX must send exactly `usb_block_size` bytes for the write payload. `usb_block_size` is read via command `0x05`.

## Error handling and state flags
- `usb_device_info_valid` : When false many commands return `0xFF`.
- `block_read_in_progress`, `block_read_ready`, `block_read_failed` : control the read lifecycle. A failed read clears `read_sequence_valid`.
- `block_write_in_progress`, `block_write_done`, `block_write_failed` : control the write lifecycle. A failed write clears `write_sequence_valid`.
- `read_sequence_valid`, `read_next_lba`, `write_sequence_valid`, `write_next_lba` : track sequential read/write sequences for `0x07` and `0x09` commands.

## Low-level IO notes (implementation details)
- The firmware polls MSX bus signals using GPIOs:
  - `PIN_IORQ` is used to detect an IO request (active low).
  - `PIN_WR` and `PIN_RD` detect write/read strobes (active low).
  - `gpio_get_all()` is sampled; the low 8 bits represent the I/O port address and bits 16..23 represent the data bus value (shifted by 16 in the code).
- During control/data reads the firmware temporarily drives the Pico data bus GPIOs to output to return bytes, then returns them to input.

## Examples

### Read block 0 example:
1. MSX: OUT (port 0x9E), value `0x06`   ; Request explicit read
2. MSX: OUT (port 0x9F), value `0x00`   ; LBA byte 0 (LSB)
3. MSX: OUT (port 0x9F), value `0x00`   ; LBA byte 1
4. MSX: OUT (port 0x9F), value `0x00`   ; LBA byte 2
5. MSX: OUT (port 0x9F), value `0x00`   ; LBA byte 3 (MSB)
   - Bridge returns control port `0x01` while performing USB read.
6. After USB completes, MSX: IN (port 0x9E) returns `0x00` (ready)
7. MSX: IN (port 0x9F) repeated 512 times to read the block payload (bridge returns bytes from `data_response_buffer`).

### Sequential read example (read block N then N+1):
1. Do explicit read of block N (as above) — this sets `read_sequence_valid` and `read_next_lba = N+1`.
2. MSX: OUT (port 0x9E), value `0x07` ; Request next sequential block
   - Bridge will start read of `read_next_lba` and increment `read_next_lba` again on success.

### Write block example (explicit address):
1. MSX: OUT (port 0x9E), value `0x08` ; Start write with explicit address
2. MSX: OUT (port 0x9F), value LBA byte 0
3. MSX: OUT (port 0x9F), value LBA byte 1
4. MSX: OUT (port 0x9F), value LBA byte 2
5. MSX: OUT (port 0x9F), value LBA byte 3
6. MSX: OUT (port 0x9F) repeated `usb_block_size` times with payload bytes
   - Bridge returns `0x01` while the write completes. After completion, the control port will return `0x00` for success or `0xFF` for failure.

## Implementation notes / caveats
- The bridge uses TinyUSB host stack asynchronously. USB transactions may take relatively long time; the firmware reports busy via `0x01` so the MSX can wait or poll.
- The firmware limits block payload handling to 512 bytes buffers. If a device returns a `usb_block_size` larger than the internal buffer the operation will fail.
- The data response buffer is pre-filled with `0xFF` by default; on successful reads the bridge copies the read data into the response buffer and pads any missing bytes with `0x00`.
- The firmware assumes little-endian ordering for all multi-byte integers exchanged with the MSX.
- The firmware uses the `read_sequence_valid` and `write_sequence_valid` flags to maintain sequential transfers; those flags are cleared on errors.

## Pointers to code
- Command handling and protocol state machine: `nextor_io()` in `nextor.c`.
- USB read/write start and completion: `start_block_read()`, `block_read_complete_cb()`, `start_block_write()`, `block_write_complete_cb()`.
- USB device probing/inquiry: `tuh_msc_inquiry()` and `inquiry_complete_cb()`.

## Appendix: Quick reference
- Control port (`0x9E`) commands and behaviour:
  - `0x01` : Initialize USB host (returns `0x00` or `0xFF`)
  - `0x02` : Get VID (2 bytes, LE)
  - `0x03` : Get vendor/product/rev strings (8/16/4 bytes)
  - `0x04` : Get block count (4 bytes, LE)
  - `0x05` : Get block size (4 bytes, LE)
  - `0x06` : Read explicit block (send 4 LBA bytes via data port, then read 512 bytes)
  - `0x07` : Read next sequential block (no address bytes)
  - `0x08` : Write explicit block (send 4 LBA bytes then send payload bytes)
  - `0x09` : Write next sequential block (no address bytes)
