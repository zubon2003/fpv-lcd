/* Radio access for the LCD band monitor, implemented in the C5 receiver
 * (main/families/c5_c6_c61/receiver.c). Not thread-safe: one caller only. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Retune the LO to a whole MHz (recalibrates the RX path when it changes). */
bool sdr_hw_tune(unsigned mhz);
/* Analog filter in MHz; 0 = widest (what the PC monitor uses). */
void sdr_hw_bandwidth(unsigned mhz);
/* Manual gain-table index (not dB), or -1 for the hardware AGC. */
void sdr_hw_gain(int index);
int sdr_hw_gain_max(void);
/* One 80 MS/s snapshot of n IQ10 words (I bits 0..9, Q bits 10..19). The
 * pointer stays valid until the next capture. */
bool sdr_hw_capture(unsigned n, const uint32_t **iq);

/* Receive only: tune the Wi-Fi driver (null mode, promiscuous, never
 * transmits) to a Wi-Fi channel with the hardware AGC and hand every frame
 * received in ms to cb (a wifi_promiscuous_cb_t). The next sdr_hw_tune
 * prepares the SDR receiver again; set the gain again too. */
bool sdr_hw_wifi_listen(unsigned channel, unsigned ms, void (*cb)(void *buf, int type));

/* Hand one stock ESP-SDR protocol line to the firmware's own handler. */
void sdr_hw_command(char *line);

/* First thing in app_main: deep-sleeps instead of starting on a flat battery. */
void fpv_monitor_early(void);
/* Entry point of the monitor; called from app_main, never returns. */
void fpv_monitor_main(void);

#ifdef __cplusplus
}
#endif
