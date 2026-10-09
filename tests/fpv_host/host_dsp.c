/* Host harness for main/fpv/dsp.c + analysis.c (compared against the Python
 * fpv-band-monitor-c5 reference by check_dsp.py).
 *
 *   host_dsp psd   <iq.bin> <n>                     -> psd float32 x1024 on stdout
 *   host_dsp sweep <iq.bin> <n> <start> <stop> <step> <usable> <dc_khz> <ch_mhz>...
 *        iq.bin holds one n-word capture per hop of the plan, in LO order;
 *        prints the stitched sweep (float32) to sweep.bin and one line per
 *        channel: name centre bw3 bw20 peak snr floor present */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "analysis.h"
#include "dsp.h"

static uint32_t *read_words(const char *path, size_t *count) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)sz);
    if (fread(w, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); exit(2); }
    fclose(f);
    *count = (size_t)sz / 4;
    return w;
}

int main(int argc, char **argv) {
    if (argc < 4) return 1;
    dsp_init();
    size_t count;
    uint32_t *iq = read_words(argv[2], &count);
    unsigned n = (unsigned)atoi(argv[3]);
    static float psd[DSP_NFFT];
    if (!strcmp(argv[1], "psd")) {
        dsp_hop_psd(iq, n, psd);
        fwrite(psd, sizeof(float), DSP_NFFT, stdout);
        return 0;
    }
    if (strcmp(argv[1], "sweep") || argc < 10) return 1;
    sweep_plan_t p;
    if (sweep_plan(&p, (float)atof(argv[4]), (float)atof(argv[5]), atoi(argv[6]))) return 3;
    float usable = (float)atof(argv[7]), dc = (float)atof(argv[8]);
    if (count < (size_t)n * p.nhops) { fprintf(stderr, "need %d hops\n", p.nhops); return 4; }
    static float sweep[SWEEP_MAX_BINS];
    sweep_clear(sweep, p.nbins);
    for (int h = 0; h < p.nhops; h++) {
        dsp_hop_psd(iq + (size_t)h * n, n, psd);
        sweep_stitch(&p, h, psd, sweep, usable, dc);
    }
    FILE *f = fopen("sweep.bin", "wb");
    fwrite(sweep, sizeof(float), (size_t)p.nbins, f);
    fclose(f);
    fprintf(stdout, "plan start=%d nbins=%d hops=%d los=", p.start_mhz, p.nbins, p.nhops);
    for (int h = 0; h < p.nhops; h++) fprintf(stdout, "%d%s", p.lo_mhz[h], h + 1 < p.nhops ? "," : "\n");

    static uint32_t scratch[SWEEP_MAX_BINS];
    float floor_db = an_floor_db(sweep, p.nbins, scratch);
    an_params_t prm;
    an_default_params(&prm);
    int freqs[16], nch = 0;
    for (int i = 9; i < argc && nch < 16; i++) freqs[nch++] = atoi(argv[i]);
    prm.search_mhz = an_auto_search(freqs, nch, 8.0f);
    for (int i = 0; i < nch; i++) {
        an_meas_t m;
        an_measure(sweep, &p, (float)freqs[i], &prm, floor_db, &m);
        printf("ch %d %.4f %.4f %.4f %.3f %.3f %.3f %d\n", freqs[i], m.centre_mhz, m.bw3_mhz,
               m.bw20_mhz, m.peak_db, m.snr_db, m.floor_db, m.present);
    }
    return 0;
}
