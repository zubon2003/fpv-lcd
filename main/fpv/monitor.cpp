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
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_rom_crc.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "nvs.h"

#include "analysis.h"
#include "battery.h"
#include "link.h"
extern "C" {
#include "burst_serial.h"
}
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
/* 1CH: the triangle buttons (and "FPV CH") step through these, in this order. */
static const char *const SINGLE_CHANNELS[] = {"E2", "E1", "F1", "F3", "F4", "F5"};
constexpr int NUM_SINGLE = sizeof(SINGLE_CHANNELS) / sizeof(SINGLE_CHANNELS[0]);

struct Settings {
    uint8_t mode = MODE_4CH;
    uint8_t single = 3;          /* 1CH: index into SINGLE_CHANNELS (F3) */
    int8_t gain = FPV_DEFAULT_GAIN;
    uint8_t snr = FPV_DEFAULT_SNR_DB;   /* detection threshold */
};
static Settings s_set;
static const char *NVS_NS = "fpvmon";

static void settings_load() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    Settings t;
    size_t n = sizeof(t);
    if (nvs_get_blob(h, "set", &t, &n) == ESP_OK && n == sizeof(t) && t.mode < MODE_COUNT &&
        t.single < NUM_SINGLE && t.snr >= FPV_SNR_MIN_DB && t.snr <= FPV_SNR_MAX_DB)
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
static volatile int s_bat_mv;               /* latest battery reading */
static volatile bool s_shutdown;            /* receiver -> UI: battery low */

/* ---------------------------------------------------------------- power */
/* Set while sleeping because of a low battery: the next timer wake then
 * waits for FPV_BAT_RESUME_MV (charged) instead of FPV_BAT_BOOT_MV. */
static RTC_DATA_ATTR bool s_low_sleep;

static const gpio_num_t HELD_PINS[] = {
    (gpio_num_t)FPV_PIN_LCD_CS, (gpio_num_t)FPV_PIN_TOUCH_CS, (gpio_num_t)FPV_PIN_LCD_RST,
#if FPV_PIN_BACKLIGHT >= 0
    (gpio_num_t)FPV_PIN_BACKLIGHT,
#endif
};

static void backlight(bool on) {
#if FPV_PIN_BACKLIGHT >= 0
    gpio_hold_dis((gpio_num_t)FPV_PIN_BACKLIGHT);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << FPV_PIN_BACKLIGHT;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)FPV_PIN_BACKLIGHT, on ? FPV_BACKLIGHT_ON : !FPV_BACKLIGHT_ON);
#else
    (void)on;
#endif
}

/* Deep sleep until the next battery check. The LCD must already be asleep;
 * the backlight and both SPI chip selects are held off while sleeping. */
[[noreturn]] static void power_off() {
    backlight(false);
    /* chip selects and the LCD reset high, so nothing wakes or resets the
     * panels while the pins would otherwise float */
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << FPV_PIN_LCD_CS) | (1ULL << FPV_PIN_TOUCH_CS) | (1ULL << FPV_PIN_LCD_RST);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)FPV_PIN_LCD_CS, 1);
    gpio_set_level((gpio_num_t)FPV_PIN_TOUCH_CS, 1);
    gpio_set_level((gpio_num_t)FPV_PIN_LCD_RST, 1);
    for (gpio_num_t p : HELD_PINS) gpio_hold_en(p);
    bat_park();
    s_low_sleep = true;
    esp_sleep_enable_timer_wakeup((uint64_t)FPV_BAT_WAKE_S * 1000000ULL);
    esp_deep_sleep_start();
}

static bool battery_low(int mv, int limit) {
    return FPV_BAT_CUTOFF_MV > 0 && mv >= FPV_BAT_ABSENT_MV && mv < limit;
}

static float *s_work;
static float *s_psd;

/* ------------------------------------------------------------- USB link */

static QueueHandle_t s_pc_q;                /* receiver -> UI: settings from the PC */
static bool s_stream;                       /* SWEEP frames on */
static uint32_t s_stream_seq;
static uint16_t *s_codes;                   /* frame payload */
static volatile bool s_raw;                 /* PC drives the radio (ESP-SDR protocol) */
static const char *mode_name(int m) { return m == MODE_4CH ? "4CH" : m == MODE_3CH ? "3CH" : m == MODE_1CH ? "1CH" : "PC"; }
static volatile int s_shown_mode;           /* what the LCD runs, for STATE? */

static void link_reply(const char *s) { burst_serial_send(s, strlen(s)); }

/* Everything or nothing within the deadline; a host that stopped reading
 * turns streaming off instead of stalling the receiver. */
static bool link_send(const void *data, size_t n, int64_t deadline) {
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        size_t done = burst_serial_try_send(p, n);
        p += done;
        n -= done;
        if (n && esp_timer_get_time() > deadline) return false;
    }
    return true;
}

static void send_sweep(const sweep_plan_t &plan, const float *sweep, int64_t t_us) {
    fpv_encode(sweep, plan.nbins, s_codes);
    size_t bytes = sizeof(uint16_t) * plan.nbins;
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)s_codes, bytes);
    char h[96];
    int n = snprintf(h, sizeof(h), "SWEEP %u %d %d %d %u %08x\n", (unsigned)++s_stream_seq, plan.start_mhz,
                     plan.nbins, DSP_BIN_HZ, (unsigned)(t_us / 1000), (unsigned)crc);
    int64_t deadline = esp_timer_get_time() + 300000;
    if (!link_send(h, (size_t)n, deadline) || !link_send(s_codes, bytes, deadline)) s_stream = false;
}

static void fpv_command(const char *line, const sweep_plan_t &plan, int gain) {
    fpv_cmd_t c;
    char r[200];
    switch (fpv_parse(line, SINGLE_CHANNELS, NUM_SINGLE, &c)) {
    case FC_HELLO:
        snprintf(r, sizeof(r), "FPV %d bin_hz=%d maxbins=%d step=%d usable=%.0f dc_khz=%.0f samples=%d\n",
                 FPV_LINK_VERSION, DSP_BIN_HZ, SWEEP_MAX_BINS, FPV_STEP_MHZ, (double)FPV_USABLE_MHZ,
                 (double)FPV_DC_KHZ, FPV_SAMPLES);
        link_reply(r);
        return;
    case FC_STATE:
        snprintf(r, sizeof(r), "STATE mode=%s ch=%s gain=%d snr=%d start=%d nbins=%d hops=%d stream=%d bat_mv=%d\n",
                 mode_name(s_shown_mode), SINGLE_CHANNELS[s_set.single], gain, s_set.snr, plan.start_mhz,
                 plan.nbins, plan.nhops, s_stream ? 1 : 0, (int)s_bat_mv);
        link_reply(r);
        return;
    case FC_STREAM:
        s_stream = c.ival != 0;
        link_reply("OK\n");
        return;
    case FC_BAD:
        snprintf(r, sizeof(r), "ERR %s\n", c.err);
        link_reply(r);
        return;
    default:                                 /* settings: applied by the UI task */
        if (xQueueSend(s_pc_q, &c, 0) == pdTRUE) link_reply("OK\n");
        else link_reply("ERR busy\n");
        return;
    }
}

/* Serve the USB port. Returns true while the PC holds the radio with the
 * ESP-SDR protocol (5 s lease after its last line, or until RELEASE). */
static bool link_service(const sweep_plan_t &plan, int gain) {
    static int64_t raw_until;
    char line[160];
    int st;
    while ((st = burst_serial_poll_line(line, sizeof(line))) != 0) {
        if (st < 0) { link_reply("ERR command_length\n"); continue; }
        const char *t = line;
        while (*t == ' ' || *t == '\r') t++;
        if (!*t) continue;                   /* blank: hosts send one to resync */
        fpv_cmd_t probe;
        if (fpv_parse(line, SINGLE_CHANNELS, NUM_SINGLE, &probe) != FC_NONE) {
            fpv_command(line, plan, gain);
            continue;
        }
        sdr_hw_command(line);                /* stock protocol: the PC's own sweep */
        raw_until = !strcmp(line, "RELEASE") ? 0 : esp_timer_get_time() + 5000000;
    }
    bool raw = raw_until && esp_timer_get_time() < raw_until;
    if (!raw) raw_until = 0;
    s_raw = raw;
    return raw;
}

static void receiver_loop() {
    sdr_hw_bandwidth(0);                    /* widest analog filter */
    uint32_t gen = 0;
    int gain = -999;
    bool was_raw = false;
    sweep_plan_t plan{};
    for (;;) {
        if (link_service(plan, gain)) {        /* the PC has the radio */
            was_raw = true;
            vTaskDelay(1);
            continue;
        }
        if (was_raw) {                         /* take it back: our filter and gain */
            was_raw = false;
            sdr_hw_bandwidth(0);
            gain = -999;
        }
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

        /* battery, between sweeps (the ADC never runs during a capture) */
        static int64_t t_bat;
        static int low_reads;
        int64_t now = esp_timer_get_time();
        if (now - t_bat >= (int64_t)FPV_BAT_CHECK_MS * 1000) {
            t_bat = now;
            int mv = bat_read_mv();
            s_bat_mv = mv;
            low_reads = battery_low(mv, FPV_BAT_CUTOFF_MV) ? low_reads + 1 : 0;
            if (low_reads >= FPV_BAT_LOW_READS) {
                s_shutdown = true;                   /* the UI powers off */
                for (;;) vTaskDelay(portMAX_DELAY);
            }
        }

        sweep_clear(s_work, plan.nbins);
        bool aborted = false;
        for (int h = 0; h < plan.nhops; h++) {
            if (__atomic_load_n(&s_req.gen, __ATOMIC_RELAXED) != gen) { aborted = true; break; }   /* mode changed */
            if (link_service(plan, gain)) { aborted = true; break; }                            /* PC took the radio */
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

        int64_t t_done = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_shared, s_work, sizeof(float) * plan.nbins);
        s_shared_gen = gen;
        s_shared_seq++;
        s_shared_t_us = t_done;
        xSemaphoreGive(s_lock);
        xTaskNotifyGive(s_ui_task);
        if (s_stream) send_sweep(plan, s_work, t_done);
    }
}

/* ------------------------------------------------------------------- UI */
/* Large buffers live on the heap (PSRAM when present): the C5's static
 * sections must end below the modem's capture bank at 0x40820000. */
static UiView *s_viewp;
#define s_view (*s_viewp)
static float *s_avg, *s_hold;
static float *s_ring;          /* FPV_AVG_SWEEPS stored sweeps, SWEEP_MAX_BINS apart */
static an_ring_t s_avg_ring;   /* their exact running mean */
static uint32_t *s_scratch;
static float s_row_lin[COLS];

struct UiState {
    uint32_t gen = 0;
    uint32_t seen_seq = 0;
    int64_t last_t_us = 0;
    float period_s = 0;
    bool hold_first = true;
    bool row_open = false;
    int64_t row_t_us = 0;
    float wf_top = NAN;
    float search_mhz = 8;
    an_smooth_t smooth[MAX_CH];
    int64_t message_until_us = 0;
    bool wf_dirty = false;        /* waterfall gained a row since drawn */
};
static UiState s_st;

static void reset_measurement() {
    an_ring_reset(&s_avg_ring, s_view.plan.nbins);
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

/* "FPV SPAN" from the PC: overrides the mode until a mode is chosen here. */
struct PcSpan {
    bool active;
    float start, stop;
    int n;
    fpv_channel_t ch[MAX_CH];
};
static PcSpan s_pc;

static void apply_settings() {
    Lineup l = lineup_for(s_set);
    if (s_pc.active) {
        l.n = s_pc.n;
        for (int i = 0; i < l.n; i++) l.ch[i] = s_pc.ch[i];
    }
    float lo = 1e9f, hi = -1e9f;
    int freqs[MAX_CH];
    s_view.mode = s_pc.active ? MODE_PC : (Mode)s_set.mode;
    s_shown_mode = s_view.mode;
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
    if (s_pc.active) sweep_plan(&s_view.plan, s_pc.start, s_pc.stop, FPV_STEP_MHZ);
    else sweep_plan(&s_view.plan, lo - FPV_MARGIN_MHZ, hi + FPV_MARGIN_MHZ, FPV_STEP_MHZ);
    s_st.search_mhz = an_auto_search(freqs, l.n, 8.0f);
    s_view.gain = s_set.gain;
    s_view.snr_db = s_set.snr;
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

static void set_gain_now(int gain) {
    s_view.gain = gain;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req.gain = gain;
    xSemaphoreGive(s_lock);
    an_ring_reset(&s_avg_ring, s_view.plan.nbins);   /* new gain: restart the average */
    s_st.hold_first = true;
}

/* Main screen taps (the setup screen is handled in ui_task). */
static void on_hit(Hit hit) {
    switch (hit) {
    case HIT_MODE:                       /* 4CH -> 3CH -> 1CH -> 4CH; PC -> back to ours */
        if (s_pc.active) s_pc.active = false;
        else s_set.mode = (uint8_t)((s_set.mode + 1) % MODE_COUNT);
        apply_settings();
        s_ui.draw_all(s_view);
        settings_save();
        return;
    case HIT_HOLD:
        s_view.hold_on = !s_view.hold_on;
        s_st.hold_first = true;
        return;
    case HIT_CH_PREV:
    case HIT_CH_NEXT: {
        int step = hit == HIT_CH_NEXT ? 1 : NUM_SINGLE - 1;
        s_set.single = (uint8_t)((s_set.single + step) % NUM_SINGLE);
        apply_settings();
        s_ui.draw_all(s_view);
        settings_save();
        return;
    }
    default:
        return;
    }
}

static void calibrate_touch();

/* ---------------------------------------------------------- setup screen */
static Settings s_edit;          /* what the setup screen shows / edits */
static bool s_edit_mode_set;     /* a mode button was pressed (leaves PC mode) */

static SetupView setup_view() {
    SetupView sv;
    fpv_channel_t c;
    fpv_channel_parse(SINGLE_CHANNELS[s_edit.single], &c);
    sv.mode = (s_pc.active && !s_edit_mode_set) ? MODE_PC : (Mode)s_edit.mode;
    sv.single_name = SINGLE_CHANNELS[s_edit.single];
    sv.single_mhz = c.freq_mhz;
    sv.gain = s_edit.gain;
    sv.snr_db = s_edit.snr;
    sv.bat_mv = s_bat_mv;
    return sv;
}

/* Returns true when the setup screen closes. */
static bool on_setup_hit(SetupHit h) {
    switch (h) {
    case SH_MODE_4: s_edit.mode = MODE_4CH; s_edit_mode_set = true; break;
    case SH_MODE_3: s_edit.mode = MODE_3CH; s_edit_mode_set = true; break;
    case SH_MODE_1: s_edit.mode = MODE_1CH; s_edit_mode_set = true; break;
    case SH_CH_PREV: s_edit.single = (uint8_t)((s_edit.single + NUM_SINGLE - 1) % NUM_SINGLE); break;
    case SH_CH_NEXT: s_edit.single = (uint8_t)((s_edit.single + 1) % NUM_SINGLE); break;
    case SH_GAIN_DOWN:          /* AGC -> 60 -> ... -> 0 */
        s_edit.gain = s_edit.gain < 0 ? FPV_GAIN_MAX : (s_edit.gain > 0 ? s_edit.gain - 1 : 0);
        break;
    case SH_GAIN_UP:            /* ... -> 60 -> AGC */
        if (s_edit.gain >= 0) s_edit.gain = s_edit.gain >= FPV_GAIN_MAX ? -1 : s_edit.gain + 1;
        break;
    case SH_SNR_DOWN: if (s_edit.snr > FPV_SNR_MIN_DB) s_edit.snr--; break;
    case SH_SNR_UP: if (s_edit.snr < FPV_SNR_MAX_DB) s_edit.snr++; break;
    case SH_CAL: calibrate_touch(); break;
    case SH_DONE: {
        bool replan = s_edit.mode != s_set.mode ||
                      (s_edit.mode == MODE_1CH && s_edit.single != s_set.single) ||
                      (s_pc.active && s_edit_mode_set);
        if (s_edit_mode_set) s_pc.active = false;
        bool regain = s_edit.gain != s_set.gain;
        s_set = s_edit;
        settings_save();
        s_view.snr_db = s_set.snr;
        if (replan) apply_settings();          /* also sends the gain */
        else if (regain) set_gain_now(s_set.gain);
        return true;
    }
    default: break;
    }
    s_ui.draw_setup(setup_view());
    return false;
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

    /* store this sweep in the ring and average the ones held */
    an_ring_push(&s_avg_ring, s_shared, s_avg);
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
        s_st.wf_dirty = true;
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
    prm.min_snr_db = (float)s_set.snr;
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
    s_view.bat_mv = s_bat_mv;
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

/* Settings sent over USB ("FPV MODE/CH/GAIN/SNR/SPAN"). True if any. */
static bool apply_pc_commands() {
    fpv_cmd_t c;
    bool any = false;
    while (xQueueReceive(s_pc_q, &c, 0) == pdTRUE) {
        any = true;
        switch (c.kind) {
        case FC_MODE:
            s_pc.active = false;
            s_set.mode = (uint8_t)c.ival;
            apply_settings();
            break;
        case FC_CH:
            s_set.single = (uint8_t)c.ival;
            if (s_set.mode == MODE_1CH && !s_pc.active) apply_settings();
            break;
        case FC_GAIN:
            s_set.gain = (int8_t)c.ival;
            set_gain_now(s_set.gain);
            break;
        case FC_SNR:
            s_set.snr = (uint8_t)c.ival;
            s_view.snr_db = s_set.snr;
            break;
        case FC_SPAN:
            s_pc.active = true;
            s_pc.start = c.start;
            s_pc.stop = c.stop;
            s_pc.n = c.nch;
            for (int i = 0; i < c.nch; i++) s_pc.ch[i] = c.ch[i];
            apply_settings();
            break;
        default:
            break;
        }
        if (c.kind != FC_SPAN) settings_save();
    }
    return any;
}

static void ui_task(void *) {
    s_lcd.init();
    backlight(true);
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
    int64_t touch_t0 = 0, t_repeat = 0;
    Hit touch_hit = HIT_NONE;
    bool in_setup = false;                /* the setup screen is up */
    bool raw_shown = false;
    SetupHit setup_touch = SH_NONE;
    /* Partial updates: the SPI transfer is most of the drawing cost, and the
     * CPU waits for it. Spectrum at most 10/s, waterfall when it gains a row,
     * readouts 4/s, header once a second or when a setting changes. */
    int64_t t_spec = 0, t_ro = 0, t_hdr = 0;
    bool spec_pending = false;          /* a sweep not yet on the screen */
    auto draw_all_now = [&](int64_t now) {
        s_ui.draw_frame(s_view);
        t_spec = t_ro = t_hdr = now;
        s_st.wf_dirty = false;
    };
    for (;;) {
        bool fresh = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15)) > 0;
        if (s_shutdown) {
            char b[48];
            int mv = s_bat_mv;
            s_lcd.fillScreen(TFT_BLACK);
            s_lcd.setFont(&fonts::Font4);
            s_lcd.setTextDatum(textdatum_t::middle_center);
            s_lcd.setTextColor(TFT_RED, TFT_BLACK);
            snprintf(b, sizeof(b), "Battery low  %d.%02d V", mv / 1000, (mv % 1000) / 10);
            s_lcd.drawString(b, SCR_W / 2, SCR_H / 2 - 16);
            s_lcd.setFont(&fonts::Font2);
            s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
            s_lcd.drawString("shutting down - charge to restart", SCR_W / 2, SCR_H / 2 + 16);
            vTaskDelay(pdMS_TO_TICKS(3000));
            s_lcd.sleep();
            power_off();
        }

        int64_t now0 = esp_timer_get_time();
        if (apply_pc_commands()) {               /* settings from the PC */
            if (in_setup) { s_edit = s_set; s_ui.draw_setup(setup_view()); }
            else { s_ui.draw_all(s_view); t_spec = t_ro = t_hdr = now0; }
        }
        /* the PC drives the radio with raw I/Q: say so on the plot */
        if (s_raw != raw_shown) {
            raw_shown = s_raw;
            s_view.message = raw_shown ? "PC is using the receiver (I/Q)" : nullptr;
            s_st.message_until_us = raw_shown ? INT64_MAX : 0;
            if (!in_setup) s_ui.draw_spectrum(s_view);
        }

        int32_t tx, ty;
        bool t = s_lcd.getTouch(&tx, &ty);
        int64_t now = esp_timer_get_time();
        if (in_setup) {
            /* setup screen: act on press, repeat steppers while held */
            bool close = false;
            if (t && !touching) {
                touching = true;
                touch_t0 = t_repeat = now;
                setup_touch = setup_hit((int)tx, (int)ty);
                close = on_setup_hit(setup_touch);
            } else if (t && touching && setup_repeats(setup_touch) && now - touch_t0 > 500000 &&
                       now - t_repeat > 120000) {
                t_repeat = now;
                close = on_setup_hit(setup_touch);
            } else if (!t && touching) {
                touching = false;
            }
            if (close) {
                in_setup = false;
                s_lcd.fillScreen(TFT_BLACK);
                s_ui.draw_axis(s_view);
                draw_all_now(now);
            }
        } else if (t && !touching) {
            touching = true;
            long_done = false;
            touch_t0 = now;
            touch_hit = hit_test(s_view, (int)tx, (int)ty);
            if (touch_hit == HIT_SETUP) {
                in_setup = true;
                s_edit = s_set;
                s_edit_mode_set = false;
                s_ui.draw_setup(setup_view());
            } else {
                if (touch_hit != HIT_PLOT) on_hit(touch_hit);
                draw_all_now(now);
            }
        } else if (t && touching && touch_hit == HIT_PLOT && !long_done && now - touch_t0 > 800000) {
            long_done = true;                         /* long press on the plot */
            clear_all();
            draw_all_now(now);
        } else if (!t && touching) {
            touching = false;
        }

        bool spec_due = false;
        if (s_view.message && now > s_st.message_until_us) {
            s_view.message = nullptr;
            spec_due = true;                          /* take the overlay off */
        }
        if (fresh) {
            bool got = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_shared_gen == s_st.gen && s_shared_seq != s_st.seen_seq) {
                s_st.seen_seq = s_shared_seq;
                ingest_locked(s_shared_t_us);
                got = true;
            }
            xSemaphoreGive(s_lock);
            if (got) {
                measure_and_level();
                spec_pending = true;
            }
        }
        if (spec_pending && now - t_spec >= 100000) spec_due = true;
        if (in_setup) continue;               /* keep measuring, draw nothing */
        if (spec_due) { s_ui.draw_spectrum(s_view); t_spec = now; spec_pending = false; }
        if (s_st.wf_dirty) { s_ui.draw_waterfall(s_view); s_st.wf_dirty = false; }
        if (s_view.have_data && now - t_ro >= 250000) { s_ui.draw_readouts(s_view); t_ro = now; }
        if (now - t_hdr >= 1000000) { s_ui.draw_header(s_view); t_hdr = now; }
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
    s_ring = (float *)big_alloc(sizeof(float) * SWEEP_MAX_BINS * FPV_AVG_SWEEPS);   /* 92 KB */
    s_codes = (uint16_t *)big_alloc(sizeof(uint16_t) * SWEEP_MAX_BINS);
    s_pc_q = xQueueCreate(8, sizeof(fpv_cmd_t));
    uint64_t *sum = (uint64_t *)big_alloc(sizeof(uint64_t) * SWEEP_MAX_BINS, true);
    uint8_t *cnt = (uint8_t *)big_alloc(SWEEP_MAX_BINS, true);
    void *v = big_alloc(sizeof(UiView));
    if (!s_psd || !s_work || !s_shared || !s_avg || !s_hold || !s_scratch || !s_ring || !sum || !cnt || !v ||
        !s_codes || !s_pc_q)
        halt("out of memory");
    an_ring_init(&s_avg_ring, s_ring, sum, cnt, FPV_AVG_SWEEPS, SWEEP_MAX_BINS);
    s_viewp = new (v) UiView();
}
}  // namespace fpv

/* Before the radio starts: refuse to run on a flat battery. After a
 * low-battery shutdown the timer wakes us every FPV_BAT_WAKE_S; we go back to
 * sleep without touching the LCD until the battery has been charged. */
extern "C" void fpv_monitor_early(void) {
    using namespace fpv;
    for (gpio_num_t p : HELD_PINS) gpio_hold_dis(p);
    bool timer_wake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    int need = (timer_wake && s_low_sleep) ? FPV_BAT_RESUME_MV : FPV_BAT_BOOT_MV;
    if (!bat_init()) { s_low_sleep = false; return; }
    int mv = bat_read_mv();
    s_bat_mv = mv;
    if (!battery_low(mv, need)) { s_low_sleep = false; return; }
    if (!timer_wake) {
        /* power-on or reset: say why, then sleep */
        char b[48];
        s_lcd.init();
        s_lcd.setRotation(FPV_LCD_ROTATION);
        backlight(true);
        s_lcd.fillScreen(TFT_BLACK);
        s_lcd.setFont(&fonts::Font4);
        s_lcd.setTextDatum(textdatum_t::middle_center);
        s_lcd.setTextColor(TFT_RED, TFT_BLACK);
        snprintf(b, sizeof(b), "Battery low  %d.%02d V", mv / 1000, (mv % 1000) / 10);
        s_lcd.drawString(b, SCR_W / 2, SCR_H / 2 - 16);
        s_lcd.setFont(&fonts::Font2);
        s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        s_lcd.drawString("charge the battery", SCR_W / 2, SCR_H / 2 + 16);
        vTaskDelay(pdMS_TO_TICKS(3000));
        s_lcd.sleep();
    }
    power_off();
}

extern "C" void fpv_monitor_main(void) {
    using namespace fpv;
    alloc_buffers();
    s_lock = xSemaphoreCreateMutex();
    settings_load();
    s_req.gain = s_set.gain;
    xTaskCreate(ui_task, "fpv_ui", 8192, nullptr, 5, &s_ui_task);
    receiver_loop();
}
