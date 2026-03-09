// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// usb_midi_host.c - USB MIDI host class driver for TinyUSB
//
// Registers as a TinyUSB application-level host class driver via
// usbh_app_driver_get_cb(). Handles USB MIDI device enumeration,
// bulk endpoint management, and MIDI stream parsing/encoding.
//
// Architecture:
//   - Called from Core 1 (TinyUSB host task context)
//   - Provides thread-safe ring buffers for Core 0 (MSX I/O IRQ) interaction
//   - Parses raw MIDI byte stream into USB-MIDI Event Packets (4 bytes each)
//   - Decodes received USB-MIDI Event Packets back to raw MIDI bytes
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "host/usbh_pvt.h"
#include "usb_midi_host.h"
#include "midi.h"

// -----------------------------------------------------------------------
// USB MIDI device context
// -----------------------------------------------------------------------
typedef struct {
    uint8_t  dev_addr;
    uint8_t  ep_out;            // Bulk OUT endpoint address
    uint8_t  ep_in;             // Bulk IN endpoint address
    uint8_t  itf_num;           // MIDIStreaming interface number
    volatile bool mounted;
    volatile bool tx_busy;      // Bulk OUT transfer in progress
    volatile bool rx_busy;      // Bulk IN transfer in progress
} midi_dev_t;

static midi_dev_t midi_dev;

// -----------------------------------------------------------------------
// USB transfer buffers (4-byte aligned for DMA)
// -----------------------------------------------------------------------
static uint8_t __attribute__((aligned(4))) usb_tx_buf[64];
static uint8_t usb_tx_offset;

static uint8_t __attribute__((aligned(4))) usb_rx_buf[64];

// -----------------------------------------------------------------------
// RX ring buffer: Core 1 writes, Core 0 reads (from IRQ context)
// Single-producer single-consumer, safe without locks on RP2040
// -----------------------------------------------------------------------
static volatile uint8_t rx_ring_buf[MIDI_RX_BUFSIZE];
static volatile uint32_t rx_ring_head;  // Written by Core 1
static volatile uint32_t rx_ring_tail;  // Written by Core 0

static inline bool rx_ring_put(uint8_t byte) {
    uint32_t next = (rx_ring_head + 1) & (MIDI_RX_BUFSIZE - 1);
    if (next == rx_ring_tail) return false;
    rx_ring_buf[rx_ring_head] = byte;
    __dmb();
    rx_ring_head = next;
    return true;
}

static inline bool rx_ring_get(uint8_t *byte) {
    if (rx_ring_head == rx_ring_tail) return false;
    *byte = rx_ring_buf[rx_ring_tail];
    __dmb();
    rx_ring_tail = (rx_ring_tail + 1) & (MIDI_RX_BUFSIZE - 1);
    return true;
}

static inline bool rx_ring_available(void) {
    return rx_ring_head != rx_ring_tail;
}

// -----------------------------------------------------------------------
// MIDI stream parser state
// -----------------------------------------------------------------------
typedef struct {
    uint8_t running_status;
    uint8_t data[2];
    uint8_t collected;
    uint8_t expected;
    bool    in_sysex;
    uint8_t sysex_buf[3];
    uint8_t sysex_count;
} midi_parser_t;

static midi_parser_t parser;

// -----------------------------------------------------------------------
// MIDI message utilities
// -----------------------------------------------------------------------

// Number of data bytes expected for a channel voice/mode status byte
static uint8_t midi_expected_data(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0: case 0xD0: return 1;
        case 0x80: case 0x90: case 0xA0:
        case 0xB0: case 0xE0: return 2;
        default: return 0;
    }
}

// Code Index Number for USB-MIDI event packet
static uint8_t midi_cin(uint8_t status) {
    if (status >= 0x80 && status <= 0xEF) return (status >> 4) & 0x0F;
    switch (status) {
        case 0xF1: case 0xF3: return 0x02;  // 2-byte System Common
        case 0xF2: return 0x03;               // 3-byte System Common
        case 0xF6: return 0x05;               // Tune Request (1 byte)
        default:   return 0x0F;               // Single byte
    }
}

// Queue a 4-byte USB-MIDI event packet into the TX buffer
static void queue_usb_packet(const uint8_t packet[4]) {
    if (usb_tx_offset + 4 <= sizeof(usb_tx_buf)) {
        memcpy(usb_tx_buf + usb_tx_offset, packet, 4);
        usb_tx_offset += 4;
    }
}

// -----------------------------------------------------------------------
// MIDI stream parser — converts raw MIDI bytes to USB-MIDI events
// -----------------------------------------------------------------------

static void midi_parser_reset(void) {
    memset(&parser, 0, sizeof(parser));
}

static void midi_parser_feed(uint8_t byte) {
    // Real-Time messages (0xF8-0xFF) can interrupt anything
    if (byte >= 0xF8) {
        uint8_t pkt[4] = { 0x0F, byte, 0, 0 };
        queue_usb_packet(pkt);
        return;
    }

    // SysEx start
    if (byte == 0xF0) {
        parser.in_sysex = true;
        parser.sysex_count = 0;
        parser.sysex_buf[parser.sysex_count++] = byte;
        return;
    }

    // SysEx end
    if (byte == 0xF7) {
        if (parser.in_sysex) {
            parser.sysex_buf[parser.sysex_count++] = byte;
            uint8_t pkt[4] = {0, 0, 0, 0};
            switch (parser.sysex_count) {
                case 1:
                    pkt[0] = 0x05; pkt[1] = 0xF7;
                    break;
                case 2:
                    pkt[0] = 0x06; pkt[1] = parser.sysex_buf[0]; pkt[2] = 0xF7;
                    break;
                case 3:
                    pkt[0] = 0x07;
                    pkt[1] = parser.sysex_buf[0];
                    pkt[2] = parser.sysex_buf[1];
                    pkt[3] = 0xF7;
                    break;
            }
            queue_usb_packet(pkt);
            parser.in_sysex = false;
            parser.sysex_count = 0;
        }
        parser.running_status = 0;
        return;
    }

    // Accumulate SysEx data
    if (parser.in_sysex) {
        parser.sysex_buf[parser.sysex_count++] = byte;
        if (parser.sysex_count >= 3) {
            uint8_t pkt[4] = { 0x04, parser.sysex_buf[0], parser.sysex_buf[1], parser.sysex_buf[2] };
            queue_usb_packet(pkt);
            parser.sysex_count = 0;
        }
        return;
    }

    // New status byte (non-real-time, non-SysEx)
    if (byte >= 0x80) {
        parser.running_status = byte;
        parser.collected = 0;

        // Channel messages: wait for data bytes
        if (byte >= 0x80 && byte <= 0xEF) {
            parser.expected = midi_expected_data(byte);
            return;
        }

        // System Common messages
        switch (byte) {
            case 0xF1: case 0xF3:
                parser.expected = 1;
                return;
            case 0xF2:
                parser.expected = 2;
                return;
            case 0xF6: {
                uint8_t pkt[4] = { 0x05, byte, 0, 0 };
                queue_usb_packet(pkt);
                parser.running_status = 0;
                return;
            }
            default:
                return;
        }
    }

    // Data byte
    if (parser.running_status == 0) return;  // Orphan data byte

    parser.data[parser.collected++] = byte;

    if (parser.collected >= parser.expected) {
        uint8_t cin = midi_cin(parser.running_status);
        uint8_t pkt[4] = {
            cin,
            parser.running_status,
            parser.data[0],
            (parser.expected >= 2) ? parser.data[1] : 0
        };
        queue_usb_packet(pkt);
        parser.collected = 0;

        // Running status for System Common messages is cleared
        if (parser.running_status >= 0xF0) {
            parser.running_status = 0;
        }
    }
}

// -----------------------------------------------------------------------
// USB-MIDI event packet decoder — extracts raw MIDI bytes from RX packets
// -----------------------------------------------------------------------

static void decode_rx_packet(const uint8_t pkt[4]) {
    uint8_t cin = pkt[0] & 0x0F;
    uint8_t nbytes;

    switch (cin) {
        case 0x00: return;  // Misc / reserved
        case 0x01: return;  // Cable events (reserved)
        case 0x05:          // 1-byte System Common or SysEx end
        case 0x0F:          // Single byte
            nbytes = 1;
            break;
        case 0x02:          // 2-byte System Common
        case 0x06:          // SysEx end with 2 bytes
        case 0x0C:          // Program Change
        case 0x0D:          // Channel Pressure
            nbytes = 2;
            break;
        default:            // 3-byte messages (Note On/Off, CC, etc.)
            nbytes = 3;
            break;
    }

    for (uint8_t i = 0; i < nbytes; i++) {
        rx_ring_put(pkt[1 + i]);
    }
}

// -----------------------------------------------------------------------
// USB transfer callbacks
// -----------------------------------------------------------------------

static void tx_complete_cb(tuh_xfer_t *xfer) {
    (void)xfer;
    midi_dev.tx_busy = false;
    usb_tx_offset = 0;
}

static void submit_rx_transfer(void);

static void rx_complete_cb(tuh_xfer_t *xfer) {
    if (xfer->result == XFER_RESULT_SUCCESS && xfer->actual_len > 0) {
        for (uint32_t i = 0; i + 4 <= xfer->actual_len; i += 4) {
            decode_rx_packet(xfer->buffer + i);
        }
    }

    // Resubmit bulk IN transfer
    if (midi_dev.mounted && midi_dev.ep_in) {
        submit_rx_transfer();
    }
}

static void submit_rx_transfer(void) {
    if (!midi_dev.mounted || midi_dev.ep_in == 0 || midi_dev.rx_busy) return;

    tuh_xfer_t xfer = {
        .daddr       = midi_dev.dev_addr,
        .ep_addr     = midi_dev.ep_in,
        .buflen      = sizeof(usb_rx_buf),
        .buffer      = usb_rx_buf,
        .complete_cb = rx_complete_cb,
        .user_data   = 0
    };

    if (tuh_edpt_xfer(&xfer)) {
        midi_dev.rx_busy = true;
    }
}

// -----------------------------------------------------------------------
// TinyUSB host class driver callbacks
// -----------------------------------------------------------------------

static bool midi_host_init(void) {
    memset(&midi_dev, 0, sizeof(midi_dev));
    midi_parser_reset();
    usb_tx_offset = 0;
    rx_ring_head = 0;
    rx_ring_tail = 0;
    return true;
}

static bool midi_host_deinit(void) {
    return true;
}

static bool midi_host_open(uint8_t rhport, uint8_t dev_addr,
                           tusb_desc_interface_t const *desc_itf, uint16_t max_len) {
    (void)rhport;

    // Must be Audio class
    if (desc_itf->bInterfaceClass != TUSB_CLASS_AUDIO) return false;

    const uint8_t *p = (const uint8_t *)desc_itf;
    const uint8_t *end = p + max_len;
    bool found_midi_streaming = false;
    uint8_t ep_out = 0;
    uint8_t ep_in  = 0;

    while (p < end) {
        uint8_t len  = p[0];
        uint8_t type = p[1];

        if (len < 2 || p + len > end) break;

        if (type == TUSB_DESC_INTERFACE) {
            tusb_desc_interface_t const *itf = (tusb_desc_interface_t const *)p;
            // MIDI Streaming subclass = 0x03
            if (itf->bInterfaceClass == TUSB_CLASS_AUDIO && itf->bInterfaceSubClass == 3) {
                found_midi_streaming = true;
            }
        }

        if (found_midi_streaming && type == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            uint8_t xfer_type = ep->bmAttributes.xfer;
            if (xfer_type == TUSB_XFER_BULK) {
                if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_OUT) {
                    ep_out = ep->bEndpointAddress;
                    tuh_edpt_open(dev_addr, ep);
                } else {
                    ep_in = ep->bEndpointAddress;
                    tuh_edpt_open(dev_addr, ep);
                }
            }
        }

        p += len;
    }

    if (ep_out) {
        midi_dev.dev_addr = dev_addr;
        midi_dev.ep_out   = ep_out;
        midi_dev.ep_in    = ep_in;
        midi_dev.itf_num  = desc_itf->bInterfaceNumber;
        midi_dev.mounted  = true;
        midi_dev.tx_busy  = false;
        midi_dev.rx_busy  = false;
        midi_parser_reset();
        usb_tx_offset = 0;
        return true;
    }

    return false;
}

static bool midi_host_set_config(uint8_t dev_addr, uint8_t itf_num) {
    // Start receiving MIDI data if the device has a bulk IN endpoint
    if (midi_dev.ep_in) {
        submit_rx_transfer();
    }
    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool midi_host_xfer_cb(uint8_t dev_addr, uint8_t ep_addr,
                               xfer_result_t result, uint32_t xferred_bytes) {
    (void)dev_addr;
    (void)result;
    (void)xferred_bytes;

    if (ep_addr == midi_dev.ep_out) {
        midi_dev.tx_busy = false;
        usb_tx_offset = 0;
    } else if (ep_addr == midi_dev.ep_in) {
        midi_dev.rx_busy = false;
        if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0) {
            for (uint32_t i = 0; i + 4 <= xferred_bytes; i += 4) {
                decode_rx_packet(usb_rx_buf + i);
            }
        }
        if (midi_dev.mounted && midi_dev.ep_in) {
            submit_rx_transfer();
        }
    }

    return true;
}

static void midi_host_close(uint8_t dev_addr) {
    if (midi_dev.dev_addr == dev_addr) {
        midi_dev.mounted = false;
        midi_dev.dev_addr = 0;
        midi_dev.ep_out = 0;
        midi_dev.ep_in  = 0;
        midi_dev.tx_busy = false;
        midi_dev.rx_busy = false;
        midi_parser_reset();
        usb_tx_offset = 0;
    }
}

// -----------------------------------------------------------------------
// Driver registration — TinyUSB calls this weak callback at init
// -----------------------------------------------------------------------

static const usbh_class_driver_t midi_host_driver = {
    .name       = "MIDI",
    .init       = midi_host_init,
    .deinit     = midi_host_deinit,
    .open       = midi_host_open,
    .set_config = midi_host_set_config,
    .xfer_cb    = midi_host_xfer_cb,
    .close      = midi_host_close
};

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &midi_host_driver;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

bool usb_midi_host_mounted(void) {
    return midi_dev.mounted;
}

uint8_t usb_midi_host_dev_addr(void) {
    return midi_dev.dev_addr;
}

void usb_midi_host_send_byte(uint8_t byte) {
    midi_parser_feed(byte);
}

bool usb_midi_host_can_accept_byte(void) {
    if (!midi_dev.mounted || midi_dev.tx_busy) return false;
    return (usb_tx_offset + 4) <= sizeof(usb_tx_buf);
}

void usb_midi_host_flush(void) {
    if (!midi_dev.mounted || midi_dev.tx_busy || usb_tx_offset == 0) return;

    tuh_xfer_t xfer = {
        .daddr       = midi_dev.dev_addr,
        .ep_addr     = midi_dev.ep_out,
        .buflen      = usb_tx_offset,
        .buffer      = usb_tx_buf,
        .complete_cb = tx_complete_cb,
        .user_data   = 0
    };

    if (tuh_edpt_xfer(&xfer)) {
        midi_dev.tx_busy = true;
    }
}

bool usb_midi_host_receive_byte(uint8_t *byte) {
    return rx_ring_get(byte);
}

bool usb_midi_host_rx_available(void) {
    return rx_ring_available();
}
