/* Hop spectra and sweep stitching, integer FFT (the C5 has no FPU).
 *
 * Mirrors fpv_band_monitor/esp_sdr.py: Welch PSD per hop (Hann, 50 %
 * overlap, mean removed per segment, dB full scale, DC-centred), LO hops
 * spaced so every bin is seen by two hops, and the *lower* of the two
 * readings kept so DC spikes and I/Q images (present in one hop only) drop
 * out. The FFT is 1024 points (78.125 kHz bins at 80 MS/s) instead of the PC
 * version's 800, because the integer FFT needs a power of two.
 *
 * Powers are linear (full scale = 1.0) floats. "No data" is +INF, so the
 * min-combine needs no special case. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DSP_LOG2N 10
#define DSP_NFFT (1 << DSP_LOG2N)
#define DSP_RATE_HZ 80000000
#define DSP_BIN_HZ 78125            /* DSP_RATE_HZ / DSP_NFFT, exact */
#define DSP_BINS_PER_MHZ 12.8f

#define SWEEP_MAX_HOPS 16
#define SWEEP_MAX_BINS 2304          /* 180 MHz of span */

typedef struct {
    int nhops;
    int lo_mhz[SWEEP_MAX_HOPS];
    int start_mhz;                  /* bin k is centred at start + (k + 0.5) * bin */
    int nbins;
} sweep_plan_t;

/* Allocates the FFT work buffers (about 33 KB); false if out of memory. */
bool dsp_init(void);

/* iq: n IQ10 words. psd: DSP_NFFT linear powers, index DSP_NFFT/2 = LO. */
void dsp_hop_psd(const uint32_t *iq, unsigned n, float *psd);

/* Whole-MHz LO plan over [start, stop]; 0 on success. */
int sweep_plan(sweep_plan_t *p, float start_mhz, float stop_mhz, int step_mhz);
void sweep_clear(float *sweep, int nbins);
/* Paint one hop onto the sweep (min-combine), keeping only bins within
 * usable_mhz of the LO and outside dc_khz of it. */
void sweep_stitch(const sweep_plan_t *p, int hop, const float *psd, float *sweep,
                  float usable_mhz, float dc_khz);
float sweep_bin_mhz(const sweep_plan_t *p, int k);
int sweep_index_of(const sweep_plan_t *p, float mhz);

/* 10*log10(x) to ~0.001 dB without libm. +INF/NaN pass through; x <= 0
 * gives -200. */
float dsp_db(float lin);
float dsp_lin(float db);
static inline bool dsp_valid(float x) {
    union { float f; uint32_t u; } v = { x };
    return (v.u & 0x7f800000u) != 0x7f800000u;
}
float dsp_inf(void);

#ifdef __cplusplus
}
#endif
