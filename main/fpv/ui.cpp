#include "ui.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fpv_config.h"

namespace fpv {

/* ---------------------------------------------------------------- colours */
/* UI palette (spectrum, header, axis, readouts): index -> RGB. */
enum : uint8_t {
    C_BG = 0, C_GRID, C_AXIS, C_TEXT, C_DIM, C_RAW, C_AVG, C_HOLD, C_PANEL,
    C_BTN, C_BTN_ON, C_DEV_OK, C_DEV_WARN, C_DEV_BAD, C_DEV_IDLE, C_ERR,
    C_CH0 = 16,   /* + 4 * slot + {0 full, 1 mid, 2 inner band, 3 outer band} */
};
static const uint32_t UI_RGB[16] = {
    0x0e0e12, 0x262632, 0x8a8a9a, 0xf0f0f6, 0x5a5a68, 0x50506a, 0xf2f2f8,
    0x8a8aa4, 0x16161d, 0x2a2a36, 0xd8d8e4, 0x4aa8ff, 0xffd23f, 0xff5f56,
    0x5a5a68, 0xff3030,
};
/* fpv-band-monitor-c5 channel colours: red, green, blue, yellow, purple, cyan */
static const uint32_t CH_RGB[6] = {0xff5f56, 0x45d97a, 0x4aa8ff, 0xffbd2e, 0xc678dd, 0x00d0c0};

static inline uint8_t ch_full(int s) { return C_CH0 + 4 * s; }
static inline uint8_t ch_mid(int s) { return C_CH0 + 4 * s + 1; }
static inline uint8_t ch_inner(int s) { return C_CH0 + 4 * s + 2; }
static inline uint8_t ch_outer(int s) { return C_CH0 + 4 * s + 3; }

static uint32_t blend(uint32_t fg, uint32_t bg, float a) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float f = (float)((fg >> sh) & 255), b = (float)((bg >> sh) & 255);
        out |= (uint32_t)(b + a * (f - b) + 0.5f) << sh;
    }
    return out;
}

static void set_rgb(LGFX_Sprite &s, int i, uint32_t rgb) {
    s.setPaletteColor(i, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static void ui_palette(LGFX_Sprite &s) {
    for (int i = 0; i < 16; i++) set_rgb(s, i, UI_RGB[i]);
    for (int c = 0; c < 6; c++) {
        set_rgb(s, ch_full(c), CH_RGB[c]);
        set_rgb(s, ch_mid(c), blend(CH_RGB[c], UI_RGB[C_BG], 0.60f));
        set_rgb(s, ch_inner(c), blend(CH_RGB[c], UI_RGB[C_BG], 0.30f));
        set_rgb(s, ch_outer(c), blend(CH_RGB[c], UI_RGB[C_BG], 0.13f));
    }
}

/* Waterfall palette: viridis on 0..249, channel colours on 250..255. */
constexpr int WF_LEVELS = 250;
static void wf_palette(LGFX_Sprite &s) {
    static const uint8_t K[9][3] = {
        {68, 1, 84}, {71, 44, 122}, {59, 81, 139}, {44, 113, 142}, {33, 144, 141},
        {39, 173, 129}, {92, 200, 99}, {170, 220, 50}, {253, 231, 37}};
    for (int i = 0; i < WF_LEVELS; i++) {
        float t = (float)i / (WF_LEVELS - 1) * 8.0f;
        int a = (int)t;
        if (a > 7) a = 7;
        float f = t - a;
        uint8_t rgb[3];
        for (int j = 0; j < 3; j++) rgb[j] = (uint8_t)(K[a][j] + f * (K[a + 1][j] - K[a][j]) + 0.5f);
        s.setPaletteColor(i, rgb[0], rgb[1], rgb[2]);
    }
    for (int c = 0; c < 6; c++) set_rgb(s, WF_LEVELS + c, CH_RGB[c]);
}

const char *mode_label(Mode m) {
    switch (m) {
    case MODE_4CH: return "4CH";
    case MODE_3CH: return "3CH";
    default: return "1CH";
    }
}

uint8_t wf_code(float db) {
    if (!(db == db)) return 0;
    float c = (db + 128.0f) * 2.0f;
    if (c < 0) c = 0;
    if (c > 255) c = 255;
    return (uint8_t)(c + 0.5f);
}

static uint8_t dev_colour(float off) {
    if (!(off == off)) return C_DEV_IDLE;
    float d = fabsf(off);
    if (d <= FPV_DEV_OK_MHZ) return C_DEV_OK;
    if (d < FPV_DEV_WARN_MHZ) return C_DEV_WARN;
    return C_DEV_BAD;
}

/* -------------------------------------------------------------- hit test */
/* Header: [MODE 0..47] status [GAIN 214..275: -|+] [HOLD 278..319] */
constexpr int MODE_X1 = 48, GAIN_X0 = 214, GAIN_X1 = 276, HOLD_X0 = 278;
constexpr int ARROW_W = 34;   /* 1ch readout: < and > buttons */

Hit hit_test(const UiView &v, int x, int y) {
    if (y < HDR_H + 8) {
        if (x < MODE_X1 + 4) return HIT_MODE;
        if (x >= GAIN_X0 && x < GAIN_X1) return x < (GAIN_X0 + GAIN_X1) / 2 ? HIT_GAIN_DOWN : HIT_GAIN_UP;
        if (x >= HOLD_X0) return HIT_HOLD;
        return HIT_NONE;
    }
    if (y < RO_Y) return HIT_PLOT;
    if (v.mode == MODE_1CH) {
        if (x < ARROW_W) return HIT_CH_PREV;
        if (x >= SCR_W - ARROW_W) return HIT_CH_NEXT;
    }
    return HIT_NONE;
}

/* --------------------------------------------------------------- sprites */
static bool make(LGFX_Sprite &s, int w, int h, size_t &bytes, bool psram) {
    s.setPsram(psram);
    s.setColorDepth(8);
    if (!s.createSprite(w, h)) return false;
    if (!s.createPalette()) return false;
    bytes += (size_t)w * h;
    return true;
}

bool Renderer::begin(lgfx::LovyanGFX *dst, bool psram) {
    dst_ = dst;
    bytes_ = 0;
    bool ok = make(hdr_, SCR_W, HDR_H, bytes_, psram) && make(spec_, SCR_W, SPEC_H, bytes_, psram) &&
              make(axis_, SCR_W, AXIS_H, bytes_, psram) && make(wf_, SCR_W, WF_H, bytes_, psram) &&
              make(ro_, SCR_W, RO_H, bytes_, psram);
    if (!ok) return false;
    ui_palette(hdr_);
    ui_palette(spec_);
    ui_palette(axis_);
    ui_palette(ro_);
    wf_palette(wf_);
    return true;
}

int Renderer::x_of(const UiView &v, float mhz) const {
    float span = (float)v.plan.nbins / DSP_BINS_PER_MHZ;
    float x = (mhz - (float)v.plan.start_mhz) / span * SCR_W;
    return (int)floorf(x);
}

void Renderer::draw_all(const UiView &v) {
    draw_axis(v);
    draw_frame(v);
}

void Renderer::draw_frame(const UiView &v) {
    draw_header(v);
    draw_spectrum(v);
    draw_waterfall(v);
    draw_readouts(v);
}

/* ---------------------------------------------------------------- header */
static void button(LGFX_Sprite &s, int x0, int x1, const char *text, bool on) {
    s.fillRoundRect(x0, 1, x1 - x0, HDR_H - 2, 3, on ? C_BTN_ON : C_BTN);
    s.setTextColor(on ? C_BG : C_TEXT);
    s.setTextDatum(textdatum_t::middle_center);
    s.drawString(text, (x0 + x1) / 2, HDR_H / 2);
}

void Renderer::draw_header(const UiView &v) {
    LGFX_Sprite &s = hdr_;
    s.fillScreen(C_BG);
    s.setFont(&fonts::Font2);
    button(s, 0, MODE_X1, mode_label(v.mode), true);

    char buf[48];
    s.setFont(&fonts::Font0);
    s.setTextDatum(textdatum_t::middle_left);
    if (v.rx_errors) {
        snprintf(buf, sizeof(buf), "RX ERR %u", v.rx_errors);
        s.setTextColor(C_ERR);
    } else if (v.floor_db == v.floor_db) {
        snprintf(buf, sizeof(buf), "%4.1f sw/s  floor %.1f dB", v.sweeps_per_s, v.floor_db);
        s.setTextColor(C_AXIS);
    } else {
        snprintf(buf, sizeof(buf), "waiting for the receiver");
        s.setTextColor(C_AXIS);
    }
    s.drawString(buf, MODE_X1 + 6, HDR_H / 2);

    s.setFont(&fonts::Font2);
    if (v.gain < 0) snprintf(buf, sizeof(buf), "- AGC +");
    else snprintf(buf, sizeof(buf), "- G%d +", v.gain);
    button(s, GAIN_X0, GAIN_X1, buf, false);
    button(s, HOLD_X0, SCR_W, "HOLD", v.hold_on);
    s.pushSprite(dst_, 0, HDR_Y);
}

/* -------------------------------------------------------------- spectrum */
static void dashed_vline(LGFX_Sprite &s, int x, int y0, int y1, int on, int off, uint8_t c) {
    if (x < 0 || x >= s.width()) return;
    for (int y = y0; y < y1; y += on + off) s.drawFastVLine(x, y, (y + on < y1 ? on : y1 - y), c);
}

void Renderer::draw_spectrum(const UiView &v) {
    LGFX_Sprite &s = spec_;
    const int H = SPEC_H;
    s.fillScreen(C_BG);
    float lo = v.y_lo, hi = v.y_hi;
    if (!(hi > lo)) hi = lo + 10;
    auto y_of = [&](float db) -> int {
        float y = (hi - db) / (hi - lo) * (H - 1);
        if (y < 0) y = 0;
        if (y > H - 1) y = H - 1;
        return (int)(y + 0.5f);
    };

    /* channel bands: outer (17 MHz) then inner (9 MHz), dashed edges */
    for (int i = 0; i < v.nch; i++) {
        const UiChannel &c = v.ch[i];
        float f = (float)c.nominal_mhz;
        int xo0 = x_of(v, f - FPV_BW_MARK_OUTER_MHZ / 2), xo1 = x_of(v, f + FPV_BW_MARK_OUTER_MHZ / 2);
        int xi0 = x_of(v, f - FPV_BW_MARK_INNER_MHZ / 2), xi1 = x_of(v, f + FPV_BW_MARK_INNER_MHZ / 2);
        s.fillRect(xo0, 0, xo1 - xo0 + 1, H, ch_outer(c.color));
        s.fillRect(xi0, 0, xi1 - xi0 + 1, H, ch_inner(c.color));
        dashed_vline(s, xo0, 0, H, 3, 3, ch_mid(c.color));
        dashed_vline(s, xo1, 0, H, 3, 3, ch_mid(c.color));
        dashed_vline(s, xi0, 0, H, 3, 3, ch_mid(c.color));
        dashed_vline(s, xi1, 0, H, 3, 3, ch_mid(c.color));
    }

    /* dB grid every 10 dB */
    s.setFont(&fonts::Font0);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(C_AXIS);
    for (int d = (int)ceilf(lo / 10.0f) * 10; d <= (int)hi; d += 10) {
        int y = y_of((float)d);
        for (int x = 0; x < SCR_W; x += 4) s.drawPixel(x, y, C_GRID);
        if (y > 8 && y < H - 2) {
            char b[16];
            snprintf(b, sizeof(b), "%d", d);
            s.drawString(b, 1, y - 8);
        }
    }

    /* nominal centres */
    for (int i = 0; i < v.nch; i++)
        dashed_vline(s, x_of(v, (float)v.ch[i].nominal_mhz), 0, H, 1, 2, ch_mid(v.ch[i].color));

    if (v.have_data) {
        /* live sweep (dim), peak hold (dashed), average (bright) */
        auto trace = [&](const float *col, uint8_t colour, int step) {
            int py = -1;
            for (int x = 0; x < COLS; x++) {
                if (!(col[x] == col[x])) { py = -1; continue; }
                int y = y_of(col[x]);
                if (py >= 0 && (step == 1 || (x / step) % 2 == 0)) s.drawLine(x - 1, py, x, y, colour);
                else if (py < 0) s.drawPixel(x, y, colour);
                py = y;
            }
        };
        trace(v.col_raw, C_RAW, 1);
        if (v.hold_on) trace(v.col_hold, C_HOLD, 3);
        trace(v.col_avg, C_AVG, 1);
    }

    /* measured centres + channel names */
    s.setFont(&fonts::Font0);
    s.setTextDatum(textdatum_t::top_center);
    for (int i = 0; i < v.nch; i++) {
        const UiChannel &c = v.ch[i];
        if (c.smoothed == c.smoothed) {
            int x = x_of(v, c.smoothed);
            if (c.m.present) s.drawFastVLine(x, 10, H - 10, ch_full(c.color));
            else dashed_vline(s, x, 10, H, 2, 3, ch_full(c.color));
        }
        s.setTextColor(ch_full(c.color));
        s.drawString(c.name, x_of(v, (float)c.nominal_mhz), 1);
    }

    if (v.message) {
        s.setFont(&fonts::Font2);
        s.setTextDatum(textdatum_t::middle_center);
        int w = s.textWidth(v.message) + 16;
        s.fillRoundRect((SCR_W - w) / 2, H / 2 - 12, w, 24, 4, C_BTN);
        s.setTextColor(C_TEXT);
        s.drawString(v.message, SCR_W / 2, H / 2);
    }
    s.pushSprite(dst_, 0, SPEC_Y);
}

/* ------------------------------------------------------------------ axis */
void Renderer::draw_axis(const UiView &v) {
    LGFX_Sprite &s = axis_;
    s.fillScreen(C_BG);
    if (v.plan.nbins > 0) {
        float span = (float)v.plan.nbins / DSP_BINS_PER_MHZ;
        int tick = span > 80 ? 20 : span > 40 ? 10 : 5;
        int f0 = ((v.plan.start_mhz + tick - 1) / tick) * tick;
        s.setFont(&fonts::Font0);
        s.setTextDatum(textdatum_t::top_center);
        s.setTextColor(C_AXIS);
        for (int f = f0; (float)f < (float)v.plan.start_mhz + span; f += tick) {
            int x = x_of(v, (float)f);
            s.drawFastVLine(x, 0, 2, C_AXIS);
            char b[16];
            snprintf(b, sizeof(b), "%d", f);
            int w = s.textWidth(b);
            if (x - w / 2 >= 0 && x + w / 2 < SCR_W) s.drawString(b, x, 2);
        }
    }
    s.pushSprite(dst_, 0, AXIS_Y);
}

/* ------------------------------------------------------------- waterfall */
void Renderer::draw_waterfall(const UiView &v) {
    LGFX_Sprite &s = wf_;
    /* Map the stored half-dB codes through the current levels, as the PC
     * version re-levels the whole image every frame. */
    uint8_t lut[256];
    float lo = v.wf_lo, hi = v.wf_hi > v.wf_lo ? v.wf_hi : v.wf_lo + 10;
    for (int c = 0; c < 256; c++) {
        float db = -128.0f + c * 0.5f;
        float t = (db - lo) / (hi - lo) * (WF_LEVELS - 1);
        if (t < 0) t = 0;
        if (t > WF_LEVELS - 1) t = WF_LEVELS - 1;
        lut[c] = (uint8_t)(t + 0.5f);
    }
    uint8_t *buf = (uint8_t *)s.getBuffer();
    bool direct = buf && s.bufferLength() == (uint32_t)(SCR_W * WF_H);
    if (!direct) s.startWrite();
    for (int r = 0; r < WF_H; r++) {
        uint8_t *dst = direct ? buf + r * SCR_W : nullptr;
        bool have = r < v.wf_rows;
        const uint8_t *src = v.wf[(v.wf_head - r + WF_H) % WF_H];
        for (int x = 0; x < COLS; x++) {
            uint8_t px = have ? lut[src[x]] : 0;
            if (direct) dst[x] = px;
            else s.writePixel(x, r, px);
        }
    }
    if (!direct) s.endWrite();
    for (int i = 0; i < v.nch; i++) {
        int x = x_of(v, (float)v.ch[i].nominal_mhz);
        for (int y = 0; y < WF_H; y += 4) s.drawPixel(x, y, WF_LEVELS + v.ch[i].color);
    }
    s.pushSprite(dst_, 0, WF_Y);
}

/* -------------------------------------------------------------- readouts */
static const char *reason_text(const an_meas_t &m, char *buf, size_t n) {
    if (m.reason == AN_LOW_SNR) snprintf(buf, n, "SNR %.0f low", m.snr_db);
    else snprintf(buf, n, "no signal");
    return buf;
}

void Renderer::readout_box(const UiView &v, const UiChannel &c, int x, int w) {
    LGFX_Sprite &s = ro_;
    char b[32];
    s.fillRoundRect(x + 1, 1, w - 2, RO_H - 2, 3, C_PANEL);
    s.drawRoundRect(x + 1, 1, w - 2, RO_H - 2, 3, ch_full(c.color));
    s.setFont(&fonts::Font0);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(ch_full(c.color));
    s.drawString(c.name, x + 5, 4);
    s.setTextColor(C_AXIS);
    snprintf(b, sizeof(b), "%d", c.nominal_mhz);
    s.drawString(b, x + 5 + s.textWidth(c.name) + 4, 4);

    bool live = c.m.present;
    s.setFont(&fonts::Font2);
    s.setTextDatum(textdatum_t::top_center);
    s.setTextColor(live ? C_TEXT : C_DIM);
    if (c.smoothed == c.smoothed) snprintf(b, sizeof(b), "%.2f", c.smoothed);
    else snprintf(b, sizeof(b), "----.--");
    s.drawString(b, x + w / 2, 13);

    float off = live ? c.smoothed - (float)c.nominal_mhz : __builtin_nanf("");
    s.setTextColor(dev_colour(off));
    if (off == off) snprintf(b, sizeof(b), "%+.2f", off);
    else snprintf(b, sizeof(b), "--");
    s.drawString(b, x + w / 2, 29);

    s.setFont(&fonts::Font0);
    s.setTextColor(live ? C_AXIS : C_DIM);
    if (live) {
        if (c.m.bw3_mhz == c.m.bw3_mhz) snprintf(b, sizeof(b), "%.0fdB bw%.1f", c.m.peak_db, c.m.bw3_mhz);
        else snprintf(b, sizeof(b), "%.0fdB", c.m.peak_db);
    } else {
        reason_text(c.m, b, sizeof(b));
    }
    s.drawString(b, x + w / 2, 48);
}

void Renderer::readout_single(const UiView &v, const UiChannel &c) {
    LGFX_Sprite &s = ro_;
    char b[48];
    const int x0 = ARROW_W + 2, x1 = SCR_W - ARROW_W - 2;
    /* < and > */
    s.fillRoundRect(1, 1, ARROW_W - 2, RO_H - 2, 3, C_BTN);
    s.fillRoundRect(SCR_W - ARROW_W + 1, 1, ARROW_W - 2, RO_H - 2, 3, C_BTN);
    s.fillTriangle(ARROW_W / 2 + 5, RO_H / 2 - 9, ARROW_W / 2 + 5, RO_H / 2 + 9, ARROW_W / 2 - 6, RO_H / 2, C_TEXT);
    s.fillTriangle(SCR_W - ARROW_W / 2 - 5, RO_H / 2 - 9, SCR_W - ARROW_W / 2 - 5, RO_H / 2 + 9,
                   SCR_W - ARROW_W / 2 + 6, RO_H / 2, C_TEXT);

    s.fillRoundRect(x0, 1, x1 - x0, RO_H - 2, 3, C_PANEL);
    s.drawRoundRect(x0, 1, x1 - x0, RO_H - 2, 3, ch_full(c.color));

    bool live = c.m.present;
    /* name and nominal */
    s.setFont(&fonts::Font2);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(ch_full(c.color));
    s.drawString(c.name, x0 + 5, 3);
    s.setFont(&fonts::Font0);
    s.setTextColor(C_AXIS);
    snprintf(b, sizeof(b), "%d MHz", c.nominal_mhz);
    s.drawString(b, x0 + 30, 7);

    /* measured centre, large */
    s.setFont(&fonts::Font4);
    s.setTextColor(live ? C_TEXT : C_DIM);
    if (c.smoothed == c.smoothed) snprintf(b, sizeof(b), "%.2f", c.smoothed);
    else snprintf(b, sizeof(b), "----.--");
    s.drawString(b, x0 + 5, 20);
    int wv = s.textWidth(b);
    s.setFont(&fonts::Font0);
    s.drawString("MHz", x0 + 8 + wv, 36);

    /* deviation, top right */
    float off = live ? c.smoothed - (float)c.nominal_mhz : __builtin_nanf("");
    s.setFont(&fonts::Font2);
    s.setTextDatum(textdatum_t::top_right);
    s.setTextColor(dev_colour(off));
    if (off == off) snprintf(b, sizeof(b), "%+.2f MHz", off);
    else snprintf(b, sizeof(b), "-- MHz");
    s.drawString(b, x1 - 5, 3);

    /* details */
    s.setFont(&fonts::Font0);
    s.setTextDatum(textdatum_t::top_left);
    const int xd = x0 + 150;
    s.setTextColor(live ? C_AXIS : C_DIM);
    if (c.m.peak_db == c.m.peak_db) snprintf(b, sizeof(b), "peak %6.1f dB", c.m.peak_db);
    else snprintf(b, sizeof(b), "peak     -- dB");
    s.drawString(b, xd, 22);
    if (c.m.snr_db == c.m.snr_db) snprintf(b, sizeof(b), "SNR  %6.1f dB", c.m.snr_db);
    else snprintf(b, sizeof(b), "SNR      -- dB");
    s.drawString(b, xd, 32);
    if (v.floor_db == v.floor_db) snprintf(b, sizeof(b), "floor%6.1f dB", v.floor_db);
    else snprintf(b, sizeof(b), "floor    -- dB");
    s.drawString(b, xd, 42);

    if (live) {
        char b3[12], b20[12];
        if (c.m.bw3_mhz == c.m.bw3_mhz) snprintf(b3, sizeof(b3), "%.2f", c.m.bw3_mhz);
        else snprintf(b3, sizeof(b3), "--");
        if (c.m.bw20_mhz == c.m.bw20_mhz) snprintf(b20, sizeof(b20), "%.2f", c.m.bw20_mhz);
        else snprintf(b20, sizeof(b20), "--");
        snprintf(b, sizeof(b), "-3dB %s  -20dB %s MHz", b3, b20);
        s.setTextColor(C_AXIS);
    } else {
        reason_text(c.m, b, sizeof(b));
        s.setTextColor(C_DIM);
    }
    s.drawString(b, x0 + 5, 50);
}

void Renderer::draw_readouts(const UiView &v) {
    LGFX_Sprite &s = ro_;
    s.fillScreen(C_BG);
    if (v.mode == MODE_1CH && v.nch == 1) {
        readout_single(v, v.ch[0]);
    } else if (v.nch > 0) {
        int w = SCR_W / v.nch;
        for (int i = 0; i < v.nch; i++) readout_box(v, v.ch[i], i * w, i == v.nch - 1 ? SCR_W - i * w : w);
    }
    s.pushSprite(dst_, 0, RO_Y);
}

}  // namespace fpv
