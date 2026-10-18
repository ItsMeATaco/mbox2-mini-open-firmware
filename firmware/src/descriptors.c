/* USB descriptors, from the USB 1.1 and USB Device Class Definition for Audio Devices 1.0 specifications.
 *
 * Topology:  IT 1 (USB streaming) -> OT 2 (speaker)       playback, interface 1, EP 0x01
 *            IT 3 (line in)       -> OT 4 (USB streaming)  capture,  interface 2, EP 0x82
 * No feature units: the Mbox's level controls are analog knobs.
 * Interface 3 is a vendor interface without endpoints for the bring-up and update requests (main.c).
 * Its Microsoft OS 1.0 descriptors (string 0xEE, extended compat ID "WINUSB", extended property
 * DeviceInterfaceGUIDs) make Windows bind its own WinUSB driver to it, with no INF, so tools on the
 * PC can send those requests while usbaudio.sys owns the audio interfaces (tools/firmware/winusb.py).
 */
#include "descriptors.h"

#define LE16(x) ((x) & 0xFF), ((x) >> 8)
#define LE24(x) ((x) & 0xFF), (((x) >> 8) & 0xFF), ((x) >> 16)
#define LE32(x) ((x) & 0xFF), (((x) >> 8) & 0xFF), 0, 0     /* values below 64 K */

#define VENDOR_ID      0x1209   /* pid.codes */
#define PRODUCT_ID     0x0001   /* pid.codes test PID, for private testing */
#define DEVICE_RELEASE 0x0027   /* bcdDevice: open firmware v0.27 (DAC reset after the clocks start) */
#define RATES          2, LE24(44100), LE24(48000)  /* bSamFreqType 2: discrete rates, one clock for both (main.c) */
#define MAX_PACKET     294      /* 49 frames x 2 channels x 3 bytes: 48 kHz plus adaptive headroom (44.1 kHz: 44-45) */
#define REC_MAX_PACKET 196      /* 49 frames x 2 channels x 2 bytes (16-bit: see audio.c) */
#define AC_CS_LENGTH   52       /* class-specific audio control descriptors: header + 2 IT + 2 OT */

const __code uint8_t device_descriptor[DEVICE_DESC_LENGTH] = {
    18, 0x01,               /* bLength, DEVICE */
    LE16(0x0200),           /* bcdUSB 2.0, full speed only: Windows asks no USB 1.x device for Microsoft OS
                             * descriptors (v0.19 said 1.1 and got no osvc; neither did any other 1.x device
                             * on the test PC, 2026-10-16). A full-speed 2.0 device stalls device_qualifier. */
    0x00, 0x00, 0x00,       /* class defined per interface */
    EP0_SIZE,               /* bMaxPacketSize0 */
    LE16(VENDOR_ID),
    LE16(PRODUCT_ID),
    LE16(DEVICE_RELEASE),
    1, 2, 0,                /* iManufacturer, iProduct, no serial number */
    1,                      /* bNumConfigurations */
};

const __code uint8_t config_descriptor[CONFIG_TOTAL_LENGTH] = {
    /* Configuration */
    9, 0x02, LE16(CONFIG_TOTAL_LENGTH),
    NUM_INTERFACES, 1, 0,   /* bNumInterfaces, bConfigurationValue, iConfiguration */
    0x80, 250,              /* bus powered, 500 mA (as the original firmware) */

    /* Interface 0: audio control */
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    /* Class-specific AC header: ADC 1.0, two streaming interfaces */
    10, 0x24, 0x01, LE16(0x0100), LE16(AC_CS_LENGTH), 2, 1, 2,
    /* IT 1: USB streaming, stereo */
    12, 0x24, 0x02, 1, LE16(0x0101), 0, 2, LE16(0x0003), 0, 0,
    /* OT 2: speaker, fed by IT 1 */
    9, 0x24, 0x03, 2, LE16(0x0301), 0, 1, 0,
    /* IT 3: line connector, stereo (input 1 left, input 2 right). Not "microphone" (0x0201): Windows 11
     * binds its Voice Clarity effect to microphone endpoints by default, and that mixes both inputs into
     * one processed voice channel in every shared-mode recording (measured with v0.21, 2026-10-16). */
    12, 0x24, 0x02, 3, LE16(0x0603), 0, 2, LE16(0x0003), 0, 0,
    /* OT 4: USB streaming, fed by IT 3 */
    9, 0x24, 0x03, 4, LE16(0x0101), 0, 3, 0,

    /* Interface 1, alt 0: playback, no bandwidth */
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0,
    /* Interface 1, alt 1: playback, 24-bit stereo PCM */
    9, 0x04, 1, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 1, 1, LE16(0x0001),                  /* AS general: terminal 1, delay 1, PCM */
    14, 0x24, 0x02, 1, 2, 3, 24, RATES,                 /* type I: 2 ch, 3-byte subframes, 24 bits, 44.1/48 kHz */
    9, 0x05, EP_PLAY, 0x09, LE16(MAX_PACKET), 1, 0, 0,  /* isochronous, adaptive, every frame */
    7, 0x25, 0x01, 0x01, 0, LE16(0),                    /* CS endpoint: sampling frequency control */

    /* Interface 2, alt 0: capture, no bandwidth */
    9, 0x04, 2, 0, 0, 0x01, 0x02, 0x00, 0,
    /* Interface 2, alt 1: capture, 16-bit stereo PCM (the codec port can't byte-reverse 24-bit input) */
    9, 0x04, 2, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 4, 1, LE16(0x0001),                  /* AS general: terminal 4, delay 1, PCM */
    14, 0x24, 0x02, 1, 2, 2, 16, RATES,                 /* type I: 2 ch, 2-byte subframes, 16 bits, 44.1/48 kHz */
    9, 0x05, EP_REC, 0x0D, LE16(REC_MAX_PACKET), 1, 0, 0,   /* isochronous, synchronous, every frame */
    7, 0x25, 0x01, 0x01, 0, LE16(0),

    /* Interface 3: vendor specific, no endpoints (maintenance: WinUSB on Windows) */
    9, 0x04, MAINT_INTERFACE, 0, 0, 0xFF, 0x00, 0x00, 3,
};

static const __code uint8_t string_languages[] = { 4, 0x03, LE16(0x0409) };

static const __code uint8_t string_manufacturer[] = {
    28, 0x03,
    'O', 0, 'p', 0, 'e', 0, 'n', 0, ' ', 0, 'f', 0, 'i', 0, 'r', 0, 'm', 0, 'w', 0, 'a', 0, 'r', 0, 'e', 0,
};

static const __code uint8_t string_product[] = {
    24, 0x03,
    'M', 0, 'b', 0, 'o', 0, 'x', 0, ' ', 0, '2', 0, ' ', 0, 'M', 0, 'i', 0, 'n', 0, 'i', 0,
};

static const __code uint8_t string_maintenance[] = {
    48, 0x03,
    'M', 0, 'b', 0, 'o', 0, 'x', 0, ' ', 0, '2', 0, ' ', 0, 'M', 0, 'i', 0, 'n', 0, 'i', 0, ' ', 0,
    'm', 0, 'a', 0, 'i', 0, 'n', 0, 't', 0, 'e', 0, 'n', 0, 'a', 0, 'n', 0, 'c', 0, 'e', 0,
};

const __code uint8_t * const __code string_descriptors[NUM_STRINGS] = {
    string_languages, string_manufacturer, string_product, string_maintenance,
};

/* Microsoft OS 1.0 descriptors (Microsoft's "Microsoft OS Descriptors" 1.0 specification). Windows reads
 * string 0xEE once per VID/PID/bcdDevice, then asks for the other two with vendor request MS_VENDOR_CODE. */
const __code uint8_t ms_os_string[MS_OS_STRING_LENGTH] = {
    18, 0x03,
    'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0,
    MS_VENDOR_CODE, 0,
};

const __code uint8_t ms_compat_id[MS_COMPAT_ID_LENGTH] = {
    LE32(MS_COMPAT_ID_LENGTH), LE16(0x0100), LE16(0x0004), 1, 0, 0, 0, 0, 0, 0, 0,
    MAINT_INTERFACE, 0x01,
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,             /* compatible ID */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* sub-compatible ID */
    0, 0, 0, 0, 0, 0,
};

/* DeviceInterfaceGUIDs (REG_MULTI_SZ) = {74C63992-EFBC-429E-9653-C0BAA33F1EBC}: how tools find the
 * interface (tools/firmware/winusb.py has the same GUID). */
const __code uint8_t ms_properties[MS_PROPERTIES_LENGTH] = {
    LE32(MS_PROPERTIES_LENGTH), LE16(0x0100), LE16(0x0005), LE16(1),
    LE32(136), LE32(7),                             /* section size, REG_MULTI_SZ */
    LE16(42),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0,
    'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    LE32(80),
    '{', 0, '7', 0, '4', 0, 'C', 0, '6', 0, '3', 0, '9', 0, '9', 0, '2', 0, '-', 0, 'E', 0, 'F', 0, 'B', 0,
    'C', 0, '-', 0, '4', 0, '2', 0, '9', 0, 'E', 0, '-', 0, '9', 0, '6', 0, '5', 0, '3', 0, '-', 0, 'C', 0,
    '0', 0, 'B', 0, 'A', 0, 'A', 0, '3', 0, '3', 0, 'F', 0, '1', 0, 'E', 0, 'B', 0, 'C', 0, '}', 0, 0, 0,
    0, 0,
};
