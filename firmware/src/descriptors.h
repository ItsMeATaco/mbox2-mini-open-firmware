/* USB descriptors: a USB Audio Class 1.0 device with 24-bit stereo playback and 16-bit stereo capture
 * at 44.1 or 48 kHz, plus a vendor interface that Windows binds to WinUSB (Microsoft OS 1.0 descriptors). */
#ifndef DESCRIPTORS_H
#define DESCRIPTORS_H

#include <stdint.h>

#define EP0_SIZE            8
#define NUM_INTERFACES      4       /* 0 audio control, 1 playback streaming, 2 capture streaming, 3 maintenance */
#define MAINT_INTERFACE     3
#define EP_PLAY             0x01    /* isochronous OUT, adaptive */
#define EP_REC              0x82    /* isochronous IN, synchronous */
#define DEVICE_DESC_LENGTH  18
#define CONFIG_TOTAL_LENGTH 189
#define NUM_STRINGS         4

#define MS_OS_STRING_INDEX   0xEE
#define MS_VENDOR_CODE       0x20   /* bRequest of the Microsoft OS feature descriptor requests */
#define MS_OS_STRING_LENGTH  18
#define MS_COMPAT_ID_LENGTH  40     /* wIndex 4: header + one function section */
#define MS_PROPERTIES_LENGTH 146    /* wIndex 5: header + DeviceInterfaceGUIDs */

extern const __code uint8_t device_descriptor[DEVICE_DESC_LENGTH];
extern const __code uint8_t config_descriptor[CONFIG_TOTAL_LENGTH];
extern const __code uint8_t * const __code string_descriptors[NUM_STRINGS];
extern const __code uint8_t ms_os_string[MS_OS_STRING_LENGTH];
extern const __code uint8_t ms_compat_id[MS_COMPAT_ID_LENGTH];
extern const __code uint8_t ms_properties[MS_PROPERTIES_LENGTH];

#endif
