/* Audio path: master clock, I2S codec port, and the DMA channels between it and the USB buffers. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Buffer space after endpoint 0 (F800h-F80Fh): one region per isochronous endpoint, holding its X buffer
 * and, right after it, its Y buffer. In linear mode each is one packet long; in wrap-around mode each is
 * a ring (audio.c), by default half the region. */
#define EP1_X      0xF810
#define EP1_BUF    296                      /* linear: >= MAX_PACKET (294), a multiple of 8 */
#define EP1_Y      (EP1_X + EP1_BUF)
#define EP1_REGION 720
#define EP2_X      (EP1_X + EP1_REGION)     /* FAE0h */
#define EP2_BUF    200                      /* linear: >= REC_MAX_PACKET (196) */
#define EP2_Y      (EP2_X + EP2_BUF)
#define EP2_REGION 480
/* (they end at FCC0h; diag.c keeps its logs at FCC0h-FF27h) */

/* DMA buffer modes, per direction (audio_set_dma_mode, vendor request VREQ_DMAMODE) */
#define DMA_WAB_PLAY 0x01
#define DMA_WAB_REC  0x02

/* Sample rates (audio_set_rate): one master clock serves both directions */
#define RATE_48K  0
#define RATE_44K1 1

void audio_init(void);
void audio_set_clock_master(uint8_t master);    /* 1: the TUSB3200 drives LRCK/SCLK; 0: takes them as inputs */
uint8_t audio_clock_master(void);
void audio_set_codec_mode(uint8_t mode, uint8_t creset);   /* nonzero: I2S mode 6; 0: general purpose (same timing, CRESET driven) */
void audio_set_mclk_ratio(uint8_t ratio);                   /* 0: 256 fs, 1: 384 fs, 2: 512 fs */
void audio_set_raw(const __data uint8_t *cfg);              /* bring-up: CPTCNF1-4, CPTCTL, DMA slots, ratio */
void audio_sof(void);                                      /* call on every USB start-of-frame */
void audio_set_lock(uint8_t enabled);                       /* bring-up: 0 freezes the clock trim */
int16_t audio_lock_u(void);                                 /* current N trim, LSB */
int16_t audio_lock_phase(void);                             /* accumulated MCLK error, cycles */
void audio_set_rate(uint8_t rate);                          /* RATE_48K / RATE_44K1: changes MCLK only */
uint8_t audio_rate(void);
void audio_set_dma_mode(uint8_t mode, uint8_t ring_play, uint8_t ring_rec);   /* DMA_WAB_* bits, ring sizes
                                                            (8-byte units, 0 = default); restarts the streams */
void playback_start(void);
void playback_stop(void);
void capture_start(void);                                   /* (re)starts capture: at boot only, see audio.c */

#endif
