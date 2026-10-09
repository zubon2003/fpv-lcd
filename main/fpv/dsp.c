#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N DSP_NFFT
#define HALF (N / 2)
#define SEG_HOP (N / 2)
#define FPV_PI 3.14159265358979323846

/* Two FFT kernels:
 *
 * default    32-bit only: Q31 twiddles, the product's upper half taken with
 *            one MULH, and every stage halves (a +- W b) / 2. Data enter at
 *            up to 2^27 and stay below 2^30, so ~20 bits of precision remain
 *            after the 10 stages: no measurable loss against the reference.
 * FPV_FFT_REF the first version: Q30 twiddles, full 64-bit products, no
 *            per-stage scaling (two multiplies and a carry per product).
 *
 * Buffers on the heap, not .bss: on the C5 the static sections must end
 * below the modem's capture bank (0x40820000), which leaves little room. */
static int32_t *s_re, *s_im;
static uint64_t *s_acc;
static int32_t *s_win;              /* Hann, Q15 */
static int32_t *s_cos, *s_sin;      /* W^k = cos - j sin; Q31 (Q30 for REF) */
static uint16_t *s_rev;
static int32_t s_log2_lut[257];     /* Q16 log2(1 + i/256) */
static float s_psd_scale_1seg;      /* full-scale power per accumulated unit */
static bool s_ready;

#ifdef FPV_FFT_REF
#define TW_ONE 1073741824.0         /* Q30 */
#else
#define TW_ONE 2147483647.0         /* Q31 (1.0 itself does not fit) */
#endif

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
        s_win[k] = q;
        wsum += q / 32767.0;
    }
    for (int k = 0; k < HALF; k++) {
        double a = 2.0 * FPV_PI * k / N;
        double c = cos(a) * TW_ONE, sn = sin(a) * TW_ONE;
        s_cos[k] = (int32_t)(c >= TW_ONE ? TW_ONE : llround(c));
        s_sin[k] = (int32_t)(sn >= TW_ONE ? TW_ONE : llround(sn));
    }
    for (int k = 0; k < N; k++) {
        unsigned r = 0, x = (unsigned)k;
        for (int b = 0; b < DSP_LOG2N; b++) { r = (r << 1) | (x & 1u); x >>= 1; }
        s_rev[k] = (uint16_t)r;
    }
    for (int i = 0; i <= 256; i++)
        s_log2_lut[i] = (int32_t)lround(log2(1.0 + i / 256.0) * 65536.0);
#ifdef FPV_FFT_REF
    /* x_int = (1024 (v - mean) * w_q15) >> 20 = x_true * 512 * 32767 / 1024;
     * the FFT does not scale, so X_int = X_true * s. */
    double s = 512.0 * 32767.0 / 1024.0;
    s_psd_scale_1seg = (float)(1.0 / (s * s * wsum * wsum));
#else
    /* x_int = 4 (v - mean) w_q15 = x_true * 512 * 32767 * 4, and the ten
     * halving stages divide by 1024: X_int = X_true * s / 1024. */
    double s = 512.0 * 32767.0 * 4.0 / 1024.0;
    s_psd_scale_1seg = (float)(1.0 / (s * s * wsum * wsum));
#endif
    s_ready = true;
    return true;
}

#ifdef FPV_FFT_REF
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
#else
/* Upper 32 bits of the 64-bit product: a single MULH on RV32. */
static inline int32_t mulh(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 32); }

static void fft_inplace(void) {
    int32_t *re = s_re, *im = s_im;
    /* Stages 1 and 2 together: their twiddles are 1 and -j, so no multiply.
     * Same halving as the general stage ((a +- W b) / 2 each stage). */
    for (int g = 0; g < N; g += 4) {
        int32_t r0 = re[g] >> 1, i0 = im[g] >> 1, r1 = re[g + 1] >> 1, i1 = im[g + 1] >> 1;
        int32_t r2 = re[g + 2] >> 1, i2 = im[g + 2] >> 1, r3 = re[g + 3] >> 1, i3 = im[g + 3] >> 1;
        int32_t a0r = (r0 + r1) >> 1, a0i = (i0 + i1) >> 1;     /* stage 1, then /2 for stage 2 */
        int32_t a1r = (r0 - r1) >> 1, a1i = (i0 - i1) >> 1;
        int32_t a2r = (r2 + r3) >> 1, a2i = (i2 + i3) >> 1;
        int32_t b3r = r2 - r3, b3i = i2 - i3;                  /* times -j, halved: (b3i, -b3r) / 2 */
        int32_t tr = b3i >> 1, ti = -(b3r >> 1);
        re[g] = a0r + a2r;     im[g] = a0i + a2i;
        re[g + 2] = a0r - a2r; im[g + 2] = a0i - a2i;
        re[g + 1] = a1r + tr;  im[g + 1] = a1i + ti;
        re[g + 3] = a1r - tr;  im[g + 3] = a1i - ti;
    }
    for (int len = 8; len <= N; len <<= 1) {
        int half = len >> 1, step = N / len, groups = N / len;
        if (half <= groups) {
            /* many short groups: one twiddle per j, walk the groups */
            for (int j = 0; j < half; j++) {
                int32_t c = s_cos[j * step], s = s_sin[j * step];
                for (int i = j; i < N; i += len) {
                    int b = i + half;
                    int32_t xr = re[b], xi = im[b];
                    int32_t tr = mulh(c, xr) + mulh(s, xi);   /* (c - j s)(xr + j xi) / 2 */
                    int32_t ti = mulh(c, xi) - mulh(s, xr);
                    int32_t ar = re[i] >> 1, ai = im[i] >> 1;
                    re[b] = ar - tr; im[b] = ai - ti;
                    re[i] = ar + tr; im[i] = ai + ti;
                }
            }
        } else {
            /* few long groups: walk j inside, no per-j loop overhead */
            for (int g = 0; g < N; g += len) {
                const int32_t *cp = s_cos, *sp = s_sin;
                int32_t *ra = re + g, *ia = im + g, *rb = re + g + half, *ib = im + g + half;
                for (int j = 0; j < half; j++, cp += step, sp += step) {
                    int32_t c = *cp, s = *sp;
                    int32_t xr = rb[j], xi = ib[j];
                    int32_t tr = mulh(c, xr) + mulh(s, xi);
                    int32_t ti = mulh(c, xi) - mulh(s, xr);
                    int32_t ar = ra[j] >> 1, ai = ia[j] >> 1;
                    rb[j] = ar - tr; ib[j] = ai - ti;
                    ra[j] = ar + tr; ia[j] = ai + ti;
                }
            }
        }
    }
}
#endif

static inline int32_t iq10_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int32_t iq10_q(uint32_t w) { return (int32_t)((w >> 10) << 22) >> 22; }

void dsp_hop_psd(const uint32_t *iq, unsigned n, float *psd) {
    unsigned nseg = n >= N ? (n - N) / SEG_HOP + 1 : 0;
    memset(s_acc, 0, N * sizeof(*s_acc));
#ifndef FPV_FFT_REF
    /* Mean over the whole snapshot, once per hop, in whole LSBs. The PC
     * version removes it per 1024-sample segment; either way only the bins
     * next to DC change, and the stitcher drops +-500 kHz around each LO. */
    int32_t si = 0, sq = 0;
    for (unsigned k = 0; k < nseg * SEG_HOP + SEG_HOP && k < n; k++) { si += iq10_i(iq[k]); sq += iq10_q(iq[k]); }
    unsigned used = nseg * SEG_HOP + SEG_HOP < n ? nseg * SEG_HOP + SEG_HOP : n;
    int32_t mi = used ? (int32_t)((si + (si >= 0 ? 1 : -1) * (int32_t)(used / 2)) / (int32_t)used) : 0;
    int32_t mq = used ? (int32_t)((sq + (sq >= 0 ? 1 : -1) * (int32_t)(used / 2)) / (int32_t)used) : 0;
#endif
    for (unsigned seg = 0; seg < nseg; seg++) {
        const uint32_t *x = iq + seg * SEG_HOP;
#ifdef FPV_FFT_REF
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
#else
        for (int k = 0; k < N; k++) {
            /* complex = I - jQ (the reference client's orientation) */
            int32_t w4 = s_win[k] << 2;
            int r = s_rev[k];
            s_re[r] = (iq10_i(x[k]) - mi) * w4;
            s_im[r] = (mq - iq10_q(x[k])) * w4;
        }
#endif
        fft_inplace();
        /* |X|^2 in 64 bits: |X| < 2^27, so 15 segments stay below 2^59 */
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
