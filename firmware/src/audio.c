/* Audio path, from TI's TUSB3200A data manual (SLES018A) sections 2.2.7.4, 2.2.8, 2.2.12-2.2.13 and A.5.2-A.5.4.
 *
 * Master clock: the adaptive clock generator makes 12.288 MHz (256 x 48 kHz) on MCLKO, or 11.2896 MHz
 * (256 x 44.1 kHz) when the host picks 44.1 kHz: everything else on the codec port follows MCLK.
 * Codec port: I2S mode 6, 2 slots of 32 SCLK, 24 data bits, SCLK = MCLK / 4 = 3.072 MHz, LRCK low for
 * the left slot, data one SCLK after the LRCK edge (DDLY), byte reversal (BYOR) ON.
 * The DAC (CS4392) takes standard I2S. Measured on 2026-10-16 by recording the headphone output
 * against Avid's firmware 1.40:
 *   - this setting: THD -86 / -94 / -83 dB at -30 / -18 / -9 dBFS (Avid: -90 / -99 / -97 dB);
 *   - without DDLY a -3 dBFS tone wraps around: the DAC reads every sample shifted left one bit;
 *   - BYOR off in the I2S modes: grindy garbage. The port's 24-bit shift register wants each sample's
 *     bytes reversed relative to USB audio's little-endian order;
 *   - general-purpose mode (0) with the same timing plays, but only the top 8 bits of each sample reach
 *     the DAC (whatever the data-bits-per-slot setting), and BYOR on there is garbage.
 *
 * The TUSB3200 drives LRCK/SCLK: measured 2026-10-16 with both set as inputs, the capture DMA saw no
 * clock at all (0 samples per frame), so nothing else on the board drives them; as outputs the port
 * runs at 48 samples per frame. A vendor request can still switch them back to inputs.
 *
 * Capture: the ADC's (CS5361) data reaches pin 39: SDIN2 in mode 6 (DMA time slots 1 and 5), SDIN1 in
 * mode 5. Probed on 2026-10-16: pin 38 (SDIN1 in mode 6) is unconnected, it follows a weak pull
 * either way, and pin 40 is held low; recording SDIN1 in mode 6 is why the ADC first looked dead.
 * Measured the same day:
 *   - on DMA channel 1 the samples come out garbled (a word mixing two samples, or one byte three
 *     times), on channels 0, 2 and 3 they arrive whole;
 *   - with BYOR on, 3- and 4-byte captures repeat one byte; 2-byte captures are correct
 *     little-endian. BYOR off gives whole samples, but big-endian, and playback needs BYOR on.
 * So capture takes the top 16 bits of each 24-bit sample, 2 bytes per slot, on DMA channel 2, while
 * the port stays at 24 data bits for playback. Mode 6 rather than 5: it keeps pins 38-40 all inputs
 * (mode 5 drives SDOUT2 onto pin 38) and is the setting playback was measured with.
 * Capture DMA starts once, at boot, and runs all the time: the IN endpoint's per-frame sample count
 * shows the port's rate. Restarting it at run time is unsafe: about one restart in ten (2026-10-16,
 * v0.19) leaves it writing one byte twice per sample (both bytes of every
 * sample equal), and it stays that way through further restarts until the chip is reset. Only the
 * bring-up request that changes the capture DMA mode restarts it.
 *
 * Buffers: each DMA channel works its endpoint's X/Y buffers in one of two modes (DMACTLn.WABEN):
 *   - linear: ping-pong between X and Y, the UBM choosing by the frame number's parity (SLES018A
 *     2.2.7.4). Fine while the host's transfers come at the start of each frame (Linux), but on
 *     Windows 11 they land 25-60 % into the frame and playback came out static-y and right-only,
 *     recording silent after 0.1 s (v0.18, 2026-10-16);
 *   - wrap-around: the TUSB3200A manual only names the bit. TI's TAS1020B manual (SLES025B
 *     2.2.7.4.1) describes one ring at the X base, sized by BSIZ. Measured here instead (2026-10-16,
 *     by dumping the buffers): packets still alternate between X and Y, and each of X and Y
 *     is its own ring of BSIZ x 8 bytes from its base address, the next packet going where the
 *     previous one in that buffer ended. So X and Y each get half the region.
 */
#include <8052.h>
#include "audio.h"
#include "tusb3200.h"
#include "descriptors.h"

/* GPIO port 1 on the Mbox 2 Mini, found by listening tests on 2026-10-16 (all outputs are active low):
 *   P1.1 + P1.3  both low = analog outputs on (either one alone stays silent)
 *   P1.0/P1.4/P1.5  low = front "USB" light (one of the three; the other two not yet identified)
 *   P1.2  low resets the whole TUSB3200: never drive it low
 *   P1.6/P1.7  inputs: they follow the converters' clock state
 * P3.0/P3.1 are the UART (MIDI on the full Mbox 2); P3.3-P3.5 showed no effect. */
#define P1_RUNNING 0xC4                     /* P1.0, P1.1, P1.3, P1.4, P1.5 low */

#define FRAME_BYTES 6                       /* playback: 2 channels x 3 bytes */
#define CAPTURE_BYTES 2                     /* capture: 2 bytes per channel (see above) */

#define I2S_SLOTS_SDOUT1 0x11               /* DMA time slots 0 and 4: SDOUT1 left and right (the DAC) */
#define I2S_SLOTS_SDIN2 0x22                /* DMA time slots 1 and 5: SDIN2 left and right (the ADC) */
#define CPT_MODE_I2S_6 6
#define CPT_FORMAT (CPTCNF3_DDLY | CPTCNF3_CSCLKP | CPTCNF3_CSYNCL | CPTCNF3_BYOR)   /* I2S, 50 % LRCK */
#define CPT_CMD_SLOT_UNUSED 0xF0            /* CPTCNF4 bits 7:4: command/status slot 15, outside the frame */
#define CPT_INPUT_CLOCKS (CPTCNF3_CSCLKD | CPTCNF3_CSYNCD)

/* General purpose (bring-up only): CRESET (pin 39) is an output, CDATI (38) the input. Pin 39 carries
 * the ADC's data, so this mode drives against it; it only exists for experiments. */
#define CPT_MODE_GP 0
#define GP_SLOTS 0x03                       /* general purpose: DMA slot bits match codec slots 0 and 1 */

static __data uint8_t clock_master;
static __data uint8_t cpt_mode;
static __data uint8_t cpt_creset;
static __data uint8_t mclk_ratio;
static __data uint8_t cpt_cfg[7];           /* CPTCNF1-4, CPTCTL, playback DMA slots, capture DMA slots */
static __data uint8_t raw_cfg;              /* nonzero: cpt_cfg came from audio_set_raw() */
static __data uint8_t dma_mode;             /* DMA_WAB_PLAY / DMA_WAB_REC: wrap-around buffers */
static __data uint8_t ring_play, ring_rec;  /* wrap-around: size of each of X and Y, 8-byte units */
#ifndef BOOT_DMA_MODE
#define BOOT_DMA_MODE DMA_WAB_PLAY          /* playback wrap-around, capture linear (see the buffer note above) */
#endif
#ifdef BOOT_CPT
/* Bring-up builds: a whole codec port setting as the boot default, in audio_set_raw()'s layout.
 * Switching into or within general-purpose mode at run time stalls the playback DMA until a power
 * cycle (v0.10, 2026-10-16), so mode-0 variants can only be tried this way. */
static const __code uint8_t boot_cpt[8] = { BOOT_CPT };
#endif

/* Clock lock (stage 3): keep MCLK at exactly 256/384/512 x the sample rate as measured against USB
 * frames. Each SOF the capture register holds MCLKO's free-running 16-bit count; the difference between
 * two SOFs minus the nominal count is the frame's error in MCLK cycles. A PI loop on the accumulated
 * error (phase) trims the synthesizer's N. One LSB of N moves 12.288 MHz by about 0.96 Hz, so one MCLK
 * cycle per frame (1 kHz) is about 1042 LSB (11.2896 MHz: 0.81 Hz, 1234 LSB). Gains: u = 8 * phase +
 * integral / 32, natural frequency ~1 Hz, damping ~0.7. Measured before locking: the free-running clock
 * was +136 ppm (+1.67 cycles per frame) on 2026-10-16, +7 ppm (N trim +91) on 2026-10-17.
 * At 44.1 kHz a frame holds a fractional count (11289.6 at 256 fs): the expected count carries the
 * tenths from frame to frame (lock_tenths), so ten frames expect exactly 112896. */
#define LOCK_U_MAX 4096                     /* about +-320 ppm */
#define LOCK_I_MAX (LOCK_U_MAX * 32L)
/* MCLK cycles per 1 ms frame per rate and ratio: whole cycles, and tenths */
static const __code uint16_t mclk_nominal[2][3] = { { 12288, 18432, 24576 }, { 11289, 16934, 22579 } };
static const __code uint8_t mclk_tenths[2][3] = { { 0, 0, 0 }, { 6, 4, 2 } };
static __data uint8_t lock_running;
static __data uint8_t lock_skip;            /* SOFs to pass by before the next measurement starts */
static __data uint16_t lock_frame;
static __data uint16_t lock_cap;
static __data uint8_t lock_tenths;
static __data int16_t lock_phase;
static __data int32_t lock_integral;
static __data int16_t lock_u;
static __data uint8_t lock_enabled;         /* 0: the trim stays frozen at its last value */
static __data uint8_t rate;                 /* RATE_48K or RATE_44K1: one clock for both directions */

/* Master clock per rate and ratio: synthesizer N (600 MHz / MCLK, 6.18 fixed point) and SCLK divider
 * (64 fs). MCLKO = the synthesizer (divide by M = 1). TI's manual gives 12-25 MHz for the synthesizer,
 * but 11.2896 MHz works: measured 2026-10-17 by setting N over USB with the lock frozen, it counted
 * 11289.598-11289.600 cycles per frame. */
static const __code uint8_t mclk_frq[2][3][3] = {
    {
        { 0xC3, 0x50, 0x00 },               /* 12.288 MHz: N = 48.828125 */
        { 0x82, 0x35, 0x55 },               /* 18.432 MHz: N = 32.552083 */
        { 0x61, 0xA8, 0x00 },               /* 24.576 MHz: N = 24.4140625 */
    }, {
        { 0xD4, 0x95, 0xC5 },               /* 11.2896 MHz: N = 53.146259 (-0.015 ppm) */
        { 0x8D, 0xB9, 0x2E },               /* 16.9344 MHz: N = 35.430839 (-0.015 ppm) */
        { 0x6A, 0x4A, 0xE2 },               /* 22.5792 MHz: N = 26.573129 (+0.057 ppm) */
    },
};
static const __code uint8_t sclk_divb[3] = { 3, 5, 7 };   /* MCLK / 4, / 6, / 8 */

static void write_frequency(int16_t u)
{
    const __code uint8_t *f = mclk_frq[rate][mclk_ratio];
    uint32_t n = ((uint32_t)f[0] << 16) | ((uint16_t)f[1] << 8) | f[2];
    n += u;
    ACGFRQ2 = (uint8_t)(n >> 16);
    ACGFRQ1 = (uint8_t)(n >> 8);
    ACGFRQ0 = (uint8_t)n;                           /* loads all 24 bits at once */
}

static void lock_reset(void)
{
    lock_running = 0;
    lock_phase = 0;
    lock_integral = 0;
    lock_u = 0;
}

/* Called for every USB start-of-frame. */
void audio_sof(void)
{
    uint16_t frame = (USBFNL | ((uint16_t)USBFNH << 8)) & 0x7FF;
    uint16_t cap = ACGCAPL | ((uint16_t)ACGCAPH << 8);
    uint16_t frames, nominal;
    int16_t err;
    int32_t u;

    if (!lock_enabled)
        return;
    if (lock_skip) {
        lock_skip--;
        return;
    }
    if (!lock_running) {
        lock_frame = frame;
        lock_cap = cap;
        lock_tenths = 0;
        lock_running = 1;
        return;
    }
    frames = (frame - lock_frame) & 0x7FF;
    lock_frame = frame;
    if (frames == 0 || frames > 4) {                /* missed SOFs: start the measurement over */
        lock_cap = cap;
        return;
    }
    nominal = frames * mclk_nominal[rate][mclk_ratio];  /* modulo 10000h, like the capture register */
    lock_tenths += frames * mclk_tenths[rate][mclk_ratio];
    while (lock_tenths >= 10) {
        lock_tenths -= 10;
        nominal++;
    }
    err = (int16_t)(cap - lock_cap - nominal);
    lock_cap = cap;

    lock_phase += err;
    if (lock_phase > 30000)
        lock_phase = 30000;
    else if (lock_phase < -30000)
        lock_phase = -30000;
    lock_integral += lock_phase;
    if (lock_integral > LOCK_I_MAX)
        lock_integral = LOCK_I_MAX;
    else if (lock_integral < -LOCK_I_MAX)
        lock_integral = -LOCK_I_MAX;
    u = ((int32_t)lock_phase << 3) + (lock_integral >> 5);
    if (u > LOCK_U_MAX)
        u = LOCK_U_MAX;
    else if (u < -LOCK_U_MAX)
        u = -LOCK_U_MAX;
    if ((int16_t)u != lock_u) {
        lock_u = (int16_t)u;
        write_frequency(lock_u);                    /* higher N = lower frequency */
    }
}

void audio_set_lock(uint8_t enabled)
{
    lock_enabled = enabled ? 1 : 0;
    lock_running = 0;                               /* restart the measurement when it resumes */
}

int16_t audio_lock_u(void)
{
    return lock_u;
}

int16_t audio_lock_phase(void)
{
    return lock_phase;
}

/* Sample rate of both directions (stage 5): only the synthesizer's frequency changes. The codec port, its
 * SCLK divider and the DMA channels stay as they are, so the converters get the same clock ratios at a
 * new frequency, and the capture DMA keeps running (restarting it can break recording, see above).
 * Run-time codec port changes have stalled the playback DMA and jammed the DAC (codec_port_configure).
 * The synthesizer takes a new N at once: the frame after a step counts exactly (measured 2026-10-17).
 * The trim carries over, scaled like N, which goes with 1 / rate (48000 / 44100 = 160 / 147). The lock
 * starts measuring afresh at the second SOF from now: the frame in progress counts cycles at both
 * frequencies, and an SOF may already be waiting with a count captured before the switch (taking that
 * as the start put up to 1000 cycles of error into the loop: 20-40 cycles of phase and an 18 ppm swing
 * that took a second to settle, v0.24). */
void audio_set_rate(uint8_t r)
{
    if (r == rate || r > RATE_44K1)
        return;
    lock_u = (int16_t)(((int32_t)lock_u * (r == RATE_44K1 ? 1115 : 941)) >> 10);  /* x 160/147 or x 147/160 */
    rate = r;
    lock_running = 0;
    lock_skip = 1;
    lock_phase = 0;
    lock_integral = (int32_t)lock_u << 5;           /* u = 8 * phase + integral / 32 */
    write_frequency(lock_u);
}

uint8_t audio_rate(void)
{
    return rate;
}

static void cpt_cfg_defaults(void)
{
    uint8_t gp = cpt_mode == CPT_MODE_GP;
    cpt_cfg[0] = (1 << 3) | cpt_mode;               /* 2 time slots per frame */
    cpt_cfg[1] = (4 << 3) | 5;                      /* 24 data bits per slot; 32 SCLK per slot (slot 0 too) */
    cpt_cfg[2] = CPT_FORMAT | (clock_master ? 0 : CPT_INPUT_CLOCKS);
    cpt_cfg[3] = (gp ? CPT_CMD_SLOT_UNUSED : 0) | sclk_divb[mclk_ratio];   /* SCLK = 64 fs */
    cpt_cfg[4] = cpt_creset ? CPTCTL_CRST : 0;
    cpt_cfg[5] = gp ? GP_SLOTS : I2S_SLOTS_SDOUT1;
    cpt_cfg[6] = gp ? GP_SLOTS : I2S_SLOTS_SDIN2;
}

/* Reconfigure clocks and codec port in one go: DMA off, port off, change, port on, DMA back.
 * At boot this is reliable. Later (vendor requests only) it can leave the playback DMA stalled until a
 * power cycle: both OUT buffers stay full with NACK set. Seen after switches into, within and out of
 * general-purpose mode and once between two I2S settings (v0.10-v0.13, 2026-10-16). */
static void codec_port_configure(void)
{
    uint8_t dma0 = DMACTL0;
    uint8_t dma2 = DMACTL2;

    if (!raw_cfg)
        cpt_cfg_defaults();
    DMACTL0 = 0;
    DMACTL2 = 0;
    GLOBCTL &= ~GLOBCTL_CPTEN;                      /* configuration registers are writable only while off */
    lock_reset();
    write_frequency(0);
    CPTCNF1 = cpt_cfg[0];
    CPTCNF2 = cpt_cfg[1];
    CPTCNF3 = cpt_cfg[2];
    CPTCNF4 = cpt_cfg[3];
    CPTCTL = cpt_cfg[4];
    DMATSL0 = cpt_cfg[5];
    DMATSL2 = cpt_cfg[6];
    GLOBCTL |= GLOBCTL_CPTEN;
    DMACTL2 = dma2;
    DMACTL0 = dma0;
}

static void master_clock_init(void)
{
    write_frequency(0);                             /* a valid frequency before the output is enabled */
    ACGDCTL = 0x00;                                 /* divide by M = 1, divide by I = 1 */
    ACGCTL = ACGCTL_MCLKEN;                         /* MCLKO on; the capture counter counts MCLKO */
    ACGCTL = ACGCTL_MCLKEN | ACGCTL_DIVEN;
}

/* Capture: endpoint 2 IN, DMA channel 2 from SDIN2 (pin 39 in mode 6), 16 bits per channel. */
void capture_start(void)
{
    uint8_t wab = dma_mode & DMA_WAB_REC ? DMACTL_WAB : 0;
    uint8_t units = wab ? ring_rec : EP2_BUF / 8;

    DMACTL2 = 0;
    IEPCNF(2) = 0;
    IEPBBAX(2) = BUF_ADDR(EP2_X);
    IEPBBAY(2) = BUF_ADDR(EP2_X) + units;
    IEPBSIZ(2) = units;
    IEPDCNTX(2) = EPDCNT_NACK;
    IEPDCNTY(2) = EPDCNT_NACK;
    DMATSL2 = cpt_cfg[6];
    DMATSH2 = DMATSH_BYTES(CAPTURE_BYTES);
    DMACTL2 = wab | DMACTL_IN | 2;                  /* the UBM reads the mode bit too: set it first */
    IEPCNF(2) = EPCNF_EN | EPCNF_ISO | EPCNF_BPS(2 * CAPTURE_BYTES);
    DMACTL2 = DMACTL_EN | wab | DMACTL_IN | 2;
}

void audio_init(void)
{
    clock_master = 1;
    cpt_mode = CPT_MODE_I2S_6;
    cpt_creset = 0;                                 /* CRESET only exists in general-purpose mode */
    mclk_ratio = 0;
    rate = RATE_48K;
    raw_cfg = 0;
    dma_mode = BOOT_DMA_MODE;
    ring_play = EP1_REGION / 16;
    ring_rec = EP2_REGION / 16;
#ifdef BOOT_CPT
    {
        uint8_t i;
        for (i = 0; i < 7; i++)
            cpt_cfg[i] = boot_cpt[i];
        mclk_ratio = boot_cpt[7] < 3 ? boot_cpt[7] : 0;
        raw_cfg = 1;
    }
#endif
    lock_reset();
    lock_enabled = 1;
    master_clock_init();
    codec_port_configure();
    capture_start();
    P1 = P1_RUNNING;                                /* clocks are running: outputs on, USB light on */
}

void audio_set_clock_master(uint8_t master)
{
    clock_master = master ? 1 : 0;
    raw_cfg = 0;
    codec_port_configure();
}

uint8_t audio_clock_master(void)
{
    return clock_master;
}

void audio_set_codec_mode(uint8_t mode, uint8_t creset)
{
    cpt_mode = mode == CPT_MODE_GP ? CPT_MODE_GP : CPT_MODE_I2S_6;
    cpt_creset = creset ? 1 : 0;
    raw_cfg = 0;
    codec_port_configure();
}

void audio_set_mclk_ratio(uint8_t ratio)
{
    mclk_ratio = ratio < 3 ? ratio : 0;
    raw_cfg = 0;
    codec_port_configure();
}

/* Bring-up: apply a whole codec port configuration at once (cfg: CPTCNF1-4, CPTCTL, playback and
 * capture DMA slot masks, master clock ratio 0/1/2). */
void audio_set_raw(const __data uint8_t *cfg)
{
    uint8_t i;
    for (i = 0; i < 7; i++)
        cpt_cfg[i] = cfg[i];
    mclk_ratio = cfg[7] < 3 ? cfg[7] : 0;
    raw_cfg = 1;
    codec_port_configure();
}

/* Bring-up: restarts playback if it streams, and capture if its mode or ring changes (see the capture
 * note above: a capture restart can break recording until a reboot). */
void audio_set_dma_mode(uint8_t mode, uint8_t play, uint8_t rec)
{
    uint8_t old_mode = dma_mode & DMA_WAB_REC;
    uint8_t old_ring = ring_rec;

    dma_mode = mode & (DMA_WAB_PLAY | DMA_WAB_REC);
    ring_play = play && play <= EP1_REGION / 16 ? play : EP1_REGION / 16;
    ring_rec = rec && rec <= EP2_REGION / 16 ? rec : EP2_REGION / 16;
    if (OEPCNF(1) & EPCNF_EN)
        playback_start();
    if ((dma_mode & DMA_WAB_REC) != old_mode || ring_rec != old_ring)
        capture_start();
}

/* Playback: endpoint 1 OUT, DMA channel 0 to SDOUT1 (pin 36). */
void playback_start(void)
{
    uint8_t wab = dma_mode & DMA_WAB_PLAY ? DMACTL_WAB : 0;
    uint8_t units = wab ? ring_play : EP1_BUF / 8;

    DMACTL0 = 0;
    OEPCNF(1) = 0;
    OEPBBAX(1) = BUF_ADDR(EP1_X);
    OEPBBAY(1) = BUF_ADDR(EP1_X) + units;
    OEPBSIZ(1) = units;
    OEPDCNTX(1) = 0;
    OEPDCNTY(1) = 0;
    DMATSL0 = cpt_cfg[5];
    DMATSH0 = DMATSH_BYTES(3);
    DMACTL0 = wab | 1;                              /* the UBM reads the mode bit too: set it first */
    OEPCNF(1) = EPCNF_EN | EPCNF_ISO | EPCNF_BPS(FRAME_BYTES);
    DMACTL0 = DMACTL_EN | wab | 1;
}

void playback_stop(void)
{
    DMACTL0 = 0;
    OEPCNF(1) = 0;
}
