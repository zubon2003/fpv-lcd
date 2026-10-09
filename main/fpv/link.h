/* USB link of the LCD monitor: the FPV text commands and the SWEEP frames.
 *
 * Lines from the host that start with "FPV" are this protocol; every other
 * line is the stock ESP-SDR I/Q protocol (FREQ, CAP20, ...), so the same
 * firmware serves the PC monitor in either of its receiver modes.
 *
 * Commands (one reply line each: the answer, "OK" or "ERR <why>"):
 *   FPV?                         -> FPV 1 bin_hz=78125 maxbins=2304 step=25 ...
 *   FPV STATE?                   -> STATE mode=4CH ch=F3 gain=40 snr=18 start=5670 nbins=2112 ...
 *   FPV STREAM ON|OFF            send a SWEEP frame after every sweep
 *   FPV MODE 4CH|3CH|1CH
 *   FPV CH <E2|E1|F1|F3|F4|F5>   the 1CH channel
 *   FPV GAIN <0..60|AGC>
 *   FPV SNR <6..30>              detection threshold, dB above the floor
 *   FPV NOLCD                    LCD off, no on-screen processing: every cycle
 *                                goes to sweeping (ends with STREAM OFF, a lost
 *                                host, FPV LCD, or a tap on the screen)
 *   FPV LCD                      LCD back on
 *   FPV SPAN <start> <stop> [ch,ch,...]
 *                                sweep any span (MHz) and show up to 4
 *                                channels ("PC" mode on the LCD)
 *
 * Frame, after every finished sweep while streaming:
 *   SWEEP <seq> <start_mhz> <nbins> <bin_hz> <t_ms> <crc32 hex>\n
 *   then nbins little-endian uint16: dB = code / 100 - 200 (0xFFFF = no
 *   data). Bin k is centred at start_mhz + (k + 0.5) * bin_hz. The CRC is
 *   zlib's crc32 of the payload. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "channels.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FPV_LINK_VERSION 1
#define FPV_CODE_NONE 0xFFFFu
#define FPV_LINK_MAX_CH 4

uint16_t fpv_code(float lin);
void fpv_encode(const float *lin, int n, uint16_t *out);

typedef enum {
    FC_BAD = -1, FC_NONE = 0, FC_HELLO, FC_STATE, FC_STREAM, FC_MODE, FC_CH, FC_GAIN, FC_SNR, FC_SPAN,
    FC_NOLCD, FC_LCD,
} fpv_cmd_kind_t;

typedef struct {
    fpv_cmd_kind_t kind;
    int ival;                       /* STREAM 1/0, MODE 0..2, CH index, GAIN (-1 AGC), SNR */
    float start, stop;              /* SPAN */
    int nch;
    fpv_channel_t ch[FPV_LINK_MAX_CH];
    const char *err;                /* FC_BAD: why */
} fpv_cmd_t;

/* single_names: the 1CH list (for "FPV CH"). FC_NONE: not an FPV line. */
fpv_cmd_kind_t fpv_parse(const char *line, const char *const *single_names, int nsingle, fpv_cmd_t *c);

#ifdef __cplusplus
}
#endif
