/* Per-channel measurements on a stitched sweep; a port of
 * fpv_band_monitor/analysis.py (centroid centre, SNR detection, -3/-20 dB
 * occupied width, smoothing). Inputs are linear powers from dsp.h. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "dsp.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float search_mhz;       /* half-width searched for the peak (8) */
    float min_snr_db;       /* detection threshold above the floor (18) */
    float noise_margin_db;  /* centroid ignores bins within this of floor (3) */
    float centroid_half_mhz;/* half-width of the centroid band (7.5) */
} an_params_t;

typedef enum {
    AN_OK = 0,
    AN_NO_DATA,
    AN_LOW_SNR,
} an_reason_t;

typedef struct {
    bool present;
    an_reason_t reason;
    float nominal_mhz;
    float centre_mhz;       /* NAN when absent */
    float offset_mhz;
    float peak_mhz;         /* NAN when no data */
    float peak_db;
    float floor_db;
    float snr_db;
    float bw3_mhz;          /* NAN when not measured */
    float bw20_mhz;
} an_meas_t;

void an_default_params(an_params_t *p);
/* An empty (no data) measurement. */
void an_meas_clear(an_meas_t *m, float nominal_mhz);

/* Median of the valid bins, in dB. scratch: at least n words. */
float an_floor_db(const float *lin, int n, uint32_t *scratch);

void an_measure(const float *lin, const sweep_plan_t *plan, float nominal_mhz,
                const an_params_t *prm, float floor_db, an_meas_t *out);

/* Shrink the search window so neighbouring channels cannot steal a peak. */
float an_auto_search(const int *freqs_mhz, int n, float requested);

/* EMA of linear power: avg += a (x - avg). first=true copies. */
void an_average(float *avg, const float *x, int n, float a, bool first);
void an_peak_hold(float *hold, const float *x, int n, bool first);

/* Max of the valid bins of [k0, k1) in each of ncols columns -> dB (NAN). */
void an_columns_db(const float *lin, int nbins, int ncols, float *col_db);
void an_columns_lin(const float *lin, int nbins, int ncols, float *col_lin);

typedef struct { bool has; float v; } an_smooth_t;
/* EMA over the measured centre, holding the last value while absent. */
float an_smooth(an_smooth_t *s, const an_meas_t *m, float alpha);

#ifdef __cplusplus
}
#endif
