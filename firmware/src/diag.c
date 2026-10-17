/* Diagnostics readable from any host as string descriptors, for seeing what a host (or an audio offload
 * engine) actually sends while it streams: tools/firmware/diagstrings.py 40 49 ...  (all numbers are hex)
 *
 *   0x40       status registers: DMACTL0 (playback), DMACTL2 (capture), OEPCNF1, OEPBSIZ1, IEPCNF2,
 *              frame number, log positions, then the number of stale endpoint-0 IN interrupts ignored
 *              since boot (main.c ep0_in_done; wraps at 100h); since v0.24 also the sample rate (Hz, hex:
 *              BB80 or AC44) and the SET_CUR rate requests refused since boot (main.c set_rate; wraps)
 *   0x41-0x44  the last 16 setup packets, oldest first, 4 per string (diagnostic string reads excluded)
 *   0x45-0x46  at each of the last 32 SOFs: frame number (low byte), endpoint 1 X and Y count bytes
 *   0x47-0x48  the first 24 bytes of endpoint 1's X and Y buffers, read live
 *   0x49       counters since the stream started: SOFs, frames in which 0 / 1 / 2+ packets were seen
 *              arriving, buffer overflows (all wrap at 10000h since v0.23: take differences modulo
 *              10000h), then timer ticks per frame, histogram bin shift (one digit), the stream number (counts
 *              playback stream starts since boot, wraps at 100h: when it changes, the counters restarted)
 *   0x4A-0x4B  when packets arrive: 32 bins across the frame, counts per bin (time since SOF)
 *   0x4C-0x4D  when the DMA picks a buffer up (its NACK bit clears), same bins
 *   0x4F       reboot: pulls P1.2 low, which resets the TUSB3200, so it reloads the firmware from the
 *              EEPROM (lets a new image written over I2C start without replugging; the request
 *              itself never completes). Nothing else may drive P1.2 low.
 *   0x60-0x61  slack: how long each packet waited between arriving and the DMA taking it, 32 bins of
 *              1/16 frame (0-2 frames)
 *   0x70-0x73  set the DMA buffer mode (audio_set_dma_mode: 0x70 linear, 0x71 playback / 0x72 capture /
 *              0x73 both wrap-around, default ring sizes), so it can be changed from a PC without the
 *              WinUSB interface. Returns DMACTL0 and DMACTL2 afterwards
 * (v0.19 dropped v0.17's packet snapshot, 0x4E/0x50-0x5F, and the DMA phase experiment, 0x62/0x70-0x7F:
 * peeks through the WinUSB interface replace them.)
 *
 * Packet arrival is seen by polling the X/Y NACK bits from the main loop: the UBM sets a buffer's NACK
 * when a packet lands in it, the DMA clears it when it starts streaming that buffer (TI SLES018A
 * 2.2.7.4.1). If the DMA was already waiting, both happen between two polls and the frame counts as
 * "0 packets seen": that is a starved DMA either way. Times come from timer 0, restarted every SOF.
 * Control requests disturb this count: while a control transfer runs, about half the frames count as
 * "0 packets seen" (0.45 per SETUP and per 8-byte IN packet, measured on v0.23), though the SOF log
 * shows the buffers alternating as usual. The cost grows with the time the firmware spends on each
 * packet (a 1-byte read costs less than an 8-byte one), but calling diag_poll from endpoint 0's copy
 * loop made it worse: the transfers took twice as long and a third of the SOFs were lost (v0.23 test).
 * The X/Y bits read at the SOF itself don't tell whether a packet is waiting: in some streams both read
 * clear at nearly every SOF while polling sees each buffer held for a frame (v0.23, with Windows also
 * recording), so the SOF log shows when packets arrive, not how many.
 *
 * The logs live in the free end of the buffer space (FCC0h-FF27h), after the isochronous buffers. */
#include <stdint.h>
#include <8052.h>
#include "tusb3200.h"
#include "audio.h"
#include "diag.h"

#define SETUP_LOG ((volatile __xdata uint8_t *)0xFD00)   /* 16 x 8 bytes */
#define SOF_LOG   ((volatile __xdata uint8_t *)0xFD80)   /* 32 x 3 bytes */
#define ARRIVE    ((volatile __xdata uint16_t *)0xFDE0)  /* 32 bins */
#define PICKUP    ((volatile __xdata uint16_t *)0xFE20)  /* 32 bins */
#define STR_BUF   ((volatile __xdata uint8_t *)0xFE60)   /* the string descriptor being sent, up to 198 */
#define STR_END   (STR_BUF + 198)
#define SLACK     ((volatile __xdata uint16_t *)0xFCC0)  /* 32 bins, right after the capture buffers */

static __data uint8_t setup_head;
static __data uint8_t sof_head;
static volatile __xdata uint8_t *str_pos;    /* where the next character goes */
static __idata uint8_t prev_nack;            /* bit 0: X NACK, bit 1: Y NACK at the last poll */
static __idata uint8_t arrivals;             /* packets seen since the last SOF */
static __idata uint8_t bin_shift;            /* timer ticks >> bin_shift = histogram bin */
static __idata uint16_t ticks_per_frame;
static __idata uint16_t n_sof, n_frames[3], n_ovf;  /* wrap: a poller takes differences */
static __idata uint8_t n_streams;            /* playback stream starts since boot */
static __idata uint8_t n_stale;              /* stale endpoint-0 IN interrupts ignored since boot */
static __idata uint8_t n_refused;            /* sample rate changes refused since boot */
static __idata uint8_t rise_frame[2];        /* when each buffer's current packet arrived */
static __idata uint16_t rise_tick[2];

static void stats_clear(void)
{
    uint8_t i;
    for (i = 0; i < 32; i++) {
        ARRIVE[i] = 0;
        PICKUP[i] = 0;
        SLACK[i] = 0;
    }
    n_sof = n_frames[0] = n_frames[1] = n_frames[2] = n_ovf = 0;
    arrivals = 0;
    prev_nack = 0;
}

void diag_init(void)
{
    uint8_t i;
    for (i = 0; i < 128; i++)
        SETUP_LOG[i] = 0;
    for (i = 0; i < 96; i++)
        SOF_LOG[i] = 0;
    for (str_pos = STR_BUF; str_pos != STR_END; str_pos++)
        *str_pos = 0;                               /* put_char writes only the low bytes */
    setup_head = 0;
    sof_head = 0;
    ticks_per_frame = 0;
    bin_shift = 6;
    n_streams = 0;
    n_stale = 0;
    n_refused = 0;
    stats_clear();
    TMOD = (TMOD & 0xF0) | 0x01;                    /* timer 0: 16-bit, machine-cycle clock */
    TR0 = 1;
}

void diag_stream_start(void)
{
    stats_clear();
    n_streams++;
}

void diag_ep0_stale(void)
{
    n_stale++;
}

void diag_rate_refused(void)
{
    n_refused++;
}

void diag_setup(const __data uint8_t *setup)
{
    uint8_t i;
    volatile __xdata uint8_t *entry;

    if (setup[1] == 6 && setup[3] == 3 && setup[2] >= DIAG_FIRST_STRING)
        return;                                     /* reading the diagnostics shouldn't fill the log */
    entry = SETUP_LOG + 8 * setup_head;
    for (i = 0; i < 8; i++)
        entry[i] = setup[i];
    setup_head = (setup_head + 1) & 15;
}

static uint16_t timer_now(void)
{
    uint8_t h, l;
    do {
        h = TH0;
        l = TL0;
    } while (h != TH0);
    return ((uint16_t)h << 8) | l;
}

static void bump(volatile __xdata uint16_t *bins, uint16_t t)
{
    uint16_t b = t >> bin_shift;
    if (b > 31)
        b = 31;
    if (bins[b] != 0xFFFF)
        bins[b]++;
}

void diag_sof(void)
{
    uint16_t t = timer_now();
    uint8_t cnf;

    TR0 = 0;                                        /* restart the frame timer */
    TH0 = 0;
    TL0 = 0;
    TR0 = 1;
    if (t > 255) {                                  /* a whole frame: pick bins of about 1/32 frame */
        ticks_per_frame = t;
        for (bin_shift = 0; (t >> bin_shift) > 32; bin_shift++)
            ;
    }

    cnf = OEPCNF(1);
    if (!(cnf & EPCNF_EN))
        return;                                     /* only while the playback endpoint is configured */
    SOF_LOG[3 * sof_head] = USBFNL;
    SOF_LOG[3 * sof_head + 1] = OEPDCNTX(1);
    SOF_LOG[3 * sof_head + 2] = OEPDCNTY(1);
    sof_head = (sof_head + 1) & 31;

    n_sof++;
    n_frames[arrivals > 2 ? 2 : arrivals]++;
    arrivals = 0;
    if (cnf & 0x20) {                               /* OVF: a packet arrived with no room for it */
        n_ovf++;
        OEPCNF(1) = cnf & ~0x20;
    }
}

static void note_slack(uint8_t b, uint8_t frame, uint16_t t)
{
    /* frames elapsed x ticks per frame + ticks into this frame - ticks into the arrival frame */
    uint16_t dt = (uint8_t)(frame - rise_frame[b]) * ticks_per_frame + t - rise_tick[b];
    uint16_t bin = dt >> (bin_shift + 1);
    if (bin > 31)
        bin = 31;
    if (SLACK[bin] != 0xFFFF)
        SLACK[bin]++;
}

void diag_poll(void)
{
    uint8_t now, rose, fell, frame;
    uint16_t t;

    if (!(OEPCNF(1) & EPCNF_EN))
        return;
    now = ((OEPDCNTX(1) >> 7) & 1) | ((OEPDCNTY(1) >> 6) & 2);
    rose = now & ~prev_nack;
    fell = prev_nack & ~now;
    prev_nack = now;
    if (!(rose | fell))
        return;
    t = timer_now();
    frame = USBFNL;
    if (rose) {
        arrivals += (rose & 1) + (rose >> 1);
        bump(ARRIVE, t);
        if (rose & 1) { rise_frame[0] = frame; rise_tick[0] = t; }
        if (rose & 2) { rise_frame[1] = frame; rise_tick[1] = t; }
    }
    if (fell) {
        bump(PICKUP, t);
        if (fell & 1) note_slack(0, frame, t);
        if (fell & 2) note_slack(1, frame, t);
    }
}

/* While a string is built the main loop polls nothing: SOFs wait (two in one wait merge into one) and
 * packet arrivals go unseen. String 0x49 is read every second, so the writers avoid a call and a table
 * lookup per character: v0.22's took about 1 ms for 0x49, enough to serve an SOF a frame late. The
 * UTF-16 high bytes stay 0 from diag_init; only the low bytes are written. */
static void put_char(char c)
{
    volatile __xdata uint8_t *p = str_pos;
    if (p != STR_END) {
        *p = c;
        str_pos = p + 2;
    }
}

static void put_hex(uint8_t b)
{
    volatile __xdata uint8_t *p = str_pos;
    uint8_t c;
    if (p > STR_END - 4)
        return;
    c = (b >> 4) + '0';
    if (c > '9')
        c += 'A' - '9' - 1;
    p[0] = c;
    c = (b & 15) + '0';
    if (c > '9')
        c += 'A' - '9' - 1;
    p[2] = c;
    str_pos = p + 4;
}

static void put_hex16(uint16_t w)
{
    put_hex(w >> 8);
    put_hex(w & 0xFF);
}

static void put_bytes(volatile __xdata uint8_t *p, uint8_t n, uint8_t group)
{
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (i && i % group == 0)
            put_char(' ');
        put_hex(p[i]);
    }
}

static void put_bins(volatile __xdata uint16_t *bins)
{
    uint8_t i;
    for (i = 0; i < 16; i++) {
        if (i)
            put_char(' ');
        put_hex16(bins[i]);
    }
}

const uint8_t *diag_string(uint8_t index)
{
    uint8_t i, k;

    str_pos = STR_BUF + 2;
    switch (index) {
    case 0x40:
        put_hex(DMACTL0); put_char(' ');
        put_hex(DMACTL2); put_char(' ');
        put_hex(OEPCNF(1)); put_char(' ');
        put_hex(OEPBSIZ(1)); put_char(' ');
        put_hex(IEPCNF(2)); put_char(' ');
        put_hex(USBFNH); put_hex(USBFNL); put_char(' ');
        put_hex(setup_head); put_char(' ');
        put_hex(sof_head); put_char(' ');
        put_hex(n_stale); put_char(' ');
        put_hex16(audio_rate() == RATE_44K1 ? 44100 : 48000); put_char(' ');
        put_hex(n_refused);
        break;
    case 0x41: case 0x42: case 0x43: case 0x44:
        for (i = 0; i < 4; i++) {
            if (i)
                put_char(' ');
            k = (setup_head + 4 * (index - 0x41) + i) & 15;
            put_bytes(SETUP_LOG + 8 * k, 8, 8);
        }
        break;
    case 0x45: case 0x46:
        for (i = 0; i < 16; i++) {
            if (i)
                put_char(' ');
            k = (sof_head + 16 * (index - 0x45) + i) & 31;
            put_bytes(SOF_LOG + 3 * k, 3, 1);
        }
        break;
    case 0x47:
        put_bytes((volatile __xdata uint8_t *)EP1_X, 24, 3);
        break;
    case 0x48:
        put_bytes((volatile __xdata uint8_t *)EP1_Y, 24, 3);
        break;
    case 0x49:
        put_hex16(n_sof); put_char(' ');
        put_hex16(n_frames[0]); put_char(' ');
        put_hex16(n_frames[1]); put_char(' ');
        put_hex16(n_frames[2]); put_char(' ');
        put_hex16(n_ovf); put_char(' ');
        put_hex16(ticks_per_frame); put_char(' ');
        put_char(bin_shift < 10 ? '0' + bin_shift : 'A' - 10 + bin_shift);   /* one digit: 70 bytes, no zero-length end */
        put_char(' ');
        put_hex(n_streams);
        break;
    case 0x4A: case 0x4B:
        put_bins(ARRIVE + 16 * (index - 0x4A));
        break;
    case 0x4C: case 0x4D:
        put_bins(PICKUP + 16 * (index - 0x4C));
        break;
    case 0x60: case 0x61:
        put_bins(SLACK + 16 * (index - 0x60));
        break;
    case 0x70: case 0x71: case 0x72: case 0x73:
        audio_set_dma_mode(index - 0x70, 0, 0);
        put_hex(DMACTL0); put_char(' ');
        put_hex(DMACTL2);
        break;
    case 0x4F:
        P1 &= ~0x04;                                /* P1.2 low: the chip resets here */
        return 0;
    default:
        return 0;
    }
    STR_BUF[0] = (uint8_t)(str_pos - STR_BUF);
    STR_BUF[1] = 3;                                 /* string descriptor */
    return (const uint8_t *)STR_BUF;
}
