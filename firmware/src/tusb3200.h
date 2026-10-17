/* TUSB3200A memory-mapped registers, endpoint configuration blocks and buffer space.
 * Addresses and bits from TI's data manual SLES018A, appendix A.
 *
 * External data memory in normal operating mode (2 KB at F800h-FFFFh):
 *   F800h-FF27h  endpoint data buffers (1832 bytes)
 *   FF28h-FF2Fh  control endpoint setup packet
 *   FF30h-FFAFh  endpoint configuration blocks (16 x 8 bytes)
 *   FFB0h-FFFFh  registers
 * There is no general-purpose external RAM: firmware variables must live in internal RAM.
 */
#ifndef TUSB3200_H
#define TUSB3200_H

#include <stdint.h>

#define XREG(addr) (*(volatile __xdata uint8_t *)(addr))

/* USB registers */
#define USBFADR XREG(0xFFFF)    /* function address */
#define USBSTA  XREG(0xFFFE)    /* status (read only; cleared by writing VECINT) */
#define USBIMSK XREG(0xFFFD)    /* interrupt mask */
#define USBCTL  XREG(0xFFFC)    /* control */
#define USBFNL  XREG(0xFFFB)    /* frame number, low byte */
#define USBFNH  XREG(0xFFFA)    /* frame number, high 3 bits */

/* Miscellaneous registers */
#define OEPINT  XREG(0xFFB4)    /* out endpoint interrupt status (diagnostic) */
#define IEPINT  XREG(0xFFB3)    /* in endpoint interrupt status (diagnostic) */
#define VECINT  XREG(0xFFB2)    /* interrupt vector; write any value to clear and advance */
#define GLOBCTL XREG(0xFFB1)    /* global control */
#define MEMCFG  XREG(0xFFB0)    /* memory configuration */

/* USBCTL */
#define USBCTL_CONT  0x80       /* connect the D+ pull-up */
#define USBCTL_FEN   0x40       /* respond to USB transactions; cleared by a USB reset */
#define USBCTL_RWUP  0x20       /* remote wake-up signalling */
#define USBCTL_FRSTE 0x10       /* USB reset also resets the MCU */

/* USBSTA / USBIMSK */
#define USB_RSTR  0x80          /* USB reset */
#define USB_SUSR  0x40          /* suspend */
#define USB_RESR  0x20          /* resume */
#define USB_SOF   0x10          /* start of frame */
#define USB_PSOF  0x08          /* pseudo start of frame */
#define USB_SETUP 0x04          /* setup packet received */
#define USB_STPOW 0x01          /* setup packet overwritten */

/* GLOBCTL */
#define GLOBCTL_MCUCLK 0x80     /* MCU at 24 MHz instead of 12 */
#define GLOBCTL_XINTEN 0x40     /* external interrupt input */
#define GLOBCTL_PUDIS  0x20     /* disable on-chip pull-ups */
#define GLOBCTL_LPWR   0x04     /* must be 1, or the USB blocks, buffers and config blocks stay powered down */
#define GLOBCTL_CPTEN  0x01     /* codec port enable */

/* VECINT values: highest pending source, 24h when nothing is pending */
#define VEC_OEP0    0x00        /* out endpoint n = 0x00 + n */
#define VEC_IEP0    0x08        /* in endpoint n = 0x08 + n */
#define VEC_STPOW   0x10
#define VEC_SETUP   0x12
#define VEC_PSOF    0x13
#define VEC_SOF     0x14
#define VEC_RESUME  0x15
#define VEC_SUSPEND 0x16
#define VEC_RESET   0x17
#define VEC_NONE    0x24

/* Endpoint configuration blocks: out endpoint n at FFA8h - 8n, in endpoint n at FF68h - 8n */
#define OEPCNF(n)   XREG(0xFFA8 - 8 * (n))
#define OEPBBAX(n)  XREG(0xFFA9 - 8 * (n))
#define OEPBSIZ(n)  XREG(0xFFAA - 8 * (n))
#define OEPDCNTX(n) XREG(0xFFAB - 8 * (n))
#define OEPBBAY(n)  XREG(0xFFAD - 8 * (n))
#define OEPDCNTY(n) XREG(0xFFAF - 8 * (n))
#define IEPCNF(n)   XREG(0xFF68 - 8 * (n))
#define IEPBBAX(n)  XREG(0xFF69 - 8 * (n))
#define IEPBSIZ(n)  XREG(0xFF6A - 8 * (n))
#define IEPDCNTX(n) XREG(0xFF6B - 8 * (n))
#define IEPBBAY(n)  XREG(0xFF6D - 8 * (n))
#define IEPDCNTY(n) XREG(0xFF6F - 8 * (n))

/* Configuration byte (control/interrupt/bulk layout) */
#define EPCNF_EN     0x80
#define EPCNF_ISO    0x40
#define EPCNF_TOGGLE 0x20
#define EPCNF_DBUF   0x10
#define EPCNF_STALL  0x08
#define EPCNF_IE     0x04

/* Data count byte: bit 7 is NACK (set by hardware when the buffer is full/empty), bits 6:0 the count */
#define EPDCNT_NACK  0x80

/* Isochronous configuration byte: bits 4:0 are bytes per sample (a whole frame, all channels) minus 1 */
#define EPCNF_BPS(n) ((n) - 1)

/* DMA channels (A.5.2). Time slot bits 0-13; for I2S, SDOUT1/SDIN1 left = slot 0, right = slot 4. */
#define DMATSL0 XREG(0xFFEA)
#define DMATSH0 XREG(0xFFE9)
#define DMACTL0 XREG(0xFFE8)
#define DMATSL1 XREG(0xFFF0)
#define DMATSH1 XREG(0xFFEF)
#define DMACTL1 XREG(0xFFEE)
#define DMATSL2 XREG(0xFFF6)
#define DMATSH2 XREG(0xFFF5)
#define DMACTL2 XREG(0xFFF4)
#define DMATSL3 XREG(0xFFF9)
#define DMATSH3 XREG(0xFFF8)
#define DMACTL3 XREG(0xFFF7)
#define DMATSH_BYTES(n) (((n) - 1) << 6)    /* bytes per time slot, 1-4 */
#define DMACTL_EN   0x80
#define DMACTL_WAB  0x40                    /* wrap-around buffer mode */
#define DMACTL_IN   0x08                    /* serves an in endpoint (0: out endpoint) */

/* Adaptive clock generator (A.5.3). Synthesizer = 600 MHz / N, N = 6.18 fixed point in ACGFRQ2:1:0;
 * writing ACGFRQ0 loads the new value. MCLKO = synthesizer / M. */
#define ACGFRQ0 XREG(0xFFE7)
#define ACGFRQ1 XREG(0xFFE6)
#define ACGFRQ2 XREG(0xFFE5)
#define ACGCAPL XREG(0xFFE4)                /* MCLK cycles counted per USB frame */
#define ACGCAPH XREG(0xFFE3)
#define ACGDCTL XREG(0xFFE2)                /* bits 7:4 divide-by-M minus 1, bits 2:0 divide-by-I minus 1 */
#define ACGCTL  XREG(0xFFE1)
#define ACGCTL_MCLKEN 0x40                  /* drive the MCLKO pin */
#define ACGCTL_MCLKCP 0x20                  /* capture counter counts MCLKO2 instead of MCLKO */
#define ACGCTL_MCLKIS 0x10                  /* external clock input: MCLKI2 instead of MCLKI */
#define ACGCTL_DIVEN  0x04                  /* enable the dividers */

/* Codec port interface (A.5.4). CPTCNF1-4 can only be written while GLOBCTL.CPTEN is 0. */
#define CPTCNF1 XREG(0xFFE0)                /* bits 7:3 time slots - 1, bits 2:0 mode */
#define CPTCNF2 XREG(0xFFDF)                /* slot 0 length, data bits per slot, slot length */
#define CPTCNF3 XREG(0xFFDE)
#define CPTCNF4 XREG(0xFFDD)                /* bit 3 clock select, bits 2:0 SCLK = MCLK / (B + 1) */
#define CPTCTL  XREG(0xFFDC)
#define CPTCNF3_DDLY   0x80                 /* data one SCLK after the frame sync edge (I2S) */
#define CPTCNF3_TRSEN  0x40                 /* 3-state unused slots */
#define CPTCNF3_CSCLKP 0x20                 /* sync and data out on the falling SCLK edge, data in on the rising */
#define CPTCNF3_CSYNCP 0x10                 /* frame sync active high */
#define CPTCNF3_CSYNCL 0x08                 /* frame sync lasts all of slot 0 (50 % LRCK) */
#define CPTCNF3_BYOR   0x04                 /* reverse each sample's bytes: ON in the I2S modes for USB audio's little-endian samples */
#define CPTCNF3_CSCLKD 0x02                 /* SCLK is an input */
#define CPTCNF3_CSYNCD 0x01                 /* frame sync (LRCK) is an input */
#define CPTCTL_CRST    0x01                 /* CRESET pin high (general-purpose and AC '97 modes) */

/* Setup packet and buffer space */
#define SETUP_PACKET 0xFF28
#define BUF_BASE     0xF800
#define BUF_ADDR(a)  ((uint8_t)(((a) - BUF_BASE) >> 3))   /* base address byte: 8-byte units */

#endif
