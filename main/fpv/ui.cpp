#include "ui.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "channels.h"
#include "fpv_config.h"

namespace fpv {

/* Scale of the 320 x 240 layout: SX horizontal, SY vertical; fonts by role. */
#if FPV_SCREEN_480
static constexpr int SX(int v) { return v * 3 / 2; }
static constexpr int SY(int v) { return v * 4 / 3; }
#define F_S (&fonts::DejaVu12)
#define F_M (&fonts::DejaVu18)
#define F_L (&fonts::DejaVu24)
#define F_XL (&fonts::DejaVu40)
#else
static constexpr int SX(int v) { return v; }
static constexpr int SY(int v) { return v; }
#define F_S (&fonts::Font0)
#define F_M (&fonts::Font2)
#define F_L (&fonts::Font4)
#define F_XL (&fonts::Font4)
#endif

/* ---------------------------------------------------------------- colours */
/* UI palette (spectrum, header, axis, readouts): index -> RGB. */
enum : uint8_t {
    C_BG = 0, C_GRID, C_AXIS, C_TEXT, C_DIM, C_RAW, C_AVG, C_HOLD, C_PANEL,
    C_BTN, C_BTN_ON, C_DEV_OK, C_DEV_WARN, C_DEV_BAD, C_DEV_IDLE, C_ERR,
    C_CH0 = 16,   /* + 4 * slot + {0 full, 1 mid, 2 inner band, 3 outer band} */
    C_OK_BG = 40, C_WARN_BG, C_BAD_BG,   /* readout panels tinted by the deviation */
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
    set_rgb(s, C_OK_BG, blend(UI_RGB[C_DEV_OK], UI_RGB[C_PANEL], 0.22f));
    set_rgb(s, C_WARN_BG, blend(UI_RGB[C_DEV_WARN], UI_RGB[C_PANEL], 0.22f));
    set_rgb(s, C_BAD_BG, blend(UI_RGB[C_DEV_BAD], UI_RGB[C_PANEL], 0.22f));
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
    case MODE_4CH2: return "4CH2";
    case MODE_4CH3: return "4CH3";
    case MODE_3CH: return "3CH";
    case MODE_PC: return "PC";
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

/* readout panel: tinted by the deviation's colour, plain without a signal */
static uint8_t panel_colour(float off) {
    uint8_t c = dev_colour(off);
    return c == C_DEV_OK ? C_OK_BG : c == C_DEV_WARN ? C_WARN_BG : c == C_DEV_BAD ? C_BAD_BG : C_PANEL;
}

/* -------------------------------------------------------------- hit test */
/* Header: [channels 0..121] status battery [SETUP 196..255] [HOLD 258..319] (on
 * the 320 px screen). Taps reach 24 px below it (about 7 mm in all) so a
 * fingertip can hit them. */
constexpr int MODE_X1 = SX(122), BAT_X1 = SX(192), SET_X0 = SX(196), SET_X1 = SX(256), HOLD_X0 = SX(258);
constexpr int HDR_TAP_H = HDR_H + SY(24);
constexpr int ARROW_W = SX(44);   /* 1ch readout: < and > buttons (8 mm) */

Hit hit_test(const UiView &v, int x, int y) {
    if (y < HDR_TAP_H) {
        if (x < MODE_X1 + SX(4)) return HIT_MODE;
        if (x >= SET_X0 - SX(4) && x < SET_X1 + 1) return HIT_SETUP;
        if (x >= HOLD_X0 - 1) return HIT_HOLD;
        return HIT_NONE;
    }
    if (y < RO_Y) return HIT_PLOT;
    if (v.mode == MODE_1CH && v.nch == 1) {
        if (x < ARROW_W) return HIT_CH_PREV;
        if (x >= SCR_W - ARROW_W) return HIT_CH_NEXT;
        return HIT_BACK;
    }
    if (v.mode != MODE_PC && v.nch > 1) {
        int i = x / (SCR_W / v.nch);
        if (i > v.nch - 1) i = v.nch - 1;
        return (Hit)(HIT_CARD0 + i);
    }
    return HIT_NONE;
}

/* Setup layout: title row, then GAIN / DETECT / INFO / TOUCH rows. */
constexpr int SU_TITLE_H = SY(40), SU_ROW_Y = SY(44), SU_ROW_PITCH = SY(49), SU_ROW_H = SY(45);
constexpr int SU_COL0 = SX(62), SU_STEP_W = SX(64);
constexpr int SU_CAL_X1 = SU_COL0 + SX(120), SU_DONE_X0 = SX(226);

SetupHit setup_hit(int x, int y) {
    if (y < SU_TITLE_H) return x >= SU_DONE_X0 ? SH_DONE : SH_NONE;
    int row = (y - SU_ROW_Y + 2) / SU_ROW_PITCH;
    if (y < SU_ROW_Y - 2 || row > 3 || x < SU_COL0 - 2) return SH_NONE;
    if (row == 3) return x < SU_CAL_X1 ? SH_CAL : SH_NONE;
    if (row == 2) return SH_NONE;
    bool left = x < SU_COL0 + SU_STEP_W + SX(4), right = x >= SCR_W - SU_STEP_W - SX(4);
    if (!left && !right) return SH_NONE;
    if (row == 0) return left ? SH_GAIN_DOWN : SH_GAIN_UP;
    return left ? SH_SNR_DOWN : SH_SNR_UP;
}

/* Mode list and Wi-Fi page: a title row with CLOSE (the Wi-Fi page has SCAN
 * left of it), then the content. */
constexpr int PG_TITLE_H = SY(36), PG_CLOSE_X0 = SU_DONE_X0;
constexpr int PG_BTN2_X0 = SX(140), PG_BTN2_X1 = SX(220);
constexpr int MODE_ROW_H = (SCR_H - PG_TITLE_H) / GROUP_COUNT;
constexpr int MODE_WIFI_X0 = SCR_W - SX(62);   /* each group's WiFi button */

int modes_hit(int x, int y, bool *wifi) {
    *wifi = false;
    if (y < PG_TITLE_H) return x >= PG_CLOSE_X0 ? PICK_CLOSE : PICK_NONE;
    int i = (y - PG_TITLE_H) / MODE_ROW_H;
    if (i > GROUP_COUNT - 1) i = GROUP_COUNT - 1;
    *wifi = x >= MODE_WIFI_X0 - SX(2);
    return i;
}

int wifi_hit(int x, int y) {
    if (y >= PG_TITLE_H) return PICK_NONE;
    return x >= PG_CLOSE_X0 ? PICK_CLOSE : (x >= PG_BTN2_X0 && x < PG_BTN2_X1) ? PICK_RESCAN : PICK_NONE;
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
    s.fillRoundRect(x0, 1, x1 - x0, HDR_H - 2, SY(3), on ? C_BTN_ON : C_BTN);
    s.setTextColor(on ? C_BG : C_TEXT);
    s.setTextDatum(textdatum_t::middle_center);
    s.drawString(text, (x0 + x1) / 2, HDR_H / 2);
}

void Renderer::draw_header(const UiView &v) {
    LGFX_Sprite &s = hdr_;
    char buf[48];
    s.fillScreen(C_BG);

    /* channels / mode: the names in their colours, a down arrow: the list */
    s.fillRoundRect(0, 1, MODE_X1, HDR_H - 2, SY(3), C_BTN);
    s.setFont(F_M);
    s.setTextDatum(textdatum_t::middle_left);
    int x = SX(5);
    const int gap = s.textWidth(" ");
    if (v.mode == MODE_1CH || v.mode == MODE_PC) {
        const char *t = v.mode == MODE_PC ? "PC" : mode_label(v.back_mode);   /* 1CH: its group */
        s.setTextColor(C_AXIS);
        s.drawString(t, x, HDR_H / 2);
        x += s.textWidth(t) + gap;
    }
    for (int i = 0; i < v.nch; i++) {
        s.setTextColor(ch_full(v.ch[i].color));
        s.drawString(v.ch[i].name, x, HDR_H / 2);
        x += s.textWidth(v.ch[i].name) + gap;
    }
    const int ax = MODE_X1 - SX(8), ah = SY(3);
    s.fillTriangle(ax - SX(4), HDR_H / 2 - ah, ax + SX(4), HDR_H / 2 - ah, ax, HDR_H / 2 + ah, C_AXIS);

    s.setFont(F_S);
    if (v.rx_errors) {
        s.setTextDatum(textdatum_t::middle_left);
        snprintf(buf, sizeof(buf), "ERR %u", v.rx_errors);
        s.setTextColor(C_ERR);
        s.drawString(buf, MODE_X1 + SX(5), HDR_H / 2);
    }

    /* battery: icon and volts, right-aligned before SET */
    if (v.bat_mv != 0) {
        s.setTextDatum(textdatum_t::middle_right);
        if (v.bat_mv < FPV_BAT_ABSENT_MV) {
            s.setTextColor(C_AXIS);
            s.drawString("USB", BAT_X1, HDR_H / 2);
        } else {
            bool low = FPV_BAT_CUTOFF_MV && v.bat_mv < FPV_BAT_CUTOFF_MV + 200;
            uint8_t col = low ? C_DEV_BAD : C_TEXT;
            snprintf(buf, sizeof(buf), "%d.%02dV", v.bat_mv / 1000, (v.bat_mv % 1000) / 10);
            s.setTextColor(low ? C_DEV_BAD : C_AXIS);
            s.drawString(buf, BAT_X1, HDR_H / 2);
            const int bw = SX(18), bh = HDR_H - SY(7);
            const int bx = BAT_X1 - s.textWidth(buf) - SX(5) - bw - SX(2), by = (HDR_H - bh) / 2;
            s.drawRect(bx, by, bw, bh, col);
            s.fillRect(bx + bw, by + bh / 4, SX(2), bh - 2 * (bh / 4), col);
            float f = (float)(v.bat_mv - FPV_BAT_EMPTY_MV) / (FPV_BAT_FULL_MV - FPV_BAT_EMPTY_MV);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            int fw = (int)((bw - 4) * f + 0.5f);
            if (fw > 0) s.fillRect(bx + 2, by + 2, fw, bh - 4, col);
        }
    }
    s.setFont(F_M);
    button(s, SET_X0, SET_X1, "SETUP", false);
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
        dashed_vline(s, xo0, 0, H, SY(3), SY(3), ch_mid(c.color));
        dashed_vline(s, xo1, 0, H, SY(3), SY(3), ch_mid(c.color));
        dashed_vline(s, xi0, 0, H, SY(3), SY(3), ch_mid(c.color));
        dashed_vline(s, xi1, 0, H, SY(3), SY(3), ch_mid(c.color));
    }

    /* dB grid every 10 dB (20 on a tall range); the top label has the unit */
    s.setFont(F_S);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(C_AXIS);
    const int fh = s.fontHeight();
    const int step = hi - lo > 60 ? 20 : 10;
    bool unit = true;
    for (int d = (int)floorf(hi / step) * step; (float)d >= lo; d -= step) {
        int y = y_of((float)d);
        for (int x = 0; x < SCR_W; x += 4) s.drawPixel(x, y, C_GRID);
        if (y > fh && y < H - 2) {
            char b[16];
            snprintf(b, sizeof(b), unit ? "%d dB" : "%d", d);
            unit = false;
            s.drawString(b, 1, y - fh);
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
        if (v.hold_on) trace(v.col_hold, C_HOLD, SX(3));
        trace(v.col_avg, C_AVG, 1);
    }

    /* measured centres + channel names */
    s.setFont(F_S);
    s.setTextDatum(textdatum_t::top_center);
    for (int i = 0; i < v.nch; i++) {
        const UiChannel &c = v.ch[i];
        if (c.smoothed == c.smoothed) {
            int x = x_of(v, c.smoothed);
            const int top = s.fontHeight() + 2;
            if (c.m.present) s.drawFastVLine(x, top, H - top, ch_full(c.color));
            else dashed_vline(s, x, top, H, SY(2), SY(3), ch_full(c.color));
        }
        s.setTextColor(ch_full(c.color));
        s.drawString(c.name, x_of(v, (float)c.nominal_mhz), 1);
    }

    /* the ADC clips: too much gain (or a VTX very close) */
    if (v.clip_permille >= FPV_SAT_PERMILLE) {
        char b[24];
        snprintf(b, sizeof(b), "SAT %d%%", (v.clip_permille + 5) / 10);
        s.setFont(F_M);
        s.setTextDatum(textdatum_t::middle_center);
        int w = s.textWidth(b) + SX(10), h = s.fontHeight() + SY(4), y0 = SY(14);
        s.fillRoundRect(SCR_W - w - SX(2), y0, w, h, SY(3), C_ERR);
        s.setTextColor(C_TEXT);
        s.drawString(b, SCR_W - SX(2) - w / 2, y0 + h / 2);
    }

    if (v.message) {
        s.setFont(F_M);
        s.setTextDatum(textdatum_t::middle_center);
        int w = s.textWidth(v.message) + SX(16), h = s.fontHeight() + SY(8);
        s.fillRoundRect((SCR_W - w) / 2, H / 2 - h / 2, w, h, 4, C_BTN);
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
        s.setFont(F_S);
        s.setTextDatum(textdatum_t::top_center);
        s.setTextColor(C_AXIS);
        for (int f = f0; (float)f < (float)v.plan.start_mhz + span; f += tick) {
            int x = x_of(v, (float)f);
            s.drawFastVLine(x, 0, SY(2), C_AXIS);
            char b[16];
            snprintf(b, sizeof(b), "%d", f);
            int w = s.textWidth(b);
            if (x - w / 2 >= 0 && x + w / 2 < SCR_W) s.drawString(b, x, SY(2));
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
        for (int y = 0; y < WF_H; y += SY(4)) s.drawPixel(x, y, WF_LEVELS + v.ch[i].color);
    }
    s.pushSprite(dst_, 0, WF_Y);
}

/* -------------------------------------------------------------- readouts */
/* WEAK: a peak 6 dB or more above the floor, short of the threshold */
static const char *absent_text(const an_meas_t &m) {
    return m.reason == AN_LOW_SNR && m.snr_db >= FPV_SNR_MIN_DB ? "WEAK" : "NO SIGNAL";
}

/* One card per channel: the deviation is the point, large and in its colour
 * on a panel tinted the same way; the measured centre underneath. */
void Renderer::readout_box(const UiView &v, const UiChannel &c, int x, int w) {
    LGFX_Sprite &s = ro_;
    char b[32];
    bool live = c.m.present && c.smoothed == c.smoothed;
    float off = live ? c.smoothed - (float)c.nominal_mhz : __builtin_nanf("");
    s.fillRoundRect(x + 1, 1, w - 2, RO_H - 2, SY(4), panel_colour(off));
    s.drawRoundRect(x + 1, 1, w - 2, RO_H - 2, SY(4), ch_full(c.color));
    s.drawRoundRect(x + 2, 2, w - 4, RO_H - 4, SY(3), ch_full(c.color));

    s.setFont(F_M);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(ch_full(c.color));
    s.drawString(c.name, x + SX(5), SY(3));
    s.setFont(F_S);
    s.setTextDatum(textdatum_t::top_right);
    s.setTextColor(C_AXIS);
    snprintf(b, sizeof(b), "%d", c.nominal_mhz);
    s.drawString(b, x + w - SX(5), SY(5));

    if (live) {
        s.setFont(F_L);
        s.setTextDatum(textdatum_t::middle_center);
        s.setTextColor(dev_colour(off));
        snprintf(b, sizeof(b), "%+.2f", off);
        s.drawString(b, x + w / 2, RO_H / 2 + SY(2));
        s.setFont(F_S);
        s.setTextDatum(textdatum_t::bottom_center);
        s.setTextColor(C_AXIS);
        snprintf(b, sizeof(b), "%.2f MHz", c.smoothed);
        s.drawString(b, x + w / 2, RO_H - SY(5));
    } else {
        s.setFont(F_M);
        s.setTextDatum(textdatum_t::middle_center);
        s.setTextColor(C_DIM);
        s.drawString(absent_text(c.m), x + w / 2, RO_H / 2 + SY(4));
    }
}

/* 1CH: < > step the channel; the middle shows the deviation large, the
 * measurement on the right, and a tap there goes back to the 4CH / 3CH list. */
void Renderer::readout_single(const UiView &v, const UiChannel &c) {
    LGFX_Sprite &s = ro_;
    char b[48];
    const int x0 = ARROW_W + 2, x1 = SCR_W - ARROW_W - 2;
    /* < and > */
    s.fillRoundRect(1, 1, ARROW_W - 2, RO_H - 2, 3, C_BTN);
    s.fillRoundRect(SCR_W - ARROW_W + 1, 1, ARROW_W - 2, RO_H - 2, 3, C_BTN);
    const int ta = SX(5), tb = SX(6), th = SY(9);
    s.fillTriangle(ARROW_W / 2 + ta, RO_H / 2 - th, ARROW_W / 2 + ta, RO_H / 2 + th, ARROW_W / 2 - tb, RO_H / 2, C_TEXT);
    s.fillTriangle(SCR_W - ARROW_W / 2 - ta, RO_H / 2 - th, SCR_W - ARROW_W / 2 - ta, RO_H / 2 + th,
                   SCR_W - ARROW_W / 2 + tb, RO_H / 2, C_TEXT);

    bool live = c.m.present && c.smoothed == c.smoothed;
    float off = live ? c.smoothed - (float)c.nominal_mhz : __builtin_nanf("");
    s.fillRoundRect(x0, 1, x1 - x0, RO_H - 2, SY(4), panel_colour(off));
    s.drawRoundRect(x0, 1, x1 - x0, RO_H - 2, SY(4), ch_full(c.color));
    s.drawRoundRect(x0 + 1, 2, x1 - x0 - 2, RO_H - 4, SY(3), ch_full(c.color));

    /* name and nominal */
    s.setFont(F_M);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(ch_full(c.color));
    s.drawString(c.name, x0 + SX(5), SY(3));
    int wn = s.textWidth(c.name);
    s.setFont(F_S);
    s.setTextColor(C_AXIS);
    snprintf(b, sizeof(b), "%d MHz", c.nominal_mhz);
    s.drawString(b, x0 + SX(9) + wn, SY(5));

    /* deviation, large */
    if (live) {
        s.setFont(F_XL);
        s.setTextColor(dev_colour(off));
        snprintf(b, sizeof(b), "%+.2f", off);
        s.drawString(b, x0 + SX(5), SY(20));
        int wv = s.textWidth(b), hv = s.fontHeight();
        s.setFont(F_S);
        s.drawString("MHz", x0 + SX(8) + wv, SY(20) + hv - s.fontHeight() - SY(2));
    } else {
        s.setFont(F_L);
        s.setTextColor(C_DIM);
        s.drawString(absent_text(c.m), x0 + SX(5), SY(26));
    }

    /* measurement, right column */
    const int xd = x1 - SX(96);
    s.setFont(F_M);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(live ? C_TEXT : C_DIM);
    if (live) snprintf(b, sizeof(b), "%.2f", c.smoothed);
    else snprintf(b, sizeof(b), "----.--");
    s.drawString(b, xd, SY(3));
    s.setFont(F_S);
    s.setTextColor(C_AXIS);
    if (c.m.snr_db == c.m.snr_db) snprintf(b, sizeof(b), "SNR %.0f  peak %.0f", c.m.snr_db, c.m.peak_db);
    else snprintf(b, sizeof(b), "SNR --");
    s.drawString(b, xd, SY(22));
    if (live && c.m.bw3_mhz == c.m.bw3_mhz && c.m.bw20_mhz == c.m.bw20_mhz)
        snprintf(b, sizeof(b), "bw %.1f / %.1f MHz", c.m.bw3_mhz, c.m.bw20_mhz);
    else if (live && c.m.bw3_mhz == c.m.bw3_mhz)
        snprintf(b, sizeof(b), "bw %.1f MHz", c.m.bw3_mhz);
    else
        snprintf(b, sizeof(b), "bw --");
    s.drawString(b, xd, SY(33));
    s.setTextColor(C_DIM);
    snprintf(b, sizeof(b), "tap: back to %s", mode_label(v.back_mode));
    s.drawString(b, xd, SY(48));
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

/* ------------------------------------------------------------------ pages */
static uint16_t rgb(uint32_t c) { return lgfx::color565((c >> 16) & 255, (c >> 8) & 255, c & 255); }

void Renderer::draw_setup(const SetupView &sv) {
    lgfx::LovyanGFX &d = *dst_;
    const uint16_t bg = rgb(UI_RGB[C_BG]), btn = rgb(UI_RGB[C_BTN]), on = rgb(UI_RGB[C_BTN_ON]);
    const uint16_t text = rgb(UI_RGB[C_TEXT]), axis = rgb(UI_RGB[C_AXIS]), err = rgb(UI_RGB[C_ERR]);
    char b[64];
    d.startWrite();
    d.fillScreen(bg);

    auto box = [&](int x0, int y0, int x1, int y1, const char *t, bool sel, const lgfx::IFont *f) {
        d.fillRoundRect(x0, y0, x1 - x0, y1 - y0, 5, sel ? on : btn);
        d.setFont(f);
        d.setTextDatum(textdatum_t::middle_center);
        d.setTextColor(sel ? bg : text);
        d.drawString(t, (x0 + x1) / 2, (y0 + y1) / 2);
    };
    auto label = [&](int row, const char *t) {
        d.setFont(F_M);
        d.setTextDatum(textdatum_t::middle_left);
        d.setTextColor(axis);
        d.drawString(t, SX(4), SU_ROW_Y + row * SU_ROW_PITCH + SU_ROW_H / 2);
    };
    auto stepper = [&](int row, const char *value, const char *hint, uint16_t hint_col) {
        int y0 = SU_ROW_Y + row * SU_ROW_PITCH, y1 = y0 + SU_ROW_H;
        box(SU_COL0, y0, SU_COL0 + SU_STEP_W, y1, "-", false, F_L);
        box(SCR_W - SU_STEP_W, y0, SCR_W - 1, y1, "+", false, F_L);
        int cx = (SU_COL0 + SU_STEP_W + SCR_W - SU_STEP_W) / 2;
        d.setFont(F_L);
        d.setTextDatum(textdatum_t::middle_center);
        d.setTextColor(text);
        d.drawString(value, cx, (y0 + y1) / 2 - SY(6));
        d.setFont(F_S);
        d.setTextColor(hint_col);
        d.drawString(hint, cx, y1 - SY(6));
    };

    /* title row */
    d.setFont(F_L);
    d.setTextDatum(textdatum_t::middle_left);
    d.setTextColor(text);
    d.drawString("SETUP", SX(6), SU_TITLE_H / 2);
    box(SU_DONE_X0, 2, SCR_W - 1, SU_TITLE_H - 2, "DONE", true, F_L);

    /* GAIN */
    label(0, "GAIN");
    if (sv.gain < 0) snprintf(b, sizeof(b), "AUTO");
    else snprintf(b, sizeof(b), "%d", sv.gain);
    if (sv.clip_permille >= FPV_SAT_PERMILLE) {
        char h[32];
        snprintf(h, sizeof(h), "SAT %d%% - lower it", (sv.clip_permille + 5) / 10);
        stepper(0, b, h, err);
    } else {
        stepper(0, b, "higher = weaker VTX seen", axis);
    }
    /* detection threshold */
    label(1, "DETECT");
    snprintf(b, sizeof(b), "%d dB", sv.snr_db);
    stepper(1, b, "signal above noise", axis);
    /* INFO */
    label(2, "INFO");
    {
        int y0 = SU_ROW_Y + 2 * SU_ROW_PITCH;
        d.setFont(F_S);
        d.setTextDatum(textdatum_t::top_left);
        d.setTextColor(text);
        if (sv.floor_db == sv.floor_db)
            snprintf(b, sizeof(b), "%.1f sweeps/s   noise %.0f dB", sv.sweeps_per_s, sv.floor_db);
        else
            snprintf(b, sizeof(b), "waiting for the first sweep");
        d.drawString(b, SU_COL0, y0 + SY(8));
        if (sv.bat_mv == 0) snprintf(b, sizeof(b), "battery --");
        else if (sv.bat_mv < FPV_BAT_ABSENT_MV) snprintf(b, sizeof(b), "USB power");
        else snprintf(b, sizeof(b), "battery %d.%02d V", sv.bat_mv / 1000, (sv.bat_mv % 1000) / 10);
        if (sv.rx_errors) {
            size_t n = strlen(b);
            snprintf(b + n, sizeof(b) - n, "   RX errors %u", sv.rx_errors);
        }
        d.drawString(b, SU_COL0, y0 + SY(24));
    }
    /* TOUCH */
    label(3, "TOUCH");
    {
        int y0 = SU_ROW_Y + 3 * SU_ROW_PITCH;
        box(SU_COL0, y0, SU_CAL_X1, y0 + SU_ROW_H, "CALIBRATE", false, F_M);
    }
    d.endWrite();
}

/* title row of the mode list and the channel grid */
static void page_title(lgfx::LovyanGFX &d, const char *title) {
    d.fillScreen(rgb(UI_RGB[C_BG]));
    d.setFont(F_L);
    d.setTextDatum(textdatum_t::middle_left);
    d.setTextColor(rgb(UI_RGB[C_TEXT]));
    d.drawString(title, SX(6), PG_TITLE_H / 2);
    d.fillRoundRect(PG_CLOSE_X0, 2, SCR_W - 1 - PG_CLOSE_X0, PG_TITLE_H - 4, 5, rgb(UI_RGB[C_BTN]));
    d.setFont(F_M);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString("CLOSE", (PG_CLOSE_X0 + SCR_W - 1) / 2, PG_TITLE_H / 2);
}

/* the second title button, left of CLOSE */
static void page_button(lgfx::LovyanGFX &d, const char *text) {
    d.fillRoundRect(PG_BTN2_X0, 2, PG_BTN2_X1 - PG_BTN2_X0, PG_TITLE_H - 4, 5, rgb(UI_RGB[C_BTN]));
    d.setFont(F_M);
    d.setTextDatum(textdatum_t::middle_center);
    d.setTextColor(rgb(UI_RGB[C_TEXT]));
    d.drawString(text, (PG_BTN2_X0 + PG_BTN2_X1) / 2, PG_TITLE_H / 2);
}

void Renderer::draw_modes(const ModesView &mv) {
    lgfx::LovyanGFX &d = *dst_;
    const uint16_t btn = rgb(UI_RGB[C_BTN]), on = rgb(UI_RGB[C_BTN_ON]), axis = rgb(UI_RGB[C_AXIS]);
    const uint16_t text = rgb(UI_RGB[C_TEXT]), bg = rgb(UI_RGB[C_BG]);
    char b[16];
    d.startWrite();
    page_title(d, "MODE");
    const int ch_x0 = SX(58), ch_w = (MODE_WIFI_X0 - SX(4) - ch_x0) / MAX_CH;
    for (int r = 0; r < GROUP_COUNT; r++) {
        const GroupInfo &row = mv.row[r];
        int y0 = PG_TITLE_H + r * MODE_ROW_H + 1, y1 = y0 + MODE_ROW_H - 3;
        bool sel = row.mode == mv.current;
        d.fillRoundRect(0, y0, MODE_WIFI_X0 - SX(4), y1 - y0, 5, btn);
        if (sel) {
            d.drawRoundRect(0, y0, MODE_WIFI_X0 - SX(4), y1 - y0, 5, on);
            d.drawRoundRect(1, y0 + 1, MODE_WIFI_X0 - SX(4) - 2, y1 - y0 - 2, 4, on);
        }
        d.setFont(F_M);
        d.setTextDatum(textdatum_t::middle_left);
        d.setTextColor(sel ? on : axis);
        d.drawString(mode_label(row.mode), SX(6), (y0 + y1) / 2);
        for (int i = 0; i < row.n; i++) {
            int cx = ch_x0 + i * ch_w + ch_w / 2;
            d.setFont(F_L);
            d.setTextDatum(textdatum_t::bottom_center);
            d.setTextColor(rgb(CH_RGB[row.color[i]]));
            d.drawString(row.name[i], cx, (y0 + y1) / 2 + SY(5));
            d.setFont(F_S);
            d.setTextDatum(textdatum_t::top_center);
            d.setTextColor(axis);
            snprintf(b, sizeof(b), "%d", row.mhz[i]);
            d.drawString(b, cx, (y0 + y1) / 2 + SY(6));
        }
        /* this group's Wi-Fi check */
        d.fillRoundRect(MODE_WIFI_X0, y0, SCR_W - MODE_WIFI_X0, y1 - y0, 5, text);
        d.setFont(F_M);
        d.setTextDatum(textdatum_t::middle_center);
        d.setTextColor(bg);
        d.drawString("WiFi", (MODE_WIFI_X0 + SCR_W) / 2, (y0 + y1) / 2);
    }
    d.endWrite();
}

/* ------------------------------------------------------------- Wi-Fi page */
constexpr float WMAP_LO = 5490.0f, WMAP_HI = 5900.0f;
constexpr int WMAP_X0 = SX(4), WMAP_X1 = SCR_W - SX(4);

static int wmap_x(float mhz) { return WMAP_X0 + (int)((mhz - WMAP_LO) / (WMAP_HI - WMAP_LO) * (WMAP_X1 - WMAP_X0)); }
static int wifi_mhz(int ch) { return ch <= 14 ? 2407 + 5 * ch : 5000 + 5 * ch; }

/* How strongly a Wi-Fi signal shows: 1 at FPV_WIFI_RSSI_STRONG and above,
 * down to floor_a at FPV_WIFI_RSSI_WEAK and below. */
static float wifi_alpha(int rssi, float floor_a) {
    float a = (float)(rssi - FPV_WIFI_RSSI_WEAK) / (FPV_WIFI_RSSI_STRONG - FPV_WIFI_RSSI_WEAK);
    if (a > 1) a = 1;
    return floor_a + (1 - floor_a) * (a < 0 ? 0 : a);
}

/* The strongest access point whose signal covers a Wi-Fi channel. */
static const WifiApInfo *ap_on(const WifiView &w, int ch) {
    const WifiApInfo *best = nullptr;
    int f = wifi_mhz(ch);
    for (int i = 0; i < w.nap; i++) {
        const WifiApInfo &a = w.ap[i];
        if (f > a.lo_mhz && f < a.hi_mhz && (!best || a.rssi > best->rssi)) best = &a;
    }
    return best;
}

void Renderer::draw_wifi(const WifiView &w) {
    lgfx::LovyanGFX &d = *dst_;
    const GroupInfo &g = w.group;
    const uint16_t bg = rgb(UI_RGB[C_BG]), btn = rgb(UI_RGB[C_BTN]), text = rgb(UI_RGB[C_TEXT]);
    const uint16_t axis = rgb(UI_RGB[C_AXIS]), dim = rgb(UI_RGB[C_DIM]), grid = rgb(UI_RGB[C_GRID]);
    const uint32_t RED = 0xff3b30, YELLOW = UI_RGB[C_DEV_WARN];
    const uint16_t green = rgb(0x45d97a);
    char b[64];
    d.startWrite();
    page_title(d, mode_label(g.mode));
    page_button(d, w.running ? "STOP" : "SCAN");
    d.setFont(F_S);
    d.setTextDatum(textdatum_t::middle_left);
    d.setTextColor(text);
    d.drawString("WiFi check", SX(62), PG_TITLE_H / 2 - SY(6));
    d.setTextColor(axis);
    if (w.running && w.current >= 0) snprintf(b, sizeof(b), "ch %d  %d/%d", w.chan[w.current].ch, w.done + 1, w.nch);
    else if (w.nch) snprintf(b, sizeof(b), "%d ch %s", w.nch, w.done == w.nch ? "checked" : "stopped");
    else snprintf(b, sizeof(b), "nothing to check");
    d.drawString(b, SX(62), PG_TITLE_H / 2 + SY(6));

    /* map: the group's channels (17 MHz each), then the Wi-Fi channels checked */
    const int y_grp = PG_TITLE_H + SY(2), grp_h = SY(22);
    const int y_wifi = y_grp + grp_h + SY(2), wifi_h = SY(14);
    const int y_axis = y_wifi + wifi_h + SY(1);
    for (int k = 0; k < g.n; k++) {
        uint32_t c = CH_RGB[g.color[k]];
        int x0 = wmap_x(g.mhz[k] - FPV_BW_MARK_OUTER_MHZ / 2), x1 = wmap_x(g.mhz[k] + FPV_BW_MARK_OUTER_MHZ / 2);
        d.fillRect(x0, y_grp + SY(10), x1 - x0 + 1, grp_h - SY(10), rgb(blend(c, UI_RGB[C_BG], 0.35f)));
        d.drawFastVLine(wmap_x((float)g.mhz[k]), y_grp + SY(10), grp_h - SY(10), rgb(c));
        d.setFont(F_S);
        d.setTextDatum(textdatum_t::top_center);
        d.setTextColor(rgb(c));
        d.drawString(g.name[k], wmap_x((float)g.mhz[k]), y_grp);
    }
    d.setTextDatum(textdatum_t::middle_center);
    for (int i = 0; i < w.nch; i++) {
        const WifiChanInfo &c = w.chan[i];
        int f = wifi_mhz(c.ch), x0 = wmap_x(f - 10.0f) + 1, x1 = wmap_x(f + 10.0f) - 1;
        bool scanned = i < w.done;
        /* heard: red (Direct) / yellow (IMD), fainter the weaker it is */
        float a = scanned && c.frames ? wifi_alpha(c.rssi_max, 0.25f) : 0;
        uint16_t fill = !scanned ? bg : !c.frames ? btn : rgb(blend(c.direct ? RED : YELLOW, UI_RGB[C_BG], a));
        d.fillRect(x0, y_wifi, x1 - x0 + 1, wifi_h, fill);
        d.drawRect(x0, y_wifi, x1 - x0 + 1, wifi_h, (w.running && i == w.current) ? text : grid);
        snprintf(b, sizeof(b), "%d", c.ch);
        d.setTextColor(scanned && c.frames && a > 0.6f ? bg : axis);
        if (d.textWidth(b) <= x1 - x0) d.drawString(b, (x0 + x1) / 2, y_wifi + wifi_h / 2);
    }
    d.setTextDatum(textdatum_t::top_center);
    d.setTextColor(axis);
    for (int f = 5500; f <= 5900; f += 50) {
        int x = wmap_x((float)f);
        d.drawFastVLine(x, y_axis, SY(2), axis);
        snprintf(b, sizeof(b), "%d", f);
        int tw = d.textWidth(b);
        if (x - tw / 2 >= 0 && x + tw / 2 < SCR_W) d.drawString(b, x, y_axis + SY(2));
    }

    /* one row per FPV channel: OK, Direct (a Wi-Fi signal on it) or IMD */
    const int y_rows = y_axis + SY(13);
    const int row_h = g.n ? (SCR_H - y_rows) / g.n : 0;
    for (int k = 0; k < g.n; k++) {
        int y0 = y_rows + k * row_h;
        /* causes heard: the strongest Direct, else the strongest IMD */
        int best = -1, count = 0;
        bool best_direct = false;
        for (int i = 0; i < w.done; i++) {
            const WifiChanInfo &c = w.chan[i];
            if (!c.frames) continue;
            bool dir = c.direct & (1u << k);
            if (!dir && !c.imd[k]) continue;
            count++;
            if (best < 0 || (dir && !best_direct) ||
                (dir == best_direct && c.rssi_max > w.chan[best].rssi_max)) {
                best = i;
                best_direct = dir;
            }
        }
        d.drawFastHLine(0, y0, SCR_W, grid);
        d.setFont(F_L);
        d.setTextDatum(textdatum_t::middle_left);
        d.setTextColor(rgb(CH_RGB[g.color[k]]));
        d.drawString(g.name[k], SX(4), y0 + row_h / 2);
        d.setFont(F_S);
        d.setTextColor(axis);
        snprintf(b, sizeof(b), "%d", g.mhz[k]);
        d.drawString(b, SX(36), y0 + row_h / 2);

        const int xs = SX(70), xd = SX(132);
        d.setFont(F_M);
        if (best < 0) {
            bool all = w.done == w.nch;
            d.setTextColor(all ? green : dim);
            d.drawString(all ? "OK" : "...", xs, y0 + row_h / 2);
            continue;
        }
        const WifiChanInfo &c = w.chan[best];
        d.setTextColor(rgb(blend(best_direct ? RED : YELLOW, UI_RGB[C_BG], wifi_alpha(c.rssi_max, 0.4f))));
        d.drawString(best_direct ? "Direct" : "IMD", xs, y0 + row_h / 2);
        /* ch, how, dBm, which AP */
        d.setFont(F_S);
        d.setTextDatum(textdatum_t::bottom_left);
        d.setTextColor(text);
        if (best_direct) {
            snprintf(b, sizeof(b), "ch%d %d  %d dBm", c.ch, wifi_mhz(c.ch), c.rssi_max);
        } else {
            int v = c.imd[k] > 0 ? c.imd[k] - 1 : -c.imd[k] - 1;
            if (c.imd[k] > 0) snprintf(b, sizeof(b), "ch%d  2x%s-WiFi  %d dBm", c.ch, g.name[v], c.rssi_max);
            else snprintf(b, sizeof(b), "ch%d  2xWiFi-%s  %d dBm", c.ch, g.name[v], c.rssi_max);
        }
        d.drawString(b, xd, y0 + row_h / 2);
        const WifiApInfo *a = ap_on(w, c.ch);
        d.setTextDatum(textdatum_t::top_left);
        d.setTextColor(axis);
        if (a) snprintf(b, sizeof(b), "%.18s", a->ssid[0] ? a->ssid : "(hidden)");
        else snprintf(b, sizeof(b), "(no beacon heard)");
        if (count > 1) {
            size_t l = strlen(b);
            snprintf(b + l, sizeof(b) - l, "  +%d more", count - 1);
        }
        d.drawString(b, xd, y0 + row_h / 2 + SY(1));
    }
    d.endWrite();
}

}  // namespace fpv
