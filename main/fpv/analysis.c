#include "analysis.h"
#include <math.h>
#include <string.h>

#define AN_NAN __builtin_nanf("")
#define WIN_MAX 512

void an_default_params(an_params_t *p) {
    p->search_mhz = 8.0f;
    p->min_snr_db = 18.0f;
    p->noise_margin_db = 3.0f;
    p->centroid_half_mhz = 7.5f;
}

static uint32_t select_kth(uint32_t *a, int n, int k) {
    /* Quickselect on float bit patterns: positive floats order as integers. */
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        uint32_t pivot = a[(lo + hi) >> 1];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) { uint32_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else return a[k];
    }
    return a[k];
}

float an_floor_db(const float *lin, int n, uint32_t *scratch) {
    int m = 0;
    for (int k = 0; k < n; k++) {
        if (!dsp_valid(lin[k])) continue;
        union { float f; uint32_t u; } v = { lin[k] < 0 ? 0.0f : lin[k] };
        scratch[m++] = v.u;
    }
    if (m == 0) return AN_NAN;
    /* numpy.median: mean of the two middle values for an even count */
    union { uint32_t u; float f; } a, b;
    a.u = select_kth(scratch, m, m / 2);
    if (m & 1) return dsp_db(a.f);
    b.u = select_kth(scratch, m, m / 2 - 1);
    return 0.5f * (dsp_db(a.f) + dsp_db(b.f));
}

/* Contiguous run around peak_i at or above thr (dB array, NAN = gap). */
static void run_around(const float *db, int n, int peak_i, float thr, int *lo, int *hi) {
    int l = peak_i, h = peak_i;
    while (l - 1 >= 0 && db[l - 1] == db[l - 1] && db[l - 1] >= thr) l--;
    while (h + 1 < n && db[h + 1] == db[h + 1] && db[h + 1] >= thr) h++;
    *lo = l; *hi = h;
}

static float edge_interp(const float *db, int n, int inside, int outside, float thr) {
    if (outside < 0 || outside >= n || db[outside] != db[outside]) return 0.0f;
    float pin = db[inside], pout = db[outside];
    if (pin <= pout) return 0.0f;
    float f = (pin - thr) / (pin - pout);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static float bandwidth(const float *db, int n, int peak_i, float peak, float drop) {
    float thr = peak - drop;
    int lo, hi;
    run_around(db, n, peak_i, thr, &lo, &hi);
    float left = edge_interp(db, n, lo, lo - 1, thr);
    float right = edge_interp(db, n, hi, hi + 1, thr);
    return ((float)(hi - lo) + left + right) / DSP_BINS_PER_MHZ;
}

/* Power-weighted centroid of the bins in (centre - half, centre + half),
 * weighted by power above the floor; bins within margin of it dropped. */
static bool window_centroid(const float *lin, const sweep_plan_t *p, float centre,
                            float half, float floor_db, float margin, float *out) {
    int k0 = sweep_index_of(p, centre - half) - 1, k1 = sweep_index_of(p, centre + half) + 1;
    if (k0 < 0) k0 = 0;
    if (k1 > p->nbins - 1) k1 = p->nbins - 1;
    float floor_lin = dsp_lin(floor_db);
    float thr = floor_db + margin;
    float sw = 0.0f, swf = 0.0f;
    bool any = false;
    for (int k = k0; k <= k1; k++) {
        float f = sweep_bin_mhz(p, k) - centre;      /* relative: keeps precision */
        if (!(f > -half && f < half)) continue;
        if (!dsp_valid(lin[k])) continue;
        if (!(dsp_db(lin[k]) > thr)) continue;
        any = true;
        float w = lin[k] - floor_lin;
        if (w <= 0) continue;
        sw += w;
        swf += w * f;
    }
    if (!any || sw <= 0) return false;
    *out = centre + swf / sw;
    return true;
}

void an_meas_clear(an_meas_t *m, float nominal) {
    memset(m, 0, sizeof(*m));
    m->nominal_mhz = nominal;
    m->centre_mhz = m->offset_mhz = m->peak_mhz = AN_NAN;
    m->peak_db = m->snr_db = m->bw3_mhz = m->bw20_mhz = AN_NAN;
    m->floor_db = AN_NAN;
    m->reason = AN_NO_DATA;
}

void an_measure(const float *lin, const sweep_plan_t *p, float nominal,
                const an_params_t *prm, float floor_db, an_meas_t *m) {
    an_meas_clear(m, nominal);
    m->floor_db = floor_db;

    float search = prm->search_mhz;
    int lo = sweep_index_of(p, nominal - search);
    int hi = sweep_index_of(p, nominal + search) + 1;
    if (lo < 0) lo = 0;
    if (hi > p->nbins) hi = p->nbins;
    int n = hi - lo;
    if (n <= 0) return;
    if (n > WIN_MAX) n = WIN_MAX;
    float db[WIN_MAX];
    int peak_i = -1;
    float peak = -1e30f;
    for (int i = 0; i < n; i++) {
        float x = lin[lo + i];
        db[i] = dsp_valid(x) ? dsp_db(x) : AN_NAN;
        if (db[i] == db[i] && db[i] > peak) { peak = db[i]; peak_i = i; }
    }
    if (peak_i < 0) return;

    m->peak_mhz = sweep_bin_mhz(p, lo + peak_i);
    m->peak_db = peak;
    m->snr_db = peak - floor_db;
    if (!(m->snr_db >= prm->min_snr_db)) { m->reason = AN_LOW_SNR; return; }

    m->bw3_mhz = bandwidth(db, n, peak_i, peak, 3.0f);
    float half = prm->centroid_half_mhz < search ? prm->centroid_half_mhz : search;
    float centre;
    if (window_centroid(lin, p, nominal, search, floor_db, prm->noise_margin_db, &centre)) {
        if (centre < nominal - search) centre = nominal - search;
        if (centre > nominal + search) centre = nominal + search;
        float refined;
        if (window_centroid(lin, p, centre, half, floor_db, prm->noise_margin_db, &refined))
            centre = refined;
    } else {
        centre = m->peak_mhz;
    }
    m->present = true;
    m->reason = AN_OK;
    m->centre_mhz = centre;
    m->offset_mhz = centre - nominal;
    m->bw20_mhz = bandwidth(db, n, peak_i, peak, 20.0f);
}

float an_auto_search(const int *f, int n, float requested) {
    float best = requested;
    bool any = false;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (f[j] <= f[i]) continue;
            /* only adjacent pairs matter, but the minimum gap is the same */
            float g = (float)(f[j] - f[i]) / 2.0f;
            if (!any || g < best) { best = g; any = true; }
        }
    if (!any) return requested;
    if (best < 1.0f) best = 1.0f;
    return best < requested ? best : requested;
}

void an_mean_frames(float *avg, const float *frames, int stride, int nframes, int n) {
    for (int k = 0; k < n; k++) {
        float sum = 0.0f;
        int count = 0;
        for (int f = 0; f < nframes; f++) {
            float x = frames[(size_t)f * stride + k];
            if (dsp_valid(x)) { sum += x; count++; }
        }
        avg[k] = count ? sum / (float)count : dsp_inf();
    }
}

void an_peak_hold(float *hold, const float *x, int n, bool first) {
    for (int k = 0; k < n; k++) {
        if (first || !dsp_valid(hold[k])) { hold[k] = x[k]; continue; }
        if (dsp_valid(x[k]) && x[k] > hold[k]) hold[k] = x[k];
    }
}

void an_columns_lin(const float *lin, int nbins, int ncols, float *col) {
    for (int c = 0; c < ncols; c++) {
        int k0 = (int)((int64_t)c * nbins / ncols);
        int k1 = (int)((int64_t)(c + 1) * nbins / ncols);
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > nbins) k1 = nbins;
        float best = -1.0f;
        for (int k = k0; k < k1; k++)
            if (dsp_valid(lin[k]) && lin[k] > best) best = lin[k];
        col[c] = best >= 0.0f ? best : dsp_inf();
    }
}

void an_columns_db(const float *lin, int nbins, int ncols, float *col_db) {
    an_columns_lin(lin, nbins, ncols, col_db);
    for (int c = 0; c < ncols; c++)
        col_db[c] = dsp_valid(col_db[c]) ? dsp_db(col_db[c]) : AN_NAN;
}

float an_smooth(an_smooth_t *s, const an_meas_t *m, float alpha) {
    if (m->present && m->centre_mhz == m->centre_mhz) {
        s->v = s->has ? s->v + alpha * (m->centre_mhz - s->v) : m->centre_mhz;
        s->has = true;
    }
    return s->has ? s->v : AN_NAN;
}
