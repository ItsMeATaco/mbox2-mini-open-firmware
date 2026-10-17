/* Diagnostics any host can read as string descriptors 0x40-0x61, a reboot at 0x4F and the DMA buffer
 * mode switch at 0x70-0x73 (tools/firmware/diagstrings.py reads them). */
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

#define DIAG_FIRST_STRING 0x40

void diag_init(void);
void diag_setup(const __data uint8_t *setup);   /* log a setup packet */
void diag_sof(void);                            /* log endpoint 1's buffer counts; call every SOF */
void diag_poll(void);                           /* watch endpoint 1's buffers; call from the idle loop */
void diag_stream_start(void);                   /* clear the per-stream counters, count the stream */
void diag_ep0_stale(void);                      /* count an endpoint-0 IN interrupt ignored as stale */
void diag_rate_refused(void);                   /* count a SET_CUR sampling frequency refused (main.c set_rate) */
const uint8_t *diag_string(uint8_t index);       /* the string descriptor for `index`, or 0 */

#endif
