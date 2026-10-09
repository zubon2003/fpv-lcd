/* Bare-metal workload for cycle estimates (run under an RV32IMAC emulator by
 * cycles.py). The host writes a capture and a task id, calls _start, and
 * counts the instructions between MARK_BEGIN and MARK_END. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "analysis.h"
#include "dsp.h"

#define IQ_N 8192
#define AVG_N 10
volatile uint32_t g_task;                       /* set by the host */
volatile uint32_t g_mark;                       /* host watches writes */
uint32_t g_iq[IQ_N];                            /* filled by the host */
static float g_psd[DSP_NFFT];
static float g_sweep[SWEEP_MAX_BINS];
static float g_ring[AVG_N * SWEEP_MAX_BINS];
static float g_avg[SWEEP_MAX_BINS];
static uint32_t g_scratch[SWEEP_MAX_BINS];
static float g_cols[3][320];

/* bump allocator for dsp_init */
static uint8_t g_heap[64 * 1024] __attribute__((aligned(16)));
static size_t g_heap_used;
void *malloc(size_t n) {
    void *p = g_heap + g_heap_used;
    g_heap_used += (n + 15) & ~(size_t)15;
    return g_heap_used <= sizeof(g_heap) ? p : 0;
}
void free(void *p) { (void)p; }

#define MARK(v) do { g_mark = (v); __asm__ volatile("" ::: "memory"); } while (0)

int main(void) {
    sweep_plan_t p;
    sweep_plan(&p, 5670.0f, 5835.0f, 25);        /* 4CH span */
    dsp_init();
    sweep_clear(g_sweep, p.nbins);
    if (g_task == 1) {                          /* one hop: PSD + stitch */
        MARK(1);
        dsp_hop_psd(g_iq, IQ_N, g_psd);
        sweep_stitch(&p, 4, g_psd, g_sweep, 28.0f, 500.0f);
        MARK(2);
    } else if (g_task == 2) {                   /* UI processing of one 4CH sweep */
        for (int h = 0; h < p.nhops; h++) {     /* fill a realistic sweep (not timed) */
            dsp_hop_psd(g_iq, 2048, g_psd);
            sweep_stitch(&p, h, g_psd, g_sweep, 28.0f, 500.0f);
        }
        for (int f = 0; f < AVG_N; f++) memcpy(g_ring + f * SWEEP_MAX_BINS, g_sweep, sizeof(float) * p.nbins);
        MARK(1);
        an_mean_frames(g_avg, g_ring, SWEEP_MAX_BINS, AVG_N, p.nbins);
        an_columns_db(g_sweep, p.nbins, 320, g_cols[0]);
        an_columns_lin(g_sweep, p.nbins, 320, g_cols[1]);
        an_columns_db(g_avg, p.nbins, 320, g_cols[2]);
        float floor_db = an_floor_db(g_avg, p.nbins, g_scratch);
        an_params_t prm;
        an_default_params(&prm);
        static const int ch[4] = {5685, 5705, 5780, 5820};
        prm.search_mhz = an_auto_search(ch, 4, 8.0f);
        for (int i = 0; i < 4; i++) {
            an_meas_t m;
            an_measure(g_avg, &p, (float)ch[i], &prm, floor_db, &m);
        }
        MARK(2);
    }
    MARK(3);
    for (;;) {}
}
