/* Screen layout and drawing: 320 x 240 landscape (2.8" ILI9341), or 480 x 320
 * with FPV_PANEL_RESTOUCH35 (Waveshare Pico-ResTouch-LCD-3.5, ILI9488) or
 * FPV_PANEL_CAPTOUCH35 (Waveshare 3.5inch Capacitive Touch LCD, ST7796S): the
 * same layout, scaled. Depends only on LovyanGFX sprites, so the same code
 * renders on the PC for previews. */
#pragma once
#include <stdint.h>
#include "analysis.h"
#include "dsp.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#if FPV_PANEL_RESTOUCH35 || FPV_PANEL_CAPTOUCH35
#define FPV_SCREEN_480 1
#else
#define FPV_SCREEN_480 0
#endif

namespace fpv {

#if FPV_SCREEN_480
constexpr int SCR_W = 480, SCR_H = 320;
constexpr int HDR_H = 22, SPEC_H = 140, AXIS_H = 14, WF_H = 64, RO_H = 80;
#else
constexpr int SCR_W = 320, SCR_H = 240;
constexpr int HDR_H = 16, SPEC_H = 104, AXIS_H = 10, WF_H = 48, RO_H = 62;
#endif
constexpr int HDR_Y = 0;
constexpr int SPEC_Y = HDR_Y + HDR_H;
constexpr int AXIS_Y = SPEC_Y + SPEC_H;
constexpr int WF_Y = AXIS_Y + AXIS_H;
constexpr int RO_Y = WF_Y + WF_H;
static_assert(RO_Y + RO_H == SCR_H, "layout must fill the screen");
constexpr int COLS = SCR_W;
constexpr int MAX_CH = 4;

/* Groups of channels (4CH, 4CH2, 4CH3, 3CH); MODE_1CH shows one channel of
 * the current group. MODE_PC: a span and channels set over USB ("FPV SPAN");
 * not stored. The values are stored in NVS: add new modes at the end. */
enum Mode : uint8_t {
    MODE_4CH = 0, MODE_3CH, MODE_1CH, MODE_4CH2, MODE_4CH3, MODE_COUNT, MODE_PC = MODE_COUNT
};
/* The groups, in the order of the mode list. */
constexpr int GROUP_COUNT = 4;
constexpr Mode GROUP_ORDER[GROUP_COUNT] = {MODE_4CH, MODE_4CH2, MODE_4CH3, MODE_3CH};
const char *mode_label(Mode m);

struct UiChannel {
    char name[8];
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
    int snr_db = 18;             /* detection threshold */
    unsigned rx_errors = 0;
    int bat_mv = 0;                /* 0 unknown, < FPV_BAT_ABSENT_MV: USB */
    int clip_permille = 0;         /* saturated ADC samples: "SAT" on the plot */
    Mode back_mode = MODE_4CH;     /* the group (1CH: where a tap on the readout goes back to) */
    const char *message = nullptr;  /* overlay text, e.g. "clear" */
};

/* Waterfall code: dB = -128 + code / 2. */
uint8_t wf_code(float db);

/* Main screen header: [channels / mode: opens the mode list] battery [SET]
 * [HOLD]; the plots take a long press (clear). Readouts: a card zooms into
 * its channel (1CH); in 1CH, < > step the channel and the middle goes back. */
enum Hit : uint8_t {
    HIT_NONE = 0, HIT_MODE, HIT_SETUP, HIT_HOLD, HIT_PLOT, HIT_CH_PREV, HIT_CH_NEXT, HIT_BACK,
    HIT_CARD0,                 /* + card index */
};
Hit hit_test(const UiView &v, int x, int y);

/* Setup screen: big buttons (45 px rows, about 8 mm). */
struct SetupView {
    int gain;                  /* -1 = AGC */
    int snr_db;
    int bat_mv;
    float sweeps_per_s;
    float floor_db;
    int clip_permille;
    unsigned rx_errors;
};
enum SetupHit : uint8_t {
    SH_NONE = 0, SH_DONE, SH_CAL, SH_GAIN_DOWN, SH_GAIN_UP, SH_SNR_DOWN, SH_SNR_UP,
};
SetupHit setup_hit(int x, int y);
/* buttons that repeat while held */
inline bool setup_repeats(SetupHit h) { return h >= SH_GAIN_DOWN; }

/* A group's channels, coloured by frequency (red, green, blue, yellow). */
struct GroupInfo {
    Mode mode;
    int n;
    char name[MAX_CH][8];
    int mhz[MAX_CH];
    int color[MAX_CH];
};
/* Mode list (from the header): one row per group in GROUP_ORDER, each with
 * its own WiFi button. */
struct ModesView {
    GroupInfo row[GROUP_COUNT];
    Mode current;              /* the group shown; MODE_PC: none of them */
};
/* Wi-Fi scan (receive only) of the channels that can reach the FPV bands. */
constexpr int WIFI_SCAN_MAX_CH = 20, WIFI_SCAN_MAX_AP = 24;
struct WifiApInfo {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;               /* strongest beacon, dBm */
    uint8_t chan;              /* primary channel, 0 = unknown */
    uint16_t lo_mhz, hi_mhz;   /* occupied, from the beacon's HT / VHT operation */
    uint16_t beacons;
};
struct WifiChanInfo {
    uint8_t ch;
    uint16_t frames;           /* any frame heard while listening there */
    int8_t rssi_max;
    /* What a Wi-Fi signal on this 20 MHz channel does to the group's channel k:
     * direct bit k = overlaps it; imd[k] = v + 1: 2 x VTX v - Wi-Fi lands on
     * it, -(v + 1): 2 x Wi-Fi - VTX v; 0 = nothing. */
    uint8_t direct;
    int8_t imd[MAX_CH];
};
struct WifiView {
    GroupInfo group;           /* the group checked */
    int nch, done;             /* channels to scan (only those that can hurt it), scanned so far */
    bool running;
    int current;               /* channel index being listened to, -1 none */
    WifiChanInfo chan[WIFI_SCAN_MAX_CH];
    int nap;
    WifiApInfo ap[WIFI_SCAN_MAX_AP];
};

constexpr int PICK_NONE = -1, PICK_CLOSE = -2, PICK_RESCAN = -4;
/* row in GROUP_ORDER (*wifi: its WiFi button), or PICK_CLOSE / PICK_NONE */
int modes_hit(int x, int y, bool *wifi);
int wifi_hit(int x, int y);                               /* PICK_CLOSE, PICK_RESCAN */

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
    /* full-screen pages, drawn straight onto the LCD */
    void draw_setup(const SetupView &s);
    void draw_modes(const ModesView &m);
    void draw_wifi(const WifiView &w);

private:
    void readout_box(const UiView &v, const UiChannel &c, int x, int w);
    void readout_single(const UiView &v, const UiChannel &c);
    int x_of(const UiView &v, float mhz) const;

    lgfx::LovyanGFX *dst_ = nullptr;
    LGFX_Sprite hdr_, spec_, axis_, wf_, ro_;
    size_t bytes_ = 0;
};

}  // namespace fpv
