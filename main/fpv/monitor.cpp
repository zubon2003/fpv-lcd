/* Stand-alone FPV 5.8 GHz band monitor on an ILI9341 + XPT2046 LCD.
 *
 * Two tasks:
 *   receiver (app_main's task): hops the LO over the current span, takes one
 *     80 MS/s snapshot per hop, FFTs it and min-combines it into a sweep;
 *     each finished sweep is published to the UI under a mutex.
 *   ui: polls the touch panel, folds each sweep into the average / peak hold /
 *     waterfall, measures the channels and redraws.
 *
 * Modes: 4CH (E2 E1 F3 F5), 3CH (E1 F1 F4), 1CH (one of E2 E1 F1 F3 F4 F5,
 * centre +-15 MHz).
 */
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "analysis.h"
#include "channels.h"
#include "dsp.h"
#include "fpv_config.h"
#include "sdr_hw.h"
#include "ui.hpp"

namespace fpv {

/* ------------------------------------------------------------------ LCD */
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Touch_XPT2046 touch_;

public:
    LGFX() {
        {
            auto cfg = bus_.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = FPV_LCD_SPI_HZ;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = FPV_PIN_SCLK;
            cfg.pin_mosi = FPV_PIN_MOSI;
            cfg.pin_miso = FPV_PIN_MISO;
            cfg.pin_dc = FPV_PIN_LCD_DC;
            bus_.config(cfg);
            panel_.setBus(&bus_);
        }
        {
            auto cfg = panel_.config();
            cfg.pin_cs = FPV_PIN_LCD_CS;
            cfg.pin_rst = FPV_PIN_LCD_RST;
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.offset_rotation = 0;
            cfg.readable = false;       /* LCD SDO is not wired */
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;      /* the touch controller shares SPI */
            panel_.config(cfg);
        }
        {
            auto cfg = touch_.config();
            cfg.x_min = 300;
            cfg.x_max = 3900;
            cfg.y_min = 200;
            cfg.y_max = 3700;
            cfg.pin_int = -1;
            cfg.bus_shared = true;
            cfg.offset_rotation = 0;
            cfg.spi_host = SPI2_HOST;
            cfg.freq = FPV_TOUCH_SPI_HZ;
            cfg.pin_sclk = FPV_PIN_SCLK;
            cfg.pin_mosi = FPV_PIN_MOSI;
            cfg.pin_miso = FPV_PIN_MISO;
            cfg.pin_cs = FPV_PIN_TOUCH_CS;
            touch_.config(cfg);
            panel_.setTouch(&touch_);
        }
        setPanel(&panel_);
    }
};

static LGFX s_lcd;
static Renderer s_ui;
static bool s_have_psram;

/* ------------------------------------------------------------- settings */
/* 1CH: the triangle buttons step through these, in this order. */
static const char *const SINGLE_CHANNELS[] = {"E2", "E1", "F1", "F3", "F4", "F5"};
constexpr int NUM_SINGLE = sizeof(SINGLE_CHANNELS) / sizeof(SINGLE_CHANNELS[0]);

struct Settings {
    uint8_t mode = MODE_4CH;
    uint8_t single = 3;          /* 1CH: index into SINGLE_CHANNELS (F3) */
    int8_t gain = FPV_DEFAULT_GAIN;
};
static Settings s_set;
static const char *NVS_NS = "fpvmon";

static void settings_load() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    Settings t;
    size_t n = sizeof(t);
    if (nvs_get_blob(h, "set", &t, &n) == ESP_OK && n == sizeof(t) && t.mode < MODE_COUNT &&
        t.single < NUM_SINGLE)
        s_set = t;
    nvs_close(h);
}

static void settings_save() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "set", &s_set, sizeof(s_set));
    nvs_commit(h);
    nvs_close(h);
}

static bool touch_cal_load(uint16_t cal[8]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 8 * sizeof(uint16_t);
    bool ok = nvs_get_blob(h, "tcal", cal, &n) == ESP_OK && n == 8 * sizeof(uint16_t);
    nvs_close(h);
    return ok;
}

static void touch_cal_save(const uint16_t cal[8]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "tcal", cal, 8 * sizeof(uint16_t));
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------- channels */
static int colour_slot(const char *name) {
    /* red, green, blue, yellow, purple, cyan */
    static const char *const ORDER[] = {"E2", "E1", "F3", "F5", "F1", "F4"};
    for (int i = 0; i < 6; i++)
        if (!strcmp(name, ORDER[i])) return i;
    return 5;
}

struct Lineup {
    int n;
    fpv_channel_t ch[MAX_CH];
};

static Lineup lineup_for(const Settings &st) {
    static const char *const M4[] = {"E2", "E1", "F3", "F5"};
    static const char *const M3[] = {"E1", "F1", "F4"};
    Lineup l{};
    if (st.mode == MODE_4CH) {
        l.n = 4;
        for (int i = 0; i < 4; i++) fpv_channel_parse(M4[i], &l.ch[i]);
    } else if (st.mode == MODE_3CH) {
        l.n = 3;
        for (int i = 0; i < 3; i++) fpv_channel_parse(M3[i], &l.ch[i]);
    } else {
        l.n = 1;
        fpv_channel_parse(SINGLE_CHANNELS[st.single], &l.ch[0]);
    }
    return l;
}

/* --------------------------------------------------- receiver <-> UI */
struct Request {
    uint32_t gen;
    sweep_plan_t plan;
    int gain;
};

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_ui_task;
static Request s_req;                       /* UI -> receiver */
static float *s_shared;                     /* receiver -> UI */
static uint32_t s_shared_seq, s_shared_gen;
static int64_t s_shared_t_us;
static unsigned s_rx_errors;

static float *s_work;
static float *s_psd;

static void receiver_loop() {
    sdr_hw_bandwidth(0);                    /* widest analog filter */
    uint32_t gen = 0;
    int gain = -999;
    sweep_plan_t plan{};
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_req.gen != gen) { gen = s_req.gen; plan = s_req.plan; }
        int want_gain = s_req.gain;
        xSemaphoreGive(s_lock);
        if (plan.nhops == 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (want_gain != gain) {
            gain = want_gain;
            int gmax = sdr_hw_gain_max();
            sdr_hw_gain(gain < 0 ? -1 : (gain > gmax && gmax > 0 ? gmax : gain));
        }

        sweep_clear(s_work, plan.nbins);
        bool aborted = false;
        for (int h = 0; h < plan.nhops; h++) {
            if (__atomic_load_n(&s_req.gen, __ATOMIC_RELAXED) != gen) { aborted = true; break; }   /* mode changed */
            const uint32_t *iq = nullptr;
            if (!sdr_hw_tune((unsigned)plan.lo_mhz[h]) || !sdr_hw_capture(FPV_SAMPLES, &iq)) {
                s_rx_errors++;
                continue;
            }
            dsp_hop_psd(iq, FPV_SAMPLES, s_psd);
            sweep_stitch(&plan, h, s_psd, s_work, FPV_USABLE_MHZ, FPV_DC_KHZ);
            vTaskDelay(1);                       /* let the idle task run */
        }
        if (aborted) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_shared, s_work, sizeof(float) * plan.nbins);
        s_shared_gen = gen;
        s_shared_seq++;
        s_shared_t_us = esp_timer_get_time();
        xSemaphoreGive(s_lock);
        xTaskNotifyGive(s_ui_task);
    }
}

/* ------------------------------------------------------------------- UI */
/* Large buffers live on the heap (PSRAM when present): the C5's static
 * sections must end below the modem's capture bank at 0x40820000. */
static UiView *s_viewp;
#define s_view (*s_viewp)
static float *s_avg, *s_hold;
static uint32_t *s_scratch;
static float s_row_lin[COLS];

struct UiState {
    uint32_t gen = 0;
    uint32_t seen_seq = 0;
    int64_t last_t_us = 0;
    float period_s = 0;
    bool first = true;
    bool hold_first = true;
    bool row_open = false;
    int64_t row_t_us = 0;
    float wf_top = NAN;
    float search_mhz = 8;
    an_smooth_t smooth[MAX_CH];
    int64_t message_until_us = 0;
};
static UiState s_st;

static void reset_measurement() {
    s_st.first = true;
    s_st.hold_first = true;
    s_st.row_open = false;
    s_st.row_t_us = 0;
    s_st.last_t_us = 0;            /* the next span sweeps at its own rate */
    s_st.period_s = 0;
    s_st.wf_top = NAN;
    memset(s_st.smooth, 0, sizeof(s_st.smooth));
    s_view.have_data = false;
    s_view.wf_rows = 0;
    s_view.floor_db = NAN;
    for (int i = 0; i < s_view.nch; i++) {
        s_view.ch[i].smoothed = NAN;
        an_meas_clear(&s_view.ch[i].m, (float)s_view.ch[i].nominal_mhz);
    }
}

static void apply_settings() {
    Lineup l = lineup_for(s_set);
    float lo = 1e9f, hi = -1e9f;
    int freqs[MAX_CH];
    s_view.mode = (Mode)s_set.mode;
    s_view.nch = l.n;
    for (int i = 0; i < l.n; i++) {
        UiChannel &c = s_view.ch[i];
        memcpy(c.name, l.ch[i].name, sizeof(c.name));
        c.nominal_mhz = l.ch[i].freq_mhz;
        c.color = colour_slot(c.name);
        freqs[i] = c.nominal_mhz;
        lo = fminf(lo, (float)c.nominal_mhz);
        hi = fmaxf(hi, (float)c.nominal_mhz);
    }
    sweep_plan(&s_view.plan, lo - FPV_MARGIN_MHZ, hi + FPV_MARGIN_MHZ, FPV_STEP_MHZ);
    s_st.search_mhz = an_auto_search(freqs, l.n, 8.0f);
    s_view.gain = s_set.gain;
    reset_measurement();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req.plan = s_view.plan;
    s_req.gain = s_set.gain;
    s_st.gen = ++s_req.gen;
    xSemaphoreGive(s_lock);
}

static void flash_message(const char *m) {
    s_view.message = m;
    s_st.message_until_us = esp_timer_get_time() + 900000;
}

static void on_hit(Hit hit) {
    bool replan = false, regain = false;
    switch (hit) {
    case HIT_MODE:
        s_set.mode = (uint8_t)((s_set.mode + 1) % MODE_COUNT);
        replan = true;
        break;
    case HIT_GAIN_DOWN:
        if (s_set.gain < 0) s_set.gain = 60;
        else if (s_set.gain >= 5) s_set.gain -= 5;
        regain = true;
        break;
    case HIT_GAIN_UP:
        if (s_set.gain >= 0) s_set.gain = s_set.gain + 5 > 60 ? -1 : s_set.gain + 5;  /* past 60 = AGC */
        regain = true;
        break;
    case HIT_HOLD:
        s_view.hold_on = !s_view.hold_on;
        s_st.hold_first = true;
        break;
    case HIT_CH_PREV:
    case HIT_CH_NEXT: {
        int step = hit == HIT_CH_NEXT ? 1 : NUM_SINGLE - 1;
        s_set.single = (uint8_t)((s_set.single + step) % NUM_SINGLE);
        replan = true;
        break;
    }
    default:
        return;
    }
    if (replan) {
        apply_settings();
        s_ui.draw_all(s_view);
    }
    if (regain) {
        s_view.gain = s_set.gain;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_req.gain = s_set.gain;
        xSemaphoreGive(s_lock);
        s_st.first = true;          /* new gain: restart the average */
        s_st.hold_first = true;
    }
    settings_save();
}

static void clear_all() {
    reset_measurement();
    flash_message("cleared");
}

/* Fold the newest sweep into the view. Called with s_lock held. */
static void ingest_locked(int64_t t_us) {
    const int nb = s_view.plan.nbins;
    float dt = s_st.last_t_us ? (float)(t_us - s_st.last_t_us) * 1e-6f : 0.0f;
    s_st.last_t_us = t_us;
    if (dt > 0.0005f && dt < 5.0f)
        s_st.period_s = s_st.period_s > 0 ? s_st.period_s + 0.1f * (dt - s_st.period_s) : dt;

    float tau = FPV_AVG_MS / 2000.0f;
    float a = dt > 0 ? 1.0f - expf(-dt / tau) : 1.0f;
    an_average(s_avg, s_shared, nb, a, s_st.first);
    s_st.first = false;
    if (s_view.hold_on) {
        an_peak_hold(s_hold, s_shared, nb, s_st.hold_first);
        s_st.hold_first = false;
    }
    an_columns_db(s_shared, nb, COLS, s_view.col_raw);
    an_columns_lin(s_shared, nb, COLS, s_view.col_avg);   /* scratch: raw lin */

    /* waterfall: one row per FPV_WF_ROW_MS holding the peak of its sweeps */
    if (!s_st.row_open) {
        memcpy(s_row_lin, s_view.col_avg, sizeof(s_row_lin));
        s_st.row_open = true;
        if (!s_st.row_t_us) s_st.row_t_us = t_us;
    } else {
        for (int x = 0; x < COLS; x++)
            if (dsp_valid(s_view.col_avg[x]) && (!dsp_valid(s_row_lin[x]) || s_view.col_avg[x] > s_row_lin[x]))
                s_row_lin[x] = s_view.col_avg[x];
    }
    if (t_us - s_st.row_t_us >= (int64_t)FPV_WF_ROW_MS * 1000) {
        s_view.wf_head = (s_view.wf_head + 1) % WF_H;
        for (int x = 0; x < COLS; x++)
            s_view.wf[s_view.wf_head][x] = wf_code(dsp_valid(s_row_lin[x]) ? dsp_db(s_row_lin[x]) : -128.0f);
        if (s_view.wf_rows < WF_H) s_view.wf_rows++;
        s_st.row_t_us = t_us;
        s_st.row_open = false;
    }
    an_columns_db(s_avg, nb, COLS, s_view.col_avg);
    if (s_view.hold_on) an_columns_db(s_hold, nb, COLS, s_view.col_hold);
}

static void measure_and_level() {
    an_params_t prm;
    an_default_params(&prm);
    prm.search_mhz = s_st.search_mhz;
    float floor_db = an_floor_db(s_avg, s_view.plan.nbins, s_scratch);
    s_view.floor_db = floor_db;
    float top = floor_db;
    for (int i = 0; i < s_view.nch; i++) {
        UiChannel &c = s_view.ch[i];
        an_measure(s_avg, &s_view.plan, (float)c.nominal_mhz, &prm, floor_db, &c.m);
        c.smoothed = an_smooth(&s_st.smooth[i], &c.m, FPV_SMOOTH);
        if (c.m.peak_db == c.m.peak_db && c.m.peak_db > top) top = c.m.peak_db;
    }
    if (floor_db == floor_db) {
        /* spectrum: floor - 8 .. max(top + 8, floor + 30), eased */
        float lo = floor_db - 8.0f, hi = fmaxf(top + 8.0f, floor_db + 30.0f);
        if (!s_view.have_data) { s_view.y_lo = lo; s_view.y_hi = hi; }
        else { s_view.y_lo += 0.3f * (lo - s_view.y_lo); s_view.y_hi += 0.3f * (hi - s_view.y_hi); }
        /* waterfall: floor - 3 .. max(loudest + 3, floor + 25), loudest eased */
        float loud = -1e9f;
        for (int x = 0; x < COLS; x++)
            if (s_view.col_raw[x] == s_view.col_raw[x] && s_view.col_raw[x] > loud) loud = s_view.col_raw[x];
        if (loud > -1e8f) s_st.wf_top = s_st.wf_top == s_st.wf_top ? s_st.wf_top + 0.2f * (loud - s_st.wf_top) : loud;
        s_view.wf_lo = floor_db - 3.0f;
        s_view.wf_hi = fmaxf(s_st.wf_top + 3.0f, floor_db + 25.0f);
    }
    s_view.sweeps_per_s = s_st.period_s > 0 ? 1.0f / s_st.period_s : 0.0f;
    s_view.rx_errors = s_rx_errors;
    s_view.have_data = true;
}

static void calibrate_touch() {
    uint16_t cal[8];
    s_lcd.fillScreen(TFT_BLACK);
    s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    s_lcd.setFont(&fonts::Font2);
    s_lcd.setTextDatum(textdatum_t::middle_center);
    s_lcd.drawString("Touch the arrow tips", SCR_W / 2, SCR_H / 2 - 10);
    s_lcd.drawString("(touch calibration)", SCR_W / 2, SCR_H / 2 + 10);
    s_lcd.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 15);
    touch_cal_save(cal);
}

static void ui_task(void *) {
    s_lcd.init();
    s_lcd.setRotation(FPV_LCD_ROTATION);
    s_lcd.fillScreen(TFT_BLACK);

    /* Splash; holding the screen during it recalibrates the touch. */
    s_lcd.setFont(&fonts::Font2);
    s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    s_lcd.setTextDatum(textdatum_t::middle_center);
    s_lcd.drawString("FPV band monitor  ESP32-C5", SCR_W / 2, SCR_H / 2 - 12);
    s_lcd.setTextColor(0x8C71u, TFT_BLACK);
    s_lcd.drawString("hold the screen to calibrate touch", SCR_W / 2, SCR_H / 2 + 12);
    uint16_t cal[8];
    bool have_cal = touch_cal_load(cal);
    if (have_cal) s_lcd.setTouchCalibrate(cal);
    bool held = true;
    for (int i = 0; i < 30; i++) {
        int32_t x, y;
        if (!s_lcd.getTouch(&x, &y)) held = false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (held) {
        calibrate_touch();
    } else if (!have_cal) {
        /* First boot: calibrate, but do not hang on a board whose touch is
         * not wired - start with the built-in defaults after 10 s. */
        s_lcd.fillScreen(TFT_BLACK);
        s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        s_lcd.drawString("Touch the screen to calibrate", SCR_W / 2, SCR_H / 2 - 10);
        s_lcd.setTextColor(0x8C71u, TFT_BLACK);
        s_lcd.drawString("(skipped in 10 s)", SCR_W / 2, SCR_H / 2 + 12);
        bool touched = false;
        for (int i = 0; i < 200 && !touched; i++) {
            int32_t x, y;
            touched = s_lcd.getTouch(&x, &y);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (touched) {
            int32_t rx, ry;
            while (s_lcd.getTouch(&rx, &ry)) vTaskDelay(pdMS_TO_TICKS(20));  /* wait for release */
            calibrate_touch();
        }
    }

    if (!s_ui.begin(&s_lcd, s_have_psram)) {
        s_lcd.fillScreen(TFT_BLACK);
        s_lcd.setTextColor(TFT_RED, TFT_BLACK);
        char b[64];
        snprintf(b, sizeof(b), "out of memory (free %u / %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        s_lcd.drawString(b, SCR_W / 2, SCR_H / 2);
        for (;;) vTaskDelay(portMAX_DELAY);
    }
    s_lcd.fillScreen(TFT_BLACK);
    apply_settings();
    s_ui.draw_all(s_view);

    bool touching = false, long_done = false;
    int64_t touch_t0 = 0;
    Hit touch_hit = HIT_NONE;
    for (;;) {
        bool fresh = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15)) > 0;

        int32_t tx, ty;
        bool t = s_lcd.getTouch(&tx, &ty);
        int64_t now = esp_timer_get_time();
        if (t && !touching) {
            touching = true;
            long_done = false;
            touch_t0 = now;
            touch_hit = hit_test(s_view, (int)tx, (int)ty);
            if (touch_hit != HIT_PLOT) on_hit(touch_hit);
            s_ui.draw_frame(s_view);
        } else if (t && touching && touch_hit == HIT_PLOT && !long_done && now - touch_t0 > 800000) {
            long_done = true;                         /* long press on the plot */
            clear_all();
            s_ui.draw_frame(s_view);
        } else if (!t && touching) {
            touching = false;
        }

        if (s_view.message && now > s_st.message_until_us) {
            s_view.message = nullptr;
            fresh = true;
        }
        if (!fresh) continue;

        bool got = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_shared_gen == s_st.gen && s_shared_seq != s_st.seen_seq) {
            s_st.seen_seq = s_shared_seq;
            ingest_locked(s_shared_t_us);
            got = true;
        }
        xSemaphoreGive(s_lock);
        if (got) measure_and_level();
        s_ui.draw_frame(s_view);
    }
}

}  // namespace fpv

namespace fpv {

static void *big_alloc(size_t bytes, bool internal = false) {
    void *p = nullptr;
    if (!internal && s_have_psram) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

static void halt(const char *what) {
    /* Before the LCD is up: report on the USB console and stop. */
    printf("fpv monitor: %s (internal free %u, psram free %u)\n", what,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    for (;;) vTaskDelay(portMAX_DELAY);
}

static void alloc_buffers() {
    s_have_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    /* hot paths (FFT, per-hop spectrum) stay in internal SRAM */
    if (!dsp_init()) halt("no memory for the FFT");
    s_psd = (float *)big_alloc(sizeof(float) * DSP_NFFT, true);
    s_work = (float *)big_alloc(sizeof(float) * SWEEP_MAX_BINS, true);
    s_shared = (float *)big_alloc(sizeof(float) * SWEEP_MAX_BINS);
    s_avg = (float *)big_alloc(sizeof(float) * SWEEP_MAX_BINS);
    s_hold = (float *)big_alloc(sizeof(float) * SWEEP_MAX_BINS);
    s_scratch = (uint32_t *)big_alloc(sizeof(uint32_t) * SWEEP_MAX_BINS);
    void *v = big_alloc(sizeof(UiView));
    if (!s_psd || !s_work || !s_shared || !s_avg || !s_hold || !s_scratch || !v)
        halt("out of memory");
    s_viewp = new (v) UiView();
}
}  // namespace fpv

extern "C" void fpv_monitor_main(void) {
    using namespace fpv;
    alloc_buffers();
    s_lock = xSemaphoreCreateMutex();
    settings_load();
    s_req.gain = s_set.gain;
    xTaskCreate(ui_task, "fpv_ui", 8192, nullptr, 5, &s_ui_task);
    receiver_loop();
}
