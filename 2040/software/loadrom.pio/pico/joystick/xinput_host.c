// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// xinput_host.c - Xbox controller USB host class driver for TinyUSB
//
// Implements a TinyUSB custom class driver that claims vendor-specific
// interfaces matching either:
//   - Xbox 360 / XInput (class 0xFF, subclass 0x5D, protocol 0x01)
//   - Xbox One / Series X|S / GIP (class 0xFF, subclass 0x47, protocol 0xD0)
//
// Xbox 360 controllers send 20-byte reports immediately after configuration.
// Xbox One controllers use the GIP protocol and require an initialisation
// packet (0x05 0x20 0x00 0x01 0x00) sent to the OUT endpoint before they
// start producing 18-byte input reports.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "xinput_host.h"
#include "joystick.h"

// -----------------------------------------------------------------------
// External joystick state — shared with joystick_main.c (Core 0 reads)
// -----------------------------------------------------------------------
extern volatile uint8_t joystick_state[2];

// -----------------------------------------------------------------------
// GIP init packet — sent on OUT endpoint to start Xbox One input
// -----------------------------------------------------------------------
static const uint8_t gip_init_power[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };

// -----------------------------------------------------------------------
// Per-device Xbox controller context
// -----------------------------------------------------------------------
#define XINPUT_MAX_DEVICES  2

typedef struct {
    bool         active;
    xbox_proto_t proto;        // Which protocol this device uses
    uint8_t      dev_addr;
    uint8_t      itf_num;
    uint8_t      ep_in;        // Interrupt IN endpoint
    uint16_t     ep_in_size;
    uint8_t      ep_out;       // Interrupt OUT endpoint (GIP init, 0 if none)
    uint8_t      msx_port;     // 0 or 1, 0xFF = unassigned
    bool         init_sent;    // GIP: init packet has been sent
    uint8_t      report_buf[64] __attribute__((aligned(4)));
    uint8_t      out_buf[8]     __attribute__((aligned(4)));  // For GIP init
} xinput_device_t;

static xinput_device_t xinput_devs[XINPUT_MAX_DEVICES];

// -----------------------------------------------------------------------
// MSX port allocation (independent of HID driver's allocation)
// -----------------------------------------------------------------------
static uint8_t xinput_allocate_port(void)
{
    bool used[2] = { false, false };
    for (int i = 0; i < XINPUT_MAX_DEVICES; i++) {
        if (xinput_devs[i].active && xinput_devs[i].msx_port < 2)
            used[xinput_devs[i].msx_port] = true;
    }
    if (!used[0]) return 0;
    if (!used[1]) return 1;
    return 0xFF;
}

static int xinput_find_free_slot(void)
{
    for (int i = 0; i < XINPUT_MAX_DEVICES; i++) {
        if (!xinput_devs[i].active) return i;
    }
    return -1;
}

// -----------------------------------------------------------------------
// Convert Xbox 360 (XInput) report to MSX joystick byte (active-low)
//
// PSG R14 bit layout:
//   bit 0 = Up, bit 1 = Down, bit 2 = Left, bit 3 = Right
//   bit 4 = Trigger A, bit 5 = Trigger B
//   bits 6-7 = 1 (unused)
// -----------------------------------------------------------------------
static uint8_t xinput_to_msx(const xinput_report_t *rpt)
{
    uint8_t result = 0xFF;  // All released
    uint16_t btn = rpt->buttons;

    // D-pad -> directions
    if (btn & XINPUT_BTN_DPAD_UP)    result &= ~(1u << 0);
    if (btn & XINPUT_BTN_DPAD_DOWN)  result &= ~(1u << 1);
    if (btn & XINPUT_BTN_DPAD_LEFT)  result &= ~(1u << 2);
    if (btn & XINPUT_BTN_DPAD_RIGHT) result &= ~(1u << 3);

    // Face buttons -> triggers: A -> Trigger A, B or X -> Trigger B
    if (btn & XINPUT_BTN_A)                       result &= ~(1u << 4);
    if (btn & (XINPUT_BTN_B | XINPUT_BTN_X))      result &= ~(1u << 5);

    // Left analog stick -> directions (with deadzone)
    // XInput stick: X positive = right, Y positive = up
    if (rpt->stick_lx < -XINPUT_STICK_DEADZONE) result &= ~(1u << 2);  // Left
    if (rpt->stick_lx >  XINPUT_STICK_DEADZONE) result &= ~(1u << 3);  // Right
    if (rpt->stick_ly >  XINPUT_STICK_DEADZONE) result &= ~(1u << 0);  // Up
    if (rpt->stick_ly < -XINPUT_STICK_DEADZONE) result &= ~(1u << 1);  // Down

    return result;
}

// -----------------------------------------------------------------------
// Convert Xbox One (GIP) report to MSX joystick byte (active-low)
// -----------------------------------------------------------------------
static uint8_t gip_to_msx(const gip_report_t *rpt)
{
    uint8_t result = 0xFF;  // All released
    uint16_t btn = rpt->buttons;

    // D-pad -> directions (bits 8-11 on Xbox One, vs 0-3 on Xbox 360)
    if (btn & GIP_BTN_DPAD_UP)    result &= ~(1u << 0);
    if (btn & GIP_BTN_DPAD_DOWN)  result &= ~(1u << 1);
    if (btn & GIP_BTN_DPAD_LEFT)  result &= ~(1u << 2);
    if (btn & GIP_BTN_DPAD_RIGHT) result &= ~(1u << 3);

    // Face buttons -> triggers: A -> Trigger A, B or X -> Trigger B
    if (btn & GIP_BTN_A)                     result &= ~(1u << 4);
    if (btn & (GIP_BTN_B | GIP_BTN_X))      result &= ~(1u << 5);

    // Left analog stick -> directions (with deadzone)
    // GIP stick convention matches XInput: X positive = right, Y positive = up
    if (rpt->stick_lx < -XINPUT_STICK_DEADZONE) result &= ~(1u << 2);  // Left
    if (rpt->stick_lx >  XINPUT_STICK_DEADZONE) result &= ~(1u << 3);  // Right
    if (rpt->stick_ly >  XINPUT_STICK_DEADZONE) result &= ~(1u << 0);  // Up
    if (rpt->stick_ly < -XINPUT_STICK_DEADZONE) result &= ~(1u << 1);  // Down

    return result;
}

// -----------------------------------------------------------------------
// TinyUSB custom class driver callbacks
// -----------------------------------------------------------------------

static bool xinput_driver_init(void)
{
    memset(xinput_devs, 0, sizeof(xinput_devs));
    return true;
}

static bool xinput_driver_deinit(void)
{
    return true;
}

static bool xinput_driver_open(uint8_t rhport, uint8_t dev_addr,
                               tusb_desc_interface_t const *desc_itf,
                               uint16_t max_len)
{
    (void)rhport;

    // Determine which protocol this interface matches
    xbox_proto_t proto;
    if (desc_itf->bInterfaceClass    == XINPUT_IF_CLASS &&
        desc_itf->bInterfaceSubClass == XINPUT_IF_SUBCLASS &&
        desc_itf->bInterfaceProtocol == XINPUT_IF_PROTOCOL)
    {
        proto = XBOX_PROTO_360;
    }
    else if (desc_itf->bInterfaceClass    == GIP_IF_CLASS &&
             desc_itf->bInterfaceSubClass == GIP_IF_SUBCLASS &&
             desc_itf->bInterfaceProtocol == GIP_IF_PROTOCOL)
    {
        proto = XBOX_PROTO_ONE;
    }
    else
    {
        return false;  // Not an Xbox controller
    }

    int slot = xinput_find_free_slot();
    if (slot < 0) return false;

    uint8_t port = xinput_allocate_port();
    if (port == 0xFF) return false;

    // Walk descriptors to find IN and OUT endpoints
    uint8_t const *p_desc = (uint8_t const *)desc_itf;
    uint16_t drv_len = 0;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t ep_in_size = 0;

    while (drv_len < max_len) {
        uint8_t len = tu_desc_len(p_desc);
        if (len == 0) break;

        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep =
                (tusb_desc_endpoint_t const *)p_desc;

            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN && ep_in == 0) {
                ep_in = ep->bEndpointAddress;
                ep_in_size = tu_edpt_packet_size(ep);
                tuh_edpt_open(dev_addr, ep);
            }
            else if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_OUT && ep_out == 0) {
                ep_out = ep->bEndpointAddress;
                tuh_edpt_open(dev_addr, ep);
            }
        }

        drv_len += len;
        p_desc = tu_desc_next(p_desc);

        // Stop if we hit another interface descriptor (next interface)
        if (drv_len < max_len && tu_desc_type(p_desc) == TUSB_DESC_INTERFACE)
            break;
    }

    if (ep_in == 0) return false;

    // Populate device slot
    xinput_device_t *dev = &xinput_devs[slot];
    memset(dev, 0, sizeof(*dev));
    dev->active     = true;
    dev->proto      = proto;
    dev->dev_addr   = dev_addr;
    dev->itf_num    = desc_itf->bInterfaceNumber;
    dev->ep_in      = ep_in;
    dev->ep_in_size = (ep_in_size > sizeof(dev->report_buf))
                        ? sizeof(dev->report_buf) : ep_in_size;
    dev->ep_out     = ep_out;
    dev->msx_port   = port;
    dev->init_sent  = false;

    return true;
}

static bool xinput_driver_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    for (int i = 0; i < XINPUT_MAX_DEVICES; i++) {
        xinput_device_t *dev = &xinput_devs[i];
        if (!dev->active || dev->dev_addr != dev_addr ||
            dev->itf_num != itf_num)
            continue;

        if (dev->proto == XBOX_PROTO_ONE && dev->ep_out != 0) {
            // Xbox One: send GIP init packet to wake the controller,
            // then start listening for input reports
            memcpy(dev->out_buf, gip_init_power, GIP_INIT_POWER_LEN);
            usbh_edpt_xfer(dev_addr, dev->ep_out,
                           dev->out_buf, GIP_INIT_POWER_LEN);
            dev->init_sent = true;
        }

        // Start receiving input reports
        usbh_edpt_xfer(dev_addr, dev->ep_in,
                       dev->report_buf, dev->ep_in_size);
        break;
    }

    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool xinput_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr,
                                   xfer_result_t result, uint32_t xferred_bytes)
{
    (void)result;

    for (int i = 0; i < XINPUT_MAX_DEVICES; i++) {
        xinput_device_t *dev = &xinput_devs[i];
        if (!dev->active || dev->dev_addr != dev_addr)
            continue;

        // OUT endpoint completion (GIP init packet sent) — nothing to do
        if (ep_addr == dev->ep_out)
            return true;

        if (ep_addr != dev->ep_in)
            continue;

        // Parse report based on protocol
        if (dev->proto == XBOX_PROTO_360) {
            // Xbox 360: 20-byte fixed report
            if (xferred_bytes >= sizeof(xinput_report_t)) {
                xinput_report_t const *rpt =
                    (xinput_report_t const *)dev->report_buf;
                if (rpt->msg_type == XINPUT_MSG_INPUT && dev->msx_port < 2) {
                    uint8_t joy = xinput_to_msx(rpt);
                    __dmb();
                    joystick_state[dev->msx_port] = joy;
                    __dmb();
                }
            }
        }
        else if (dev->proto == XBOX_PROTO_ONE) {
            // Xbox One: 18-byte GIP report (4 header + 14 payload)
            if (xferred_bytes >= sizeof(gip_report_t)) {
                gip_report_t const *rpt =
                    (gip_report_t const *)dev->report_buf;
                if (rpt->command == GIP_CMD_INPUT && dev->msx_port < 2) {
                    uint8_t joy = gip_to_msx(rpt);
                    __dmb();
                    joystick_state[dev->msx_port] = joy;
                    __dmb();
                }
            }
        }

        // Re-arm the transfer for the next report
        usbh_edpt_xfer(dev_addr, dev->ep_in,
                       dev->report_buf, dev->ep_in_size);
        return true;
    }

    return false;
}

static void xinput_driver_close(uint8_t dev_addr)
{
    for (int i = 0; i < XINPUT_MAX_DEVICES; i++) {
        if (xinput_devs[i].active && xinput_devs[i].dev_addr == dev_addr) {
            // Release MSX port — set state to idle
            if (xinput_devs[i].msx_port < 2)
                joystick_state[xinput_devs[i].msx_port] = 0xFF;
            xinput_devs[i].active = false;
        }
    }
}

// -----------------------------------------------------------------------
// Driver descriptor — referenced by joystick_main.c
// -----------------------------------------------------------------------
usbh_class_driver_t const xinput_class_driver = {
    .name       = "XINPUT",
    .init       = xinput_driver_init,
    .deinit     = xinput_driver_deinit,
    .open       = xinput_driver_open,
    .set_config = xinput_driver_set_config,
    .xfer_cb    = xinput_driver_xfer_cb,
    .close      = xinput_driver_close,
};
