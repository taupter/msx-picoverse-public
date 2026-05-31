// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// tusb_config.h - TinyUSB configuration for Sunrise IDE USB host support
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#if EXPLORER_USB_STDIO_DEBUG
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE
#else
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_HOST
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_NONE
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

#if EXPLORER_USB_STDIO_DEBUG

//--------------------------------------------------------------------
// DEVICE CONFIGURATION - temporary USB stdio diagnostics
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256

#else

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HUB                 1
#define CFG_TUH_CDC                 0
#define CFG_TUH_HID                 0
#define CFG_TUH_MIDI                0
#define CFG_TUH_MSC                 1
#define CFG_TUH_VENDOR              0

#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 4 : 1)

#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

#endif

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
