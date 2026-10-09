/* Screen layout and drawing (320 x 240 landscape). Depends only on
 * LovyanGFX sprites, so the same code renders on the PC for previews. */
#pragma once
#include <stdint.h>
#include "analysis.h"
#include "dsp.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace fpv {

constexpr int SCR_W = 320, SCR_H = 240;
constexpr int HDR_Y = 0, HDR_H = 16;
constexpr int SPEC_Y = 16, SPEC_H = 104;
constexpr int AXIS_Y = 120, AXIS_H = 10;
constexpr int WF_Y = 130, WF_H = 48;
constexpr int RO_Y = 178, RO_H = 62;
constexpr int COLS = SCR_W;
constexpr int MAX_CH = 4;

enum Mode : uint8_t { MODE_4CH = 0, MODE_3CH, MODE_1CH, MODE_COUNT };
const char *mode_label(Mode m);

struct UiChannel {
    char name[4];
    int nominal_mhz;
    int color;              /* colour slot 0..5 */
    an_meas_t m;
    float smoothed;         /* NAN until first seen */
};

struct UiView {
    Mode mode = MODE_4CH;
    sweep_plan_t plan{};
    int nch = 0;
    UiChannel ch[MAX_CH]{};
    float col_raw[COLS], col_avg[COLS], col_hold[COLS];   /* dB, NAN = none */
    bool hold_on = false;
    bool have_data = false;
    float y_lo = -100, y_hi = -40;
    uint8_t wf[WF_H][COLS]{};    /* half-dB codes, newest row at wf_head */
    int wf_head = 0, wf_rows = 0;
    float wf_lo = -100, wf_hi = -60;
    float floor_db = __builtin_nanf("");
    float sweeps_per_s = 0;
    int gain = 40;               /* -1 = AGC */
    unsigned rx_errors = 0;
    int bat_mv = 0;                /* 0 unknown, < FPV_BAT_ABSENT_MV: USB */
    const char *message = nullptr;  /* overlay text, e.g. "clear" */
};

/* Waterfall code: dB = -128 + code / 2. */
uint8_t wf_code(float db);

enum Hit : uint8_t {
    HIT_NONE = 0, HIT_MODE, HIT_GAIN_DOWN, HIT_GAIN_UP, HIT_HOLD,
    HIT_PLOT, HIT_CH_PREV, HIT_CH_NEXT,
};
Hit hit_test(const UiView &v, int x, int y);

class Renderer {
public:
    bool begin(lgfx::LovyanGFX *dst, bool psram = false);  /* allocates the sprites */
    void draw_all(const UiView &v);        /* full redraw */
    void draw_frame(const UiView &v);      /* everything that changes */
    size_t sprite_bytes() const { return bytes_; }
    /* single regions, for partial updates */
    void draw_header(const UiView &v);
    void draw_spectrum(const UiView &v);
    void draw_axis(const UiView &v);
    void draw_waterfall(const UiView &v);
    void draw_readouts(const UiView &v);

private:
    void readout_box(const UiView &v, const UiChannel &c, int x, int w);
    void readout_single(const UiView &v, const UiChannel &c);
    int x_of(const UiView &v, float mhz) const;

    lgfx::LovyanGFX *dst_ = nullptr;
    LGFX_Sprite hdr_, spec_, axis_, wf_, ro_;
    size_t bytes_ = 0;
};

}  // namespace fpv
