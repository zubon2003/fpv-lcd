#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N DSP_NFFT
#define HALF (N / 2)
#define SEG_HOP (N / 2)
#define FPV_PI 3.14159265358979323846

/* Heap, not .bss: on the C5 the static sections must end below the modem's
 * capture bank (0x40820000), which leaves little room. */
static int32_t *s_re, *s_im;
static uint64_t *s_acc;
static int16_t *s_win;              /* Hann, Q15 */
static int32_t *s_cos, *s_sin;      /* Q30, W^k = cos - j sin */
static uint16_t *s_rev;
static int32_t s_log2_lut[257];     /* Q16 log2(1 + i/256) */
static float s_psd_scale_1seg;      /* 1 / (S^2 * W^2) */
static bool s_ready;

float dsp_inf(void) {
    union { uint32_t u; float f; } v = { 0x7f800000u };
    return v.f;
}

bool dsp_init(void) {
    if (s_ready) return true;
    s_re = malloc(N * sizeof(*s_re));
    s_im = malloc(N * sizeof(*s_im));
    s_acc = malloc(N * sizeof(*s_acc));
    s_win = malloc(N * sizeof(*s_win));
    s_cos = malloc(HALF * sizeof(*s_cos));
    s_sin = malloc(HALF * sizeof(*s_sin));
    s_rev = malloc(N * sizeof(*s_rev));
    if (!s_re || !s_im || !s_acc || !s_win || !s_cos || !s_sin || !s_rev) return false;
    double wsum = 0.0;
    for (int k = 0; k < N; k++) {
        /* numpy.hanning(N): symmetric, 0.5 - 0.5 cos(2 pi k / (N - 1)) */
        double w = 0.5 - 0.5 * cos(2.0 * FPV_PI * k / (N - 1));
        int q = (int)lround(w * 32767.0);
        s_win[k] = (int16_t)q;
        wsum += q / 32767.0;
    }
    for (int k = 0; k < HALF; k++) {
        double a = 2.0 * FPV_PI * k / N;
        s_cos[k] = (int32_t)llround(cos(a) * 1073741824.0);
        s_sin[k] = (int32_t)llround(sin(a) * 1073741824.0);
    }
    for (int k = 0; k < N; k++) {
        unsigned r = 0, x = (unsigned)k;
        for (int b = 0; b < DSP_LOG2N; b++) { r = (r << 1) | (x & 1u); x >>= 1; }
        s_rev[k] = (uint16_t)r;
    }
    for (int i = 0; i <= 256; i++)
        s_log2_lut[i] = (int32_t)lround(log2(1.0 + i / 256.0) * 65536.0);
    /* x_int = (1024 (v - mean) * w_q15) >> 20 = x_true * 512 * 32767 / 1024 */
    double s = 512.0 * 32767.0 / 1024.0;
    s_psd_scale_1seg = (float)(1.0 / (s * s * wsum * wsum));
    s_ready = true;
    return true;
}

static void fft_inplace(void) {
    for (int len = 2; len <= N; len <<= 1) {
        int half = len >> 1, step = N / len;
        for (int j = 0; j < half; j++) {
            int64_t c = s_cos[j * step], s = s_sin[j * step];
            for (int i = j; i < N; i += len) {
                int b = i + half;
                int64_t xr = s_re[b], xi = s_im[b];
                int32_t tr = (int32_t)((c * xr + s * xi) >> 30);
                int32_t ti = (int32_t)((c * xi - s * xr) >> 30);
                int32_t ar = s_re[i], ai = s_im[i];
                s_re[b] = ar - tr; s_im[b] = ai - ti;
                s_re[i] = ar + tr; s_im[i] = ai + ti;
            }
        }
    }
}

static inline int32_t iq10_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int32_t iq10_q(uint32_t w) { return (int32_t)((w >> 10) << 22) >> 22; }

void dsp_hop_psd(const uint32_t *iq, unsigned n, float *psd) {
    unsigned nseg = n >= N ? (n - N) / SEG_HOP + 1 : 0;
    memset(s_acc, 0, N * sizeof(*s_acc));
    for (unsigned seg = 0; seg < nseg; seg++) {
        const uint32_t *x = iq + seg * SEG_HOP;
        int32_t si = 0, sq = 0;
        for (int k = 0; k < N; k++) { si += iq10_i(x[k]); sq += iq10_q(x[k]); }
        for (int k = 0; k < N; k++) {
            /* complex = I - jQ (the reference client's orientation) */
            int64_t vi = (int64_t)(iq10_i(x[k]) * N - si);
            int64_t vq = (int64_t)(iq10_q(x[k]) * N - sq);
            int r = s_rev[k];
            s_re[r] = (int32_t)((vi * s_win[k]) >> 20);
            s_im[r] = (int32_t)((-vq * s_win[k]) >> 20);
        }
        fft_inplace();
        for (int k = 0; k < N; k++) {
            int64_t a = s_re[k], b = s_im[k];
            s_acc[k] += (uint64_t)(a * a + b * b);
        }
    }
    float scale = nseg ? s_psd_scale_1seg / (float)nseg : 0.0f;
    for (int k = 0; k < N; k++)                       /* fftshift */
        psd[k] = (float)s_acc[(k + HALF) & (N - 1)] * scale;
}

int sweep_plan(sweep_plan_t *p, float start_mhz, float stop_mhz, int step_mhz) {
    int lo0 = (int)floorf(start_mhz) - step_mhz / 2;
    p->nhops = 0;
    for (int lo = lo0;; lo += step_mhz) {
        if (p->nhops >= SWEEP_MAX_HOPS) return -1;
        p->lo_mhz[p->nhops++] = lo;
        if (!((float)lo < stop_mhz + step_mhz / 2.0f)) break;
    }
    /* esp_sdr.hop_plan_mhz: keep adding while the last LO < stop + step/2 */
    p->start_mhz = (int)floorf(start_mhz);
    int span = (int)ceilf(stop_mhz) - p->start_mhz;
    int nb = (span * 64 + 2) / 5;                   /* round(span * 12.8) */
    if (nb < 1) nb = 1;
    if (nb > SWEEP_MAX_BINS) return -1;
    p->nbins = nb;
    return 0;
}

void sweep_clear(float *sweep, int nbins) {
    float inf = dsp_inf();
    for (int k = 0; k < nbins; k++) sweep[k] = inf;
}

static inline int floor_div10(int64_t x) {
    return (int)(x >= 0 ? x / 10 : -((-x + 9) / 10));
}

void sweep_stitch(const sweep_plan_t *p, int hop, const float *psd, float *sweep,
                  float usable_mhz, float dc_khz) {
    /* Offsets in tenths of a bin: off10 = 10 (centre_k - LO) / bin
     * = 128 (start - LO) + 10 k + 5, exact in integers. One tenth of a bin is
     * 7812.5 Hz, so |off| <= X Hz  <=>  |off10| * 15625 <= 2 X. */
    int64_t base = 128LL * (p->start_mhz - p->lo_mhz[hop]) + 5;
    int64_t usable2 = (int64_t)(usable_mhz * 2.0e6f);
    int64_t dc2 = (int64_t)(dc_khz * 2.0e3f);
    for (int k = 0; k < p->nbins; k++) {
        int64_t off10 = base + 10LL * k;
        int64_t mag = (off10 < 0 ? -off10 : off10) * 15625;
        if (mag > usable2) {
            if (off10 > 0) break;
            continue;
        }
        if (mag < dc2) continue;
        int src = floor_div10(off10 + 5) + N / 2;     /* round(off / bin) */
        if (src < 0) src = 0;
        if (src > N - 1) src = N - 1;
        float v = psd[src];
        if (v < sweep[k]) sweep[k] = v;
    }
}

float sweep_bin_mhz(const sweep_plan_t *p, int k) {
    return (float)p->start_mhz + ((float)k + 0.5f) * (1.0f / DSP_BINS_PER_MHZ);
}

int sweep_index_of(const sweep_plan_t *p, float mhz) {
    return (int)floorf((mhz - (float)p->start_mhz) * DSP_BINS_PER_MHZ - 0.5f + 0.5f);
}

float dsp_db(float x) {
    union { float f; uint32_t u; } v = { x };
    if ((v.u & 0x7f800000u) == 0x7f800000u) return x;
    if ((v.u & 0x80000000u) || v.u < 0x00800000u) return -200.0f;  /* <=0 / denormal */
    int32_t e = (int32_t)(v.u >> 23) - 127;
    uint32_t m = v.u & 0x7fffffu;
    uint32_t i = m >> 15, frac = m & 0x7fffu;
    int32_t a = s_log2_lut[i], b = s_log2_lut[i + 1];
    int32_t l = a + (int32_t)(((int64_t)(b - a) * frac) >> 15);
    int32_t log2q16 = e * 65536 + l;
    return (float)log2q16 * (3.0102999566f / 65536.0f);
}

float dsp_lin(float db) {
    return exp2f(db * 0.33219280949f);
}
