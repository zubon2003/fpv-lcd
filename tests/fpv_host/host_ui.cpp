/* Render the monitor screens on the PC (LovyanGFX in memory) to PPM files,
 * using sweeps stitched by the firmware DSP from synthetic captures.
 *
 *   host_ui <mode 0|1|2> <sweep.bin> <start> <stop> <out.ppm> [hold] [ch_mhz...]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "analysis.h"
#include "channels.h"
#include "dsp.h"
#include "ui.hpp"

using namespace fpv;

static int colour_slot(const char *name) {
    static const char *const ORDER[] = {"E2", "E1", "F3", "F5", "F1", "F4"};
    for (int i = 0; i < 6; i++)
        if (!strcmp(name, ORDER[i])) return i;
    return 5;
}

int main(int argc, char **argv) {
    if (argc < 7) return 1;
    dsp_init();
    static UiView v;
    v.mode = (Mode)atoi(argv[1]);
    FILE *f = fopen(argv[2], "rb");
    std::vector<float> sweep(SWEEP_MAX_BINS);
    size_t nb = fread(sweep.data(), sizeof(float), SWEEP_MAX_BINS, f);
    fclose(f);
    sweep_plan(&v.plan, (float)atof(argv[3]), (float)atof(argv[4]), 25);
    if ((int)nb != v.plan.nbins) { fprintf(stderr, "bins %zu != %d\n", nb, v.plan.nbins); return 2; }
    const char *out = argv[5];
    v.hold_on = atoi(argv[6]) != 0;
    int freqs[MAX_CH];
    v.nch = 0;
    for (int i = 7; i < argc && v.nch < MAX_CH; i++) {
        fpv_channel_t c;
        if (fpv_channel_parse(argv[i], &c)) return 3;
        UiChannel &u = v.ch[v.nch];
        memcpy(u.name, c.name, sizeof(u.name));
        u.nominal_mhz = c.freq_mhz;
        u.color = colour_slot(u.name);
        freqs[v.nch++] = c.freq_mhz;
    }

    /* same steps as monitor.cpp (one sweep standing in for the average) */
    std::vector<uint32_t> scratch(SWEEP_MAX_BINS);
    an_columns_db(sweep.data(), v.plan.nbins, COLS, v.col_raw);
    memcpy(v.col_avg, v.col_raw, sizeof(v.col_avg));
    for (int x = 0; x < COLS; x++)
        v.col_hold[x] = v.col_raw[x] == v.col_raw[x] ? v.col_raw[x] + 4.0f : v.col_raw[x];
    an_params_t prm;
    an_default_params(&prm);
    prm.search_mhz = an_auto_search(freqs, v.nch, 8.0f);
    v.floor_db = an_floor_db(sweep.data(), v.plan.nbins, scratch.data());
    float top = v.floor_db;
    for (int i = 0; i < v.nch; i++) {
        UiChannel &c = v.ch[i];
        an_measure(sweep.data(), &v.plan, (float)c.nominal_mhz, &prm, v.floor_db, &c.m);
        c.smoothed = c.m.present ? c.m.centre_mhz : NAN;
        if (c.m.peak_db == c.m.peak_db && c.m.peak_db > top) top = c.m.peak_db;
    }
    v.y_lo = v.floor_db - 8;
    v.y_hi = fmaxf(top + 8, v.floor_db + 30);
    float loud = -1e9f;
    for (int x = 0; x < COLS; x++)
        if (v.col_raw[x] == v.col_raw[x] && v.col_raw[x] > loud) loud = v.col_raw[x];
    v.wf_lo = v.floor_db - 3;
    v.wf_hi = fmaxf(loud + 3, v.floor_db + 25);
    /* waterfall: the same sweep with some noise, a carrier switching on */
    srand(3);
    for (int r = 0; r < WF_H; r++) {
        for (int x = 0; x < COLS; x++) {
            float db = v.col_raw[x] == v.col_raw[x] ? v.col_raw[x] : -128;
            db += (float)(rand() % 600) / 100.0f - 3.0f;
            if (r > 30 && db > v.floor_db + 15) db = v.floor_db + (float)(rand() % 300) / 100.0f;
            v.wf[(WF_H - r) % WF_H][x] = wf_code(db);
        }
    }
    v.wf_head = 0;
    v.wf_rows = WF_H;
    v.have_data = true;
    v.sweeps_per_s = v.mode == MODE_1CH ? 14.2f : v.mode == MODE_3CH ? 6.1f : 4.6f;
    v.gain = 40;
    v.bat_mv = v.mode == MODE_1CH ? 3410 : 3920;   /* 1ch preview shows the low colour */

    LGFX_Sprite screen;
    screen.setColorDepth(16);
    screen.createSprite(SCR_W, SCR_H);
    screen.fillScreen(0);
    Renderer r;
    if (!r.begin(&screen)) { fprintf(stderr, "sprites\n"); return 4; }
    r.draw_all(v);
    if (getenv("FPV_SETUP")) {                 /* the setup screen instead */
        SetupView sv{(Mode)atoi(getenv("FPV_SETUP")), "F3", 5780, 40, 18, v.bat_mv};
        r.draw_setup(sv);
    }

    FILE *o = fopen(out, "wb");
    fprintf(o, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++) {
            uint32_t c = screen.readPixelRGB(x, y).RGB888();
            unsigned char px[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
            fwrite(px, 1, 3, o);
        }
    fclose(o);
    printf("%s: floor %.1f dB, sprites %zu bytes\n", out, v.floor_db, r.sprite_bytes());
    for (int i = 0; i < v.nch; i++)
        printf("  %s centre %.3f present %d\n", v.ch[i].name, v.ch[i].m.centre_mhz, v.ch[i].m.present);
    return 0;
}
