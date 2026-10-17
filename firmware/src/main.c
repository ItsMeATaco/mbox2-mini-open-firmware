/* Mbox 2 Mini open firmware for the TI TUSB3200A.
 *
 * A USB Audio Class 1.0 device: playback and recording stream between USB and the codec port by DMA,
 * with the master clock locked to USB frames (audio.c). Vendor requests expose registers and GPIO for
 * bring-up and EEPROM updates; on Windows they go through interface 3, which Windows binds to WinUSB.
 *
 * Written from TI's TUSB3200A data manual (SLES018A) and the USB 1.1 / USB Audio 1.0
 * specifications. The chip's interrupt sources are polled through VECINT; the 8052 interrupt
 * system stays off.
 */
#include <stdint.h>
#include <8052.h>
#include "tusb3200.h"
#include "descriptors.h"
#include "audio.h"
#include "diag.h"

/* Endpoint 0 buffers at the start of the buffer space */
#define EP0_OUT_BUF 0xF800
#define EP0_IN_BUF  (EP0_OUT_BUF + EP0_SIZE)

/* Standard requests */
#define REQ_GET_STATUS        0
#define REQ_CLEAR_FEATURE     1
#define REQ_SET_FEATURE       3
#define REQ_SET_ADDRESS       5
#define REQ_GET_DESCRIPTOR    6
#define REQ_GET_CONFIGURATION 8
#define REQ_SET_CONFIGURATION 9
#define REQ_GET_INTERFACE     10
#define REQ_SET_INTERFACE     11

#define DESC_DEVICE        1
#define DESC_CONFIGURATION 2
#define DESC_STRING        3

#define TYPE_MASK      0x60
#define TYPE_STANDARD  0x00
#define TYPE_CLASS     0x20
#define TYPE_VENDOR    0x40
#define RECIP_MASK     0x1F
#define RECIP_ENDPOINT 0x02

/* Audio class requests (USB Audio 1.0, A.9) */
#define UAC_SET_CUR 0x01
#define UAC_GET_CUR 0x81
#define UAC_GET_MIN 0x82
#define UAC_GET_MAX 0x83
#define UAC_SAMPLING_FREQ_CONTROL 0x01

/* Vendor requests for bring-up (device recipient), used by tools/firmware/fwdebug.py */
#define VREQ_PEEK     0x01  /* IN: wLength bytes of external data memory from address wValue */
#define VREQ_POKE     0x02  /* no data: write the low byte of wIndex to external data address wValue */
#define VREQ_PORTS    0x03  /* IN: P1, P3, codec port clock master flag */
#define VREQ_SET_PORT 0x04  /* no data: write the low byte of wValue to P1 (wIndex 1) or P3 (wIndex 3) */
#define VREQ_CLOCK    0x05  /* no data: wValue 1 = the TUSB3200 drives LRCK/SCLK, 0 = takes them as inputs */
#define VREQ_CODEC    0x06  /* no data: wValue codec port mode (nonzero = I2S mode 6, 0 = general purpose), wIndex CRESET level */
#define VREQ_MCLK     0x07  /* no data: wValue 0/1/2 = master clock 256/384/512 x the sample rate */
#define VREQ_CPTRAW   0x08  /* OUT, 8 bytes: CPTCNF1-4, CPTCTL, playback/capture DMA slots, MCLK ratio */
#define VREQ_CLKSTAT  0x09  /* IN, 12 bytes: frame number, MCLK capture, lock N trim, lock phase (16 bits each),
                             * sample rate in Hz (24 bits), streaming interfaces (bit 0 playback, bit 1 capture
                             * at alt 1); all LE. Up to v0.23: the first 8 bytes, at 48 kHz */
#define VREQ_LOCK     0x0A  /* no data: wValue 1 = clock lock running, 0 = trim frozen */
#define VREQ_DMAMODE  0x0B  /* no data: wValue bit 0 = playback, bit 1 = capture DMA in wrap-around mode (audio.c);
                             * wIndex ring sizes in 8-byte units, playback low byte, capture high byte (0 = default) */
/* Also answered whatever the recipient: MS_VENDOR_CODE, the Microsoft OS descriptors (descriptors.c). */

enum { EP0_IDLE, EP0_IN_DATA, EP0_OUT_DATA, EP0_STATUS_IN };

static __data uint8_t setup[8];
static const uint8_t *tx_ptr;           /* generic pointer: descriptors in code memory or replies in RAM */
static __data uint16_t tx_left;
static __data uint8_t tx_zlp;           /* the transfer is shorter than wLength and may need a zero-length end */
static __data uint8_t ep0_state;
static __data uint8_t new_address;      /* bit 7 set: apply after the status stage */
static __data uint8_t configuration;
static __data uint8_t alt_setting[NUM_INTERFACES];
static __data uint8_t reply[12];
static __data uint8_t raw_cfg_buf[8];

static const __code uint8_t rates[2][3] = {     /* RATE_48K, RATE_44K1: little-endian Hz */
    { 0x80, 0xBB, 0x00 },
    { 0x44, 0xAC, 0x00 },
};

static void ep0_configure(void)
{
    OEPBBAX(0) = BUF_ADDR(EP0_OUT_BUF);
    OEPBBAY(0) = BUF_ADDR(EP0_OUT_BUF);
    OEPBSIZ(0) = EP0_SIZE / 8;
    OEPDCNTX(0) = EPDCNT_NACK;
    OEPDCNTY(0) = EPDCNT_NACK;
    OEPCNF(0) = EPCNF_EN | EPCNF_IE;
    IEPBBAX(0) = BUF_ADDR(EP0_IN_BUF);
    IEPBBAY(0) = BUF_ADDR(EP0_IN_BUF);
    IEPBSIZ(0) = EP0_SIZE / 8;
    IEPDCNTX(0) = EPDCNT_NACK;
    IEPDCNTY(0) = EPDCNT_NACK;
    IEPCNF(0) = EPCNF_EN | EPCNF_IE;
}

static void usb_reset(void)
{
    uint8_t i;
    USBFADR = 0;
    new_address = 0;
    configuration = 0;
    for (i = 0; i < NUM_INTERFACES; i++)
        alt_setting[i] = 0;
    ep0_state = EP0_IDLE;
    tx_left = 0;
    tx_zlp = 0;
    playback_stop();
    ep0_configure();
    USBIMSK = USB_RSTR | USB_SETUP | USB_SOF;
    USBCTL = USBCTL_CONT | USBCTL_FEN;      /* FEN is cleared by every USB reset */
}

static void ep0_stall(void)
{
    OEPCNF(0) = EPCNF_EN | EPCNF_IE | EPCNF_STALL;
    IEPCNF(0) = EPCNF_EN | EPCNF_IE | EPCNF_STALL;
    ep0_state = EP0_IDLE;
}

/* Queue the next IN data packet (up to 8 bytes; zero bytes ends a transfer on a packet boundary). */
static void ep0_send_packet(void)
{
    volatile __xdata uint8_t *buf = (volatile __xdata uint8_t *)EP0_IN_BUF;
    uint8_t n = tx_left > EP0_SIZE ? EP0_SIZE : (uint8_t)tx_left;
    uint8_t i;
    for (i = 0; i < n; i++)
        buf[i] = *tx_ptr++;
    tx_left -= n;
    if (n < EP0_SIZE)
        tx_zlp = 0;                         /* a short packet ends the transfer by itself */
    IEPDCNTX(0) = n;                        /* also clears NACK: sent on the next IN token */
}

/* Control read: send `len` bytes (cut to wLength), then accept the host's zero-length status OUT. */
static void ep0_reply(const uint8_t *data, uint16_t len)
{
    uint16_t wlength = setup[6] | ((uint16_t)setup[7] << 8);
    if (len > wlength)
        len = wlength;
    tx_ptr = data;
    tx_left = len;
    tx_zlp = len < wlength;
    ep0_state = EP0_IN_DATA;
    IEPCNF(0) = EPCNF_EN | EPCNF_IE | EPCNF_TOGGLE;     /* the first data packet is DATA1 */
    OEPCNF(0) = EPCNF_EN | EPCNF_IE | EPCNF_TOGGLE;     /* status stage: zero-length DATA1 OUT */
    OEPDCNTX(0) = 0;
    ep0_send_packet();
}

/* Status stage of a control write: a zero-length DATA1 IN packet. */
static void ep0_status_in(void)
{
    IEPCNF(0) = EPCNF_EN | EPCNF_IE | EPCNF_TOGGLE;
    IEPDCNTX(0) = 0;
    ep0_state = EP0_STATUS_IN;
}

/* Control write with a data stage: accept one DATA1 OUT packet. */
static void ep0_receive(void)
{
    OEPCNF(0) = EPCNF_EN | EPCNF_IE | EPCNF_TOGGLE;
    OEPDCNTX(0) = 0;
    ep0_state = EP0_OUT_DATA;
}

static uint8_t get_descriptor(uint8_t type, uint8_t index)
{
    switch (type) {
    case DESC_DEVICE:
        ep0_reply(device_descriptor, DEVICE_DESC_LENGTH);
        return 1;
    case DESC_CONFIGURATION:
        if (index != 0)
            return 0;
        ep0_reply(config_descriptor, CONFIG_TOTAL_LENGTH);
        return 1;
    case DESC_STRING:
        if (index == MS_OS_STRING_INDEX) {
            ep0_reply(ms_os_string, MS_OS_STRING_LENGTH);
            return 1;
        }
        if (index >= DIAG_FIRST_STRING) {
            const uint8_t *d = diag_string(index);
            if (!d)
                return 0;
            ep0_reply(d, d[0]);
            return 1;
        }
        if (index >= NUM_STRINGS)
            return 0;
        ep0_reply(string_descriptors[index], string_descriptors[index][0]);
        return 1;
    }
    return 0;                               /* device qualifier etc.: not a high-speed device */
}

static uint8_t standard_request(void)
{
    uint8_t value = setup[2];
    uint8_t index = setup[4];
    uint8_t i;

    switch (setup[1]) {
    case REQ_GET_STATUS:
        reply[0] = 0;                       /* bus powered, no remote wake-up, no halted endpoint */
        reply[1] = 0;
        ep0_reply(reply, 2);
        return 1;
    case REQ_CLEAR_FEATURE:
    case REQ_SET_FEATURE:
        ep0_status_in();
        return 1;
    case REQ_SET_ADDRESS:
        new_address = 0x80 | (value & 0x7F);
        ep0_status_in();
        return 1;
    case REQ_GET_DESCRIPTOR:
        return get_descriptor(setup[3], value);
    case REQ_GET_CONFIGURATION:
        reply[0] = configuration;
        ep0_reply(reply, 1);
        return 1;
    case REQ_SET_CONFIGURATION:
        if (value > 1)
            return 0;
        configuration = value;
        for (i = 0; i < NUM_INTERFACES; i++)
            alt_setting[i] = 0;
        playback_stop();
        ep0_status_in();
        return 1;
    case REQ_GET_INTERFACE:
        if (!configuration || index >= NUM_INTERFACES)
            return 0;
        reply[0] = alt_setting[index];
        ep0_reply(reply, 1);
        return 1;
    case REQ_SET_INTERFACE:
        if (!configuration || index >= NUM_INTERFACES || value > (index == 1 || index == 2 ? 1 : 0))
            return 0;
        alt_setting[index] = value;
        if (index == 1) {
            if (value) {
                playback_start();
                diag_stream_start();
            } else
                playback_stop();
        }
        ep0_status_in();
        return 1;
    }
    return 0;
}

/* Audio class: only the sampling frequency control on the two streaming endpoints. The Mbox has one
 * master clock for both directions, so both endpoints' controls read the same rate (set_rate). */
static uint8_t class_request(void)
{
    if ((setup[0] & RECIP_MASK) != RECIP_ENDPOINT || setup[3] != UAC_SAMPLING_FREQ_CONTROL
        || (setup[4] != EP_PLAY && setup[4] != EP_REC))
        return 0;

    switch (setup[1]) {
    case UAC_SET_CUR:
        if (setup[6] != 3 || setup[7] != 0)
            return 0;
        ep0_receive();                      /* the rate comes in the data stage: ep0_out_done */
        return 1;
    case UAC_GET_CUR:
        ep0_reply(rates[audio_rate()], 3);
        return 1;
    case UAC_GET_MIN:
        ep0_reply(rates[RATE_44K1], 3);
        return 1;
    case UAC_GET_MAX:
        ep0_reply(rates[RATE_48K], 3);
        return 1;
    }
    return 0;
}

/* SET_CUR sampling frequency, on either streaming endpoint: 44100 or 48000 Hz. One clock serves both
 * directions, so a request for the other rate is refused while the other direction's interface is at
 * alt 1: its stream would carry on at the wrong rate (playback overrunning or starving, recordings
 * pitched). The stream that started first keeps its rate; the host sees the second one fail (the status
 * stage stalls). Otherwise the clock follows the request, before any audio of the new stream flows:
 * Windows sends it right after SET_INTERFACE alt 1 (seen on 2026-10-17). Returns 0 to refuse. */
static uint8_t set_rate(volatile __xdata uint8_t *buf)
{
    uint8_t r;

    if (buf[2] != 0)
        return 0;
    if (buf[0] == 0x80 && buf[1] == 0xBB)
        r = RATE_48K;
    else if (buf[0] == 0x44 && buf[1] == 0xAC)
        r = RATE_44K1;
    else
        return 0;
    if (r == audio_rate())
        return 1;
    if (setup[4] == EP_PLAY ? alt_setting[2] : alt_setting[1]) {
        diag_rate_refused();
        return 0;
    }
    audio_set_rate(r);
    return 1;
}

/* Microsoft OS 1.0 feature descriptors: wIndex 4 extended compat ID (device recipient), 5 extended
 * properties (interface recipient; only interface 3 has any, so wValue's interface number is not checked). */
static uint8_t ms_os_request(void)
{
    if (!(setup[0] & 0x80) || setup[5] != 0)
        return 0;
    if (setup[4] == 4) {
        ep0_reply(ms_compat_id, MS_COMPAT_ID_LENGTH);
        return 1;
    }
    if (setup[4] == 5) {
        ep0_reply(ms_properties, MS_PROPERTIES_LENGTH);
        return 1;
    }
    return 0;
}

static uint8_t vendor_request(void)
{
    uint16_t value = setup[2] | ((uint16_t)setup[3] << 8);

    if (setup[1] == MS_VENDOR_CODE)
        return ms_os_request();
    if ((setup[0] & RECIP_MASK) != 0)
        return 0;
    switch (setup[1]) {
    case VREQ_PEEK:
        ep0_reply((const uint8_t *)(__xdata uint8_t *)value, 0xFFFF);  /* cut to wLength */
        return 1;
    case VREQ_POKE:
        XREG(value) = setup[4];
        ep0_status_in();
        return 1;
    case VREQ_PORTS:
        reply[0] = P1;
        reply[1] = P3;
        reply[2] = audio_clock_master();
        ep0_reply(reply, 3);
        return 1;
    case VREQ_SET_PORT:
        if (setup[4] == 1)
            P1 = setup[2];
        else if (setup[4] == 3)
            P3 = setup[2];
        else
            return 0;
        ep0_status_in();
        return 1;
    case VREQ_CLOCK:
        audio_set_clock_master(setup[2]);
        ep0_status_in();
        return 1;
    case VREQ_CODEC:
        audio_set_codec_mode(setup[2], setup[4]);
        ep0_status_in();
        return 1;
    case VREQ_MCLK:
        audio_set_mclk_ratio(setup[2]);
        ep0_status_in();
        return 1;
    case VREQ_CLKSTAT: {
        int16_t u = audio_lock_u();
        int16_t phase = audio_lock_phase();
        reply[0] = USBFNL;
        reply[1] = USBFNH;
        reply[2] = ACGCAPL;
        reply[3] = ACGCAPH;
        reply[4] = (uint8_t)u;
        reply[5] = (uint8_t)((uint16_t)u >> 8);
        reply[6] = (uint8_t)phase;
        reply[7] = (uint8_t)((uint16_t)phase >> 8);
        reply[8] = rates[audio_rate()][0];
        reply[9] = rates[audio_rate()][1];
        reply[10] = 0;
        reply[11] = alt_setting[1] | (alt_setting[2] << 1);
        ep0_reply(reply, 12);
        return 1;
    }
    case VREQ_LOCK:
        audio_set_lock(setup[2]);
        ep0_status_in();
        return 1;
    case VREQ_DMAMODE:
        audio_set_dma_mode(setup[2], setup[4], setup[5]);
        ep0_status_in();
        return 1;
    case VREQ_CPTRAW:
        if (setup[6] != 8 || setup[7] != 0)
            return 0;
        ep0_receive();
        return 1;
    }
    return 0;
}

static void handle_setup(void)
{
    volatile __xdata uint8_t *packet = (volatile __xdata uint8_t *)SETUP_PACKET;
    uint8_t handled = 0;
    uint8_t i;

    for (i = 0; i < 8; i++)
        setup[i] = packet[i];
    diag_setup(setup);

    /* A new setup packet ends any transfer in progress and clears a stall. */
    OEPDCNTX(0) = EPDCNT_NACK;
    IEPDCNTX(0) = EPDCNT_NACK;
    OEPCNF(0) = EPCNF_EN | EPCNF_IE;
    IEPCNF(0) = EPCNF_EN | EPCNF_IE;
    ep0_state = EP0_IDLE;
    tx_left = 0;
    tx_zlp = 0;

    switch (setup[0] & TYPE_MASK) {
    case TYPE_STANDARD:
        handled = standard_request();
        break;
    case TYPE_CLASS:
        handled = class_request();
        break;
    case TYPE_VENDOR:
        handled = vendor_request();
        break;
    }
    if (!handled)
        ep0_stall();
    /* The caller's write to VECINT clears SETUP; until then the hardware NAKs data and status stages. */
}

/* Whether the packet queued on endpoint 0 IN has gone to the host: the UBM sets NACK once it has. Polled
 * for a few microseconds, in case the bit lags the interrupt. */
static uint8_t ep0_in_sent(void)
{
    uint8_t i;
    for (i = 0; i < 16; i++) {
        if (IEPDCNTX(0) & EPDCNT_NACK)
            return 1;
    }
    return 0;
}

/* An IN packet on endpoint 0 was acknowledged by the host. Or not: the interrupt of the previous
 * transfer's last packet can still be pending when the next SETUP is serviced (SETUP comes first), and
 * taking it for this transfer's first packet queued the second one over it before it was sent. The
 * host then got bytes 8-15 first, or byte 8 in place of byte 0, or nothing more (a timeout). Windows
 * dropped the corrupted extended compat ID that way at enumeration, which left the maintenance
 * interface without WinUSB (code 28, until v0.22). */
static void ep0_in_done(void)
{
    if (!ep0_in_sent()) {
        diag_ep0_stale();
        return;
    }
    if (ep0_state == EP0_IN_DATA) {
        if (tx_left || tx_zlp)
            ep0_send_packet();
        else
            ep0_state = EP0_IDLE;           /* the status OUT is already armed */
    } else if (ep0_state == EP0_STATUS_IN) {
        if (new_address & 0x80) {
            USBFADR = new_address & 0x7F;   /* the new address applies once the status stage is done */
            new_address = 0;
        }
        ep0_state = EP0_IDLE;
    }
}

/* An OUT packet on endpoint 0 arrived: SET_CUR data, or the status stage of a control read. */
static void ep0_out_done(void)
{
    volatile __xdata uint8_t *buf = (volatile __xdata uint8_t *)EP0_OUT_BUF;

    if (ep0_state != EP0_OUT_DATA)
        return;
    /* The UBM sets NACK once the data packet is in. Without it this is a stale interrupt, e.g. from
     * the previous control read's status stage when the next SETUP arrived before it was serviced
     * (SETUP is serviced first): treating it as the data dropped vendor writes (2026-10-16). */
    if (!(OEPDCNTX(0) & EPDCNT_NACK))
        return;
    if ((setup[0] & TYPE_MASK) == TYPE_VENDOR) {
        uint8_t i;
        if ((OEPDCNTX(0) & 0x7F) >= 8) {
            for (i = 0; i < 8; i++)
                raw_cfg_buf[i] = buf[i];
            audio_set_raw(raw_cfg_buf);
        }
        ep0_status_in();
        return;
    }
    if ((OEPDCNTX(0) & 0x7F) < 3 || !set_rate(buf)) {
        ep0_stall();                        /* the status stage stalls: the host sees the request fail */
        return;
    }
    ep0_status_in();
}

void main(void)
{
    uint8_t vector;
    uint8_t i;

    EA = 0;
    GLOBCTL = GLOBCTL_MCUCLK | GLOBCTL_LPWR;
    USBCTL = 0;
    USBIMSK = 0;
    for (i = 0; i < 64 && VECINT != VEC_NONE; i++)
        VECINT = 0;                         /* drop anything left pending by the boot loader */
    diag_init();
    audio_init();

    USBIMSK = USB_RSTR | USB_SETUP | USB_SOF;
    usb_reset();                            /* configures endpoint 0 and connects */

    for (;;) {
        vector = VECINT;
        if (vector == VEC_NONE) {
            diag_poll();
            continue;
        }
        if (vector == VEC_SETUP) {
            handle_setup();
            VECINT = 0;                     /* last: until SETUP is cleared the hardware NAKs the data and status stages */
            continue;
        }
        /* Any other source is cleared before it's handled, so the same event arriving meanwhile (the
         * host taking the IN packet just queued, a data packet after a stale OUT interrupt) raises it
         * again instead of being cleared with it. */
        VECINT = 0;                         /* clear this source; VECINT moves to the next one */
        switch (vector) {
        case VEC_RESET:
            usb_reset();
            break;
        case VEC_IEP0:
            ep0_in_done();
            break;
        case VEC_OEP0:
            ep0_out_done();
            break;
        case VEC_SOF:
            diag_sof();
            audio_sof();
            break;
        }
    }
}
