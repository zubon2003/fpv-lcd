/* Stand-alone FPV 5.8 GHz band monitor on an ILI9341 + XPT2046 LCD.
 *
 * Two tasks:
 *   receiver (app_main's task): hops the LO over the current span, takes one
 *     80 MS/s snapshot per hop, FFTs it and min-combines it into a sweep;
 *     each finished sweep is published to the UI under a mutex.
 *   ui: polls the touch panel, folds each sweep into the average / peak hold /
 *     waterfall, measures the channels and redraws.
 *
 * Modes: 4CH (E2 E1 F3 F5), 4CH2 (E1 A8 A5 F5), 4CH3 (R2 F1 R4 R5),
 * 3CH (E1 F1 F4), 1CH (one of A5 A8 E1 E2 F1 F3 F4 F5 R2 R4 R5, centre
 * +-15 MHz).
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
#include "esp_wifi.h"
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
#if FPV_PANEL_RESTOUCH35
/* Waveshare Pico-ResTouch-LCD-3.5: an ILI9488 on its 16-bit parallel bus,
 * fed from SPI through shift registers. Every SPI word is 16 bits (commands
 * and parameters too: dlen_16bit), so pixels go as RGB565 - LovyanGFX's
 * ILI9488 would pick 18-bit colour for an SPI bus. Nothing can be read back. */
struct Panel_ResTouch35 : public lgfx::Panel_ILI9488 {
    void setColorDepth_impl(lgfx::color_depth_t) override {
        _write_depth = lgfx::rgb565_2Byte;
        _read_depth = lgfx::rgb565_2Byte;
    }
};
#endif

class LGFX : public lgfx::LGFX_Device {
#if FPV_PANEL_RESTOUCH35
    Panel_ResTouch35 panel_;
#elif FPV_PANEL_CAPTOUCH35
    lgfx::Panel_ST7796 panel_;
#else
    lgfx::Panel_ILI9341 panel_;
#endif
    lgfx::Bus_SPI bus_;
#if FPV_PANEL_CAPTOUCH35
    lgfx::Touch_FT5x06 touch_;      /* FT6336U */
#else
    lgfx::Touch_XPT2046 touch_;
#endif

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
#if FPV_PANEL_RESTOUCH35
            cfg.panel_width = 320;
            cfg.panel_height = 480;
            cfg.offset_rotation = 0;
            cfg.readable = false;       /* behind shift registers */
            cfg.invert = FPV_LCD_INVERT;
            cfg.rgb_order = false;
            cfg.dlen_16bit = true;      /* 16-bit words: the parallel bus */
#elif FPV_PANEL_CAPTOUCH35
            cfg.panel_width = 320;
            cfg.panel_height = 480;
            cfg.offset_rotation = 0;
            cfg.readable = false;       /* MISO is not wired */
            cfg.invert = FPV_LCD_INVERT;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
#else
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.offset_rotation = 0;
            cfg.readable = false;       /* LCD SDO is not wired */
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
#endif
#if FPV_PANEL_CAPTOUCH35
            cfg.bus_shared = false;     /* the touch is on I2C */
#else
            cfg.bus_shared = true;      /* the touch controller shares SPI */
#endif
            panel_.config(cfg);
        }
        {
            auto cfg = touch_.config();
#if FPV_PANEL_CAPTOUCH35
            /* reports panel pixels in the panel's own (portrait) frame */
            cfg.x_min = 0;
            cfg.x_max = 319;
            cfg.y_min = 0;
            cfg.y_max = 479;
            cfg.pin_int = -1;
            cfg.pin_rst = FPV_PIN_TOUCH_RST;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port = 0;
            cfg.i2c_addr = FPV_TOUCH_I2C_ADDR;
            cfg.freq = FPV_TOUCH_I2C_HZ;
            cfg.pin_sda = FPV_PIN_TOUCH_SDA;
            cfg.pin_scl = FPV_PIN_TOUCH_SCL;
#else
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
#endif
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
/* The groups' channels. 1CH shows one of the current group's channels: the
 * triangle buttons (and "FPV CH") step through the group, in this order. */
static const char *const G4[] = {"E2", "E1", "F3", "F5"};
static const char *const G4_2[] = {"E1", "A8", "A5", "F5"};
static const char *const G4_3[] = {"R2", "F1", "R4", "R5"};
static const char *const G3[] = {"E1", "F1", "F4"};

static const char *const *group_names(int group, int *n) {
    switch (group) {
    case MODE_4CH2: *n = 4; return G4_2;
    case MODE_4CH3: *n = 4; return G4_3;
    case MODE_3CH: *n = 3; return G3;
    default: *n = 4; return G4;
    }
}

static bool is_group(int m) { return m == MODE_4CH || m == MODE_4CH2 || m == MODE_4CH3 || m == MODE_3CH; }

struct Settings {
    uint8_t mode = MODE_4CH;            /* the group (never MODE_1CH) */
    uint8_t single = 2;                 /* index in the group (F3 in 4CH), SINGLE_ON: 1CH shown */
    int8_t gain = FPV_DEFAULT_GAIN;
    uint8_t snr = FPV_DEFAULT_SNR_DB;   /* detection threshold */
};
constexpr uint8_t SINGLE_ON = 0x80;
static Settings s_set;
static const char *NVS_NS = "fpvmon";

static int single_index() { return s_set.single & 0x7f; }
static bool single_shown() { return s_set.single & SINGLE_ON; }
static const char *single_name() {
    int n;
    const char *const *names = group_names(s_set.mode, &n);
    return names[single_index() < n ? single_index() : 0];
}

/* "set3": the group and 1CH within it. Older firmware ("set2", "set") had
 * 1CH as a mode of its own: it comes back as its group (4CH for 1CH). */
static const char *NVS_SET = "set3";

static void settings_load() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    Settings t;
    size_t n = sizeof(t);
    bool ok = nvs_get_blob(h, NVS_SET, &t, &n) == ESP_OK && n == sizeof(t);
    if (!ok) {
        const char *old[] = {"set2", "set"};
        for (int i = 0; i < 2 && !ok; i++) {
            n = sizeof(t);
            ok = nvs_get_blob(h, old[i], &t, &n) == ESP_OK && n == sizeof(t);
        }
        if (ok) {
            if (!is_group(t.mode)) t.mode = MODE_4CH;
            t.single = 0;
        }
    }
    int gn;
    group_names(t.mode, &gn);
    if (ok && is_group(t.mode) && (t.single & 0x7f) < gn && t.snr >= FPV_SNR_MIN_DB && t.snr <= FPV_SNR_MAX_DB)
        s_set = t;
    nvs_close(h);
}

static void settings_save() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_SET, &s_set, sizeof(s_set));
    nvs_commit(h);
    nvs_close(h);
}

static bool touch_cal_load(uint16_t cal[8]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 8 * sizeof(uint16_t);
    bool ok = nvs_get_blob(h, FPV_TCAL_KEY, cal, &n) == ESP_OK && n == 8 * sizeof(uint16_t);
    nvs_close(h);
    return ok;
}

static void touch_cal_save(const uint16_t cal[8]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, FPV_TCAL_KEY, cal, 8 * sizeof(uint16_t));
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------- channels */
/* Colour slots (red, green, blue, yellow, purple, cyan): by frequency, lowest
 * first: red, green, blue, yellow. 1CH keeps the colour its channel has in
 * the group. */
static void assign_colours(UiChannel *ch, int n) {
    for (int i = 0; i < n; i++) {
        int rank = 0;
        for (int j = 0; j < n; j++)
            if (ch[j].nominal_mhz < ch[i].nominal_mhz || (ch[j].nominal_mhz == ch[i].nominal_mhz && j < i)) rank++;
        ch[i].color = rank;
    }
}

struct Lineup {
    int n;
    fpv_channel_t ch[MAX_CH];
};

static Lineup group_lineup(int group) {
    Lineup l{};
    const char *const *names = group_names(group, &l.n);
    for (int i = 0; i < l.n; i++) fpv_channel_parse(names[i], &l.ch[i]);
    return l;
}

/* A group with its colours, for the mode list and the Wi-Fi page. */
static GroupInfo group_info(Mode group) {
    Lineup l = group_lineup(group);
    UiChannel ch[MAX_CH];
    for (int i = 0; i < l.n; i++) ch[i].nominal_mhz = l.ch[i].freq_mhz;
    assign_colours(ch, l.n);
    GroupInfo g;
    g.mode = group;
    g.n = l.n;
    for (int i = 0; i < l.n; i++) {
        memcpy(g.name[i], l.ch[i].name, sizeof(g.name[i]));
        g.mhz[i] = l.ch[i].freq_mhz;
        g.color[i] = ch[i].color;
    }
    return g;
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
static volatile int s_clip_permille;
/* Diagnostics: the last presses seen by the UI (FPV TOUCH?) and its page. */
struct TouchLog { uint32_t ms; int16_t x, y; uint8_t page; uint16_t dur_ms; };
constexpr int TOUCH_LOG_N = 12;
static TouchLog s_touch_log[TOUCH_LOG_N];
static volatile unsigned s_touch_n;
static volatile int s_page_shown, s_hold_shown;
static volatile int s_open_wifi = -1;       /* FPV WIFICHECK: group to open */        /* last sweep: worst hop's saturated samples */
static volatile bool s_shutdown;            /* receiver -> UI: battery low */

/* ---------------------------------------------------------------- power */
/* Set while sleeping because of a low battery: the next timer wake then
 * waits for FPV_BAT_RESUME_MV (charged) instead of FPV_BAT_BOOT_MV. */
static RTC_DATA_ATTR bool s_low_sleep;

static const gpio_num_t HELD_PINS[] = {
    (gpio_num_t)FPV_PIN_LCD_CS, (gpio_num_t)FPV_PIN_LCD_RST,
#if FPV_PIN_TOUCH_CS >= 0
    (gpio_num_t)FPV_PIN_TOUCH_CS,
#endif
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
    io.pin_bit_mask = (1ULL << FPV_PIN_LCD_CS) | (1ULL << FPV_PIN_LCD_RST);
#if FPV_PIN_TOUCH_CS >= 0
    io.pin_bit_mask |= 1ULL << FPV_PIN_TOUCH_CS;
#endif
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)FPV_PIN_LCD_CS, 1);
#if FPV_PIN_TOUCH_CS >= 0
    gpio_set_level((gpio_num_t)FPV_PIN_TOUCH_CS, 1);
#endif
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
static volatile bool s_nolcd;               /* FPV NOLCD: LCD off, sweep flat out */
static const char *mode_name(int m) { return mode_label((Mode)m); }
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
    if (!link_send(h, (size_t)n, deadline) || !link_send(s_codes, bytes, deadline)) {
        s_stream = false;                    /* host gone: the LCD comes back */
        s_nolcd = false;
    }
}

/* ------------------------------------------------- Wi-Fi listening (receive only) */
/* Frames heard on one Wi-Fi channel. Filled by the Wi-Fi driver's task while
 * sdr_hw_wifi_listen waits, read once it has returned. On the heap: .bss is
 * nearly full (see big_alloc). */
constexpr int WIFI_LISTEN_MAX_AP = 16;
struct WifiListen {
    unsigned frames, mgmt, ctrl, data, bytes;
    int rssi_max;
    int nap;
    WifiApInfo ap[WIFI_LISTEN_MAX_AP];
};
static WifiListen *s_wl;
static bool s_wifi_used;      /* the radio was lent to the Wi-Fi driver: retune */
static unsigned s_wl_channel; /* channel being listened to (APs without HT info) */

static int wifi_ch_mhz(int ch) { return ch <= 14 ? (ch == 14 ? 2484 : 2407 + 5 * ch) : 5000 + 5 * ch; }

/* Occupied span from the beacon: HT operation (40 MHz) and VHT operation
 * (80 / 160 MHz); 20 MHz around the primary otherwise. */
static void wifi_span(WifiApInfo &a, int ht_off, bool ht40, int vht_w, int seg0, int seg1) {
    int f = wifi_ch_mhz(a.chan ? a.chan : (int)s_wl_channel);
    int lo = f - 10, hi = f + 10;
    if (vht_w >= 1 && seg0) {
        int c = seg1 && (seg1 - seg0 == 8 || seg0 - seg1 == 8) ? seg1 : seg0;
        int half = c == seg0 ? 40 : 80;
        lo = wifi_ch_mhz(c) - half;
        hi = wifi_ch_mhz(c) + half;
    } else if (ht40 && ht_off == 1) {
        hi = f + 30;
    } else if (ht40 && ht_off == 3) {
        lo = f - 30;
    }
    a.lo_mhz = (uint16_t)lo;
    a.hi_mhz = (uint16_t)hi;
}

static void wifi_frame(void *buf, int type) {
    const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
    WifiListen &w = *s_wl;
    int rssi = pk->rx_ctrl.rssi;
    unsigned len = pk->rx_ctrl.sig_len;
    w.frames++;
    w.bytes += len;
    if (w.frames == 1 || rssi > w.rssi_max) w.rssi_max = rssi;
    if (type == WIFI_PKT_MGMT) w.mgmt++;
    else if (type == WIFI_PKT_CTRL) w.ctrl++;
    else if (type == WIFI_PKT_DATA) w.data++;
    if (type != WIFI_PKT_MGMT || len < 40) return;
    const uint8_t *f = pk->payload;
    unsigned sub = (f[0] >> 4) & 15;
    if (sub != 8 && sub != 5) return;               /* beacon / probe response */
    const uint8_t *bssid = f + 16;
    int i = 0;
    while (i < w.nap && memcmp(w.ap[i].bssid, bssid, 6)) i++;
    if (i == w.nap) {
        if (w.nap == WIFI_LISTEN_MAX_AP) return;
        WifiApInfo &a = w.ap[w.nap++];
        memset(&a, 0, sizeof(a));
        memcpy(a.bssid, bssid, 6);
        a.rssi = (int8_t)rssi;
        int ht_off = 0, vht_w = 0, seg0 = 0, seg1 = 0;
        bool ht40 = false;
        unsigned end = len > 4 ? len - 4 : len;        /* without the FCS */
        for (unsigned k = 36; k + 2 <= end;) {         /* information elements */
            unsigned id = f[k], n = f[k + 1];
            const uint8_t *v = f + k + 2;
            if (k + 2 + n > end) break;
            if (id == 0 && n <= 32) {
                for (unsigned j = 0; j < n; j++) a.ssid[j] = (v[j] >= 32 && v[j] < 127) ? (char)v[j] : '?';
            } else if (id == 3 && n >= 1) {
                a.chan = v[0];
            } else if (id == 61 && n >= 2) {               /* HT operation */
                a.chan = v[0];
                ht_off = v[1] & 3;
                ht40 = (v[1] >> 2) & 1;
            } else if (id == 192 && n >= 3) {              /* VHT operation */
                vht_w = v[0];
                seg0 = v[1];
                seg1 = v[2];
            }
            k += 2 + n;
        }
        wifi_span(a, ht_off, ht40, vht_w, seg0, seg1);
    }
    WifiApInfo &a = w.ap[i];
    if (sub == 8) a.beacons++;
    if (rssi > a.rssi) a.rssi = (int8_t)rssi;
}

static bool wifi_listen(int channel, int ms) {
    memset(s_wl, 0, sizeof(*s_wl));
    s_wl_channel = (unsigned)channel;
    bool ok = sdr_hw_wifi_listen((unsigned)channel, (unsigned)ms, wifi_frame);
    s_wifi_used = true;
    return ok;
}

static void wifi_listen_reply(int channel, int ms) {
    char r[160];
    if (!wifi_listen(channel, ms)) {
        snprintf(r, sizeof(r), "ERR channel %d refused\n", channel);
        link_reply(r);
        return;
    }
    const WifiListen &w = *s_wl;
    snprintf(r, sizeof(r), "WIFI ch=%d ms=%d frames=%u mgmt=%u ctrl=%u data=%u bytes=%u rssi_max=%d aps=%d\n",
             channel, ms, w.frames, w.mgmt, w.ctrl, w.data, w.bytes, w.frames ? w.rssi_max : 0, w.nap);
    link_reply(r);
    for (int i = 0; i < w.nap; i++) {
        const WifiApInfo &a = w.ap[i];
        snprintf(r, sizeof(r), "AP %02x:%02x:%02x:%02x:%02x:%02x ch=%d mhz=%d-%d rssi=%d beacons=%u ssid=%s\n",
                 a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5], a.chan, a.lo_mhz,
                 a.hi_mhz, a.rssi, a.beacons, a.ssid);
        link_reply(r);
    }
    link_reply("END\n");
}

/* The LCD's Wi-Fi check, per group: of these 20 MHz channels, only those
 * whose signal would overlap one of the group's channels (Direct) or put a
 * third-order product on one (IMD: 2 x VTX - Wi-Fi, 2 x Wi-Fi - VTX), each
 * FPV_WIFI_DWELL_MS. A wider BSS is a run of these channels; it is heard on
 * each (frames), named where its beacons are (the primary). */
static const uint8_t WIFI_CANDIDATES[WIFI_SCAN_MAX_CH] = {100, 104, 108, 112, 116, 120, 124, 128, 132, 136,
                                                          140, 144, 149, 153, 157, 161, 165, 169, 173, 177};
static WifiView *s_scan;              /* shared with the UI, under s_lock */
static WifiView *s_scan_view;         /* the UI's copy */
static volatile uint32_t s_scan_ver;  /* bumped on every change (under s_lock) */
static void scan_changed() { s_scan_ver = s_scan_ver + 1; }
static volatile bool s_scan_req, s_scan_cancel;
static volatile Mode s_scan_group = MODE_4CH;

/* What a signal on Wi-Fi channel ch does to the group's channels. */
static void wifi_relation(const GroupInfo &g, int ch, WifiChanInfo &c) {
    const float h = FPV_BW_MARK_OUTER_MHZ / 2;
    const float lo = (float)(5000 + 5 * ch - 10), hi = lo + 20;
    auto hits = [&](int k, float a, float b) { return (float)g.mhz[k] + h > a && (float)g.mhz[k] - h < b; };
    c.direct = 0;
    for (int k = 0; k < MAX_CH; k++) c.imd[k] = 0;
    for (int k = 0; k < g.n; k++)
        if (hits(k, lo, hi)) c.direct |= (uint8_t)(1u << k);
    for (int k = 0; k < g.n; k++)                       /* 2 x VTX - Wi-Fi first: the stronger one */
        for (int v = 0; v < g.n && !c.imd[k]; v++)
            if (v != k && hits(k, 2.0f * g.mhz[v] - hi, 2.0f * g.mhz[v] - lo)) c.imd[k] = (int8_t)(v + 1);
    for (int k = 0; k < g.n; k++)
        for (int v = 0; v < g.n && !c.imd[k]; v++)
            if (v != k && hits(k, 2.0f * lo - g.mhz[v], 2.0f * hi - g.mhz[v])) c.imd[k] = (int8_t)-(v + 1);
}

/* The channels to check for a group. Called with s_lock held. */
static void scan_prepare(Mode group) {
    memset(s_scan, 0, sizeof(*s_scan));
    s_scan->group = group_info(group);
    s_scan->current = -1;
    for (int i = 0; i < WIFI_SCAN_MAX_CH; i++) {
        WifiChanInfo c{};
        c.ch = WIFI_CANDIDATES[i];
        wifi_relation(s_scan->group, c.ch, c);
        bool any = c.direct != 0;
        for (int k = 0; k < MAX_CH; k++) any = any || c.imd[k];
        if (any) s_scan->chan[s_scan->nch++] = c;
    }
    scan_changed();
}

static void run_wifi_scan() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scan_prepare(s_scan_group);
    s_scan->running = true;
    const int nch = s_scan->nch;
    xSemaphoreGive(s_lock);
    for (int i = 0; i < nch && !s_scan_cancel; i++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_scan->current = i;
        const int ch = s_scan->chan[i].ch;
        scan_changed();
        xSemaphoreGive(s_lock);
        bool ok = wifi_listen(ch, FPV_WIFI_DWELL_MS);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        WifiChanInfo &c = s_scan->chan[i];
        c.frames = ok ? (uint16_t)(s_wl->frames > 65535 ? 65535 : s_wl->frames) : 0;
        c.rssi_max = (int8_t)(s_wl->frames ? s_wl->rssi_max : -127);
        for (int k = 0; ok && k < s_wl->nap; k++) {     /* merge, by BSSID */
            const WifiApInfo &a = s_wl->ap[k];
            int j = 0;
            while (j < s_scan->nap && memcmp(s_scan->ap[j].bssid, a.bssid, 6)) j++;
            if (j == s_scan->nap) {
                if (j == WIFI_SCAN_MAX_AP) continue;
                s_scan->ap[s_scan->nap++] = a;
            } else {
                WifiApInfo &m = s_scan->ap[j];
                if (a.rssi > m.rssi) m.rssi = a.rssi;
                m.beacons += a.beacons;
            }
        }
        s_scan->done = i + 1;
        scan_changed();
        xSemaphoreGive(s_lock);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan->running = false;
    s_scan->current = -1;
    scan_changed();
    xSemaphoreGive(s_lock);
}

/* "FPV CH" takes a channel of the current group. */
static fpv_cmd_kind_t parse_line(const char *line, fpv_cmd_t *c) {
    int n;
    const char *const *names = group_names(s_set.mode, &n);
    return fpv_parse(line, names, n, c);
}

static void fpv_command(const char *line, const sweep_plan_t &plan, int gain) {
    fpv_cmd_t c;
    char r[200];
    switch (parse_line(line, &c)) {
    case FC_HELLO:
        snprintf(r, sizeof(r), "FPV %d bin_hz=%d maxbins=%d step=%d usable=%.0f dc_khz=%.0f samples=%d\n",
                 FPV_LINK_VERSION, DSP_BIN_HZ, SWEEP_MAX_BINS, FPV_STEP_MHZ, (double)FPV_USABLE_MHZ,
                 (double)FPV_DC_KHZ, FPV_SAMPLES);
        link_reply(r);
        return;
    case FC_STATE:
        snprintf(r, sizeof(r),
                 "STATE mode=%s ch=%s gain=%d snr=%d start=%d nbins=%d hops=%d stream=%d lcd=%d bat_mv=%d clip=%d page=%d hold=%d\n",
                 mode_name(s_shown_mode), single_name(), gain, s_set.snr, plan.start_mhz,
                 plan.nbins, plan.nhops, s_stream ? 1 : 0, s_nolcd ? 0 : 1, (int)s_bat_mv,
                 (int)s_clip_permille, (int)s_page_shown, (int)s_hold_shown);
        link_reply(r);
        return;
    case FC_STREAM:
        s_stream = c.ival != 0;
        if (!s_stream) s_nolcd = false;
        link_reply("OK\n");
        return;
    case FC_NOLCD:
        s_nolcd = true;
        xTaskNotifyGive(s_ui_task);
        link_reply("OK\n");
        return;
    case FC_LCD:
        s_nolcd = false;
        xTaskNotifyGive(s_ui_task);
        link_reply("OK\n");
        return;
    case FC_WIFI:
        wifi_listen_reply(c.ival, c.ms);
        return;
    case FC_WIFICHECK:
        s_open_wifi = c.ival;
        link_reply("OK\n");
        return;
    case FC_TOUCHLOG: {
        unsigned n = s_touch_n, first = n > TOUCH_LOG_N ? n - TOUCH_LOG_N : 0;
        for (unsigned i = first; i < n; i++) {
            const TouchLog &e = s_touch_log[i % TOUCH_LOG_N];
            snprintf(r, sizeof(r), "TOUCH %u %d %d %d %u\n", (unsigned)e.ms, e.x, e.y, e.page, e.dur_ms);
            link_reply(r);
        }
        link_reply("END\n");
        return;
    }
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
        if (parse_line(line, &probe) != FC_NONE) {
            fpv_command(line, plan, gain);
            continue;
        }
        sdr_hw_command(line);                /* stock protocol: the PC's own sweep */
        raw_until = !strcmp(line, "RELEASE") ? 0 : esp_timer_get_time() + 5000000;
    }
    bool raw = raw_until && esp_timer_get_time() < raw_until;
    if (!raw) raw_until = 0;
    s_raw = raw;
    bool lent = s_wifi_used;              /* FPV WIFI: take the radio back too */
    s_wifi_used = false;
    return raw || lent;
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
        if (s_scan_req) {                      /* the LCD's Wi-Fi page */
            s_scan_req = false;
            s_scan_cancel = false;
            run_wifi_scan();
            s_wifi_used = false;
            was_raw = true;                    /* then retune and set the gain again */
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
        unsigned clip_max = 0;
        for (int h = 0; h < plan.nhops; h++) {
            if (__atomic_load_n(&s_req.gen, __ATOMIC_RELAXED) != gen || s_scan_req) { aborted = true; break; }   /* mode changed / Wi-Fi scan */
            if (link_service(plan, gain)) { aborted = true; was_raw = true; break; }            /* PC took the radio */
            const uint32_t *iq = nullptr;
            if (!sdr_hw_tune((unsigned)plan.lo_mhz[h]) || !sdr_hw_capture(FPV_SAMPLES, &iq)) {
                s_rx_errors++;
                continue;
            }
            unsigned clip = dsp_clipped(iq, FPV_SAMPLES);
            if (clip > clip_max) clip_max = clip;
            dsp_hop_psd(iq, FPV_SAMPLES, s_psd);
            sweep_stitch(&plan, h, s_psd, s_work, FPV_USABLE_MHZ, FPV_DC_KHZ);
            vTaskDelay(1);                       /* let the idle task run */
        }
        if (aborted) continue;
        s_clip_permille = (int)(clip_max * 1000u / FPV_SAMPLES);

        int64_t t_done = esp_timer_get_time();
        if (!s_nolcd) {                         /* NOLCD: the UI does nothing */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(s_shared, s_work, sizeof(float) * plan.nbins);
            s_shared_gen = gen;
            s_shared_seq++;
            s_shared_t_us = t_done;
            xSemaphoreGive(s_lock);
            xTaskNotifyGive(s_ui_task);
        }
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
    Lineup l = group_lineup(s_set.mode);
    UiChannel grp[MAX_CH];
    for (int i = 0; i < l.n; i++) grp[i].nominal_mhz = l.ch[i].freq_mhz;
    assign_colours(grp, l.n);
    int keep = -1;                           /* 1CH: the one channel of the group shown */
    if (s_pc.active) {
        l.n = s_pc.n;
        for (int i = 0; i < l.n; i++) l.ch[i] = s_pc.ch[i];
    } else if (single_shown()) {
        keep = single_index() < l.n ? single_index() : 0;
    }
    float lo = 1e9f, hi = -1e9f;
    int freqs[MAX_CH];
    s_view.mode = s_pc.active ? MODE_PC : keep >= 0 ? MODE_1CH : (Mode)s_set.mode;
    s_shown_mode = s_view.mode;
    s_view.back_mode = (Mode)s_set.mode;
    s_view.nch = keep >= 0 ? 1 : l.n;
    for (int i = 0; i < s_view.nch; i++) {
        int src = keep >= 0 ? keep : i;
        UiChannel &c = s_view.ch[i];
        memcpy(c.name, l.ch[src].name, sizeof(c.name));
        c.nominal_mhz = l.ch[src].freq_mhz;
        c.color = grp[src].color;
        freqs[i] = c.nominal_mhz;
        lo = fminf(lo, (float)c.nominal_mhz);
        hi = fmaxf(hi, (float)c.nominal_mhz);
    }
    if (s_pc.active) assign_colours(s_view.ch, s_view.nch);
    if (s_pc.active) sweep_plan(&s_view.plan, s_pc.start, s_pc.stop, FPV_STEP_MHZ);
    else sweep_plan(&s_view.plan, lo - FPV_MARGIN_MHZ, hi + FPV_MARGIN_MHZ, FPV_STEP_MHZ);
    s_st.search_mhz = an_auto_search(freqs, s_view.nch, 8.0f);
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

/* Show a group (single < 0) or one of its channels (1CH); leaves PC mode. */
static void choose(Mode group, int single = -1, bool draw = true) {
    s_pc.active = false;
    s_set.mode = group;
    if (single >= 0) s_set.single = (uint8_t)(single | SINGLE_ON);
    else s_set.single &= (uint8_t)~SINGLE_ON;
    apply_settings();
    if (draw) s_ui.draw_all(s_view);
    settings_save();
}

/* Main screen taps (the setup screen and the lists are handled in ui_task). */
static void on_hit(Hit hit) {
    if (hit >= HIT_CARD0 && hit < HIT_CARD0 + s_view.nch) {   /* zoom into that channel */
        choose((Mode)s_set.mode, hit - HIT_CARD0);
        return;
    }
    switch (hit) {
    case HIT_BACK:                       /* 1CH -> its group */
        choose((Mode)s_set.mode);
        return;
    case HIT_HOLD:
        s_view.hold_on = !s_view.hold_on;
        s_st.hold_first = true;
        return;
    case HIT_CH_PREV:
    case HIT_CH_NEXT: {
        int n;                           /* step within the group */
        group_names(s_set.mode, &n);
        int step = hit == HIT_CH_NEXT ? 1 : n - 1;
        choose((Mode)s_set.mode, (single_index() + step) % n);
        return;
    }
    default:
        return;
    }
}

static void calibrate_touch();

/* ---------------------------------------------------------- setup screen */
static Settings s_edit;          /* what the setup screen shows / edits */

static SetupView setup_view() {
    SetupView sv;
    sv.gain = s_edit.gain;
    sv.snr_db = s_edit.snr;
    sv.bat_mv = s_bat_mv;
    sv.sweeps_per_s = s_view.sweeps_per_s;
    sv.floor_db = s_view.floor_db;
    sv.clip_permille = s_clip_permille;
    sv.rx_errors = s_rx_errors;
    return sv;
}

/* Returns true when the setup screen closes. */
static bool on_setup_hit(SetupHit h) {
    switch (h) {
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
        bool regain = s_edit.gain != s_set.gain;
        s_set.gain = s_edit.gain;
        s_set.snr = s_edit.snr;
        settings_save();
        s_view.snr_db = s_set.snr;
        if (regain) set_gain_now(s_set.gain);
        return true;
    }
    default: break;
    }
    s_ui.draw_setup(setup_view());
    return false;
}

/* --------------------------------------------------------------- mode list */
static ModesView modes_view() {
    ModesView mv;
    for (int r = 0; r < GROUP_COUNT; r++) mv.row[r] = group_info(GROUP_ORDER[r]);
    mv.current = s_pc.active ? MODE_PC : (Mode)s_set.mode;
    return mv;
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
    s_view.clip_permille = s_clip_permille;
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
        case FC_MODE:                    /* a group, or 1CH within the current one */
            s_pc.active = false;
            if (c.ival == MODE_1CH) s_set.single |= SINGLE_ON;
            else { s_set.mode = (uint8_t)c.ival; s_set.single = 0; }
            apply_settings();
            break;
        case FC_CH:                      /* index in the current group */
            s_set.single = (uint8_t)(c.ival | (s_set.single & SINGLE_ON));
            if (single_shown() && !s_pc.active) apply_settings();
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
    } else if (!have_cal && FPV_TOUCH_CAL_FIRST_BOOT) {
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
    /* what is on the LCD: the monitor, or a full-screen page over it */
    enum Page { PG_MAIN, PG_SETUP, PG_MODES, PG_WIFI } page = PG_MAIN;
    uint32_t wifi_drawn = 0;              /* s_scan_ver on the screen */
    auto draw_wifi_page = [&]() {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_scan_view, s_scan, sizeof(*s_scan_view));
        wifi_drawn = s_scan_ver;
        xSemaphoreGive(s_lock);
        s_ui.draw_wifi(*s_scan_view);
    };
    bool raw_shown = false;
    bool lcd_off = false;                 /* FPV NOLCD in effect */
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
    auto back_to_main = [&](int64_t now) {
        page = PG_MAIN;
        s_lcd.fillScreen(TFT_BLACK);
        s_ui.draw_axis(s_view);
        draw_all_now(now);
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
        /* FPV NOLCD: panel asleep, backlight off, no processing; a tap (or
         * FPV LCD / the end of the stream) brings it back */
        if (s_nolcd != lcd_off) {
            lcd_off = s_nolcd;
            if (lcd_off) {
                s_lcd.fillScreen(TFT_BLACK);
                backlight(false);
                s_lcd.sleep();
            } else {
                s_lcd.wakeup();
                backlight(true);
                page = PG_MAIN;
                s_lcd.fillScreen(TFT_BLACK);
                reset_measurement();
                s_ui.draw_axis(s_view);
                s_ui.draw_frame(s_view);
                t_spec = t_ro = t_hdr = now0;
            }
        }
        if (lcd_off) {
            apply_pc_commands();                 /* still take settings, draw nothing */
            int32_t tx, ty;
            if (s_lcd.getTouch(&tx, &ty)) {
                s_nolcd = false;                 /* woken by hand */
                touching = true;                 /* this tap does nothing else */
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (apply_pc_commands()) {               /* settings from the PC */
            if (page == PG_SETUP) { s_edit = s_set; s_ui.draw_setup(setup_view()); }
            else if (page != PG_MAIN) { s_scan_cancel = true; back_to_main(now0); }   /* a list is stale now */
            else { s_ui.draw_all(s_view); t_spec = t_ro = t_hdr = now0; }
        }
        /* the PC drives the radio with raw I/Q: say so on the plot */
        if (s_raw != raw_shown) {
            raw_shown = s_raw;
            s_view.message = raw_shown ? "PC is using the receiver (I/Q)" : nullptr;
            s_st.message_until_us = raw_shown ? INT64_MAX : 0;
            if (page == PG_MAIN) s_ui.draw_spectrum(s_view);
        }

        int32_t tx, ty;
        bool raw = s_lcd.getTouch(&tx, &ty);
        int64_t now = esp_timer_get_time();
        /* diagnostics (FPV TOUCH?): every press, where and how long */
        static bool raw_prev;
        static int64_t raw_t0;
        static TouchLog *cur;
        if (raw && !raw_prev) {
            raw_t0 = now;
            cur = &s_touch_log[s_touch_n % TOUCH_LOG_N];
            cur->ms = (uint32_t)(now / 1000);
            cur->x = (int16_t)tx;
            cur->y = (int16_t)ty;
            cur->page = (uint8_t)page;
            cur->dur_ms = 0;
            s_touch_n = s_touch_n + 1;
        }
        if (cur && raw) {
            int64_t d = (now - raw_t0) / 1000;
            cur->dur_ms = (uint16_t)(d > 65535 ? 65535 : d);
        }
        bool t = raw;
        raw_prev = raw;
        s_page_shown = page;
        s_hold_shown = s_view.hold_on;
        if (s_open_wifi >= 0 && page == PG_MAIN) {   /* FPV WIFICHECK */
            page = PG_WIFI;
            s_scan_group = (Mode)s_open_wifi;
            s_open_wifi = -1;
            s_scan_cancel = false;
            s_scan_req = true;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            scan_prepare(s_scan_group);
            xSemaphoreGive(s_lock);
            draw_wifi_page();
        }
        if (page == PG_SETUP) {
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
            if (close) back_to_main(now);
        } else if (page == PG_WIFI) {
            if (t && !touching) {
                touching = true;
                int i = wifi_hit((int)tx, (int)ty);
                if (i == PICK_CLOSE) {
                    s_scan_cancel = true;          /* the receiver goes back to sweeping */
                    back_to_main(now);
                } else if (i == PICK_RESCAN) {
                    if (s_scan_view->running) s_scan_cancel = true;
                    else s_scan_req = true;
                }
            } else if (!t && touching) {
                touching = false;
            }
            if (page == PG_WIFI && s_scan_ver != wifi_drawn) draw_wifi_page();
        } else if (page == PG_MODES) {
            /* mode list: a group, or that group's Wi-Fi check */
            if (t && !touching) {
                touching = true;
                bool wifi = false;
                int i = modes_hit((int)tx, (int)ty, &wifi);
                if (i == PICK_CLOSE) {
                    back_to_main(now);
                } else if (i >= 0 && wifi) {
                    page = PG_WIFI;
                    s_scan_group = GROUP_ORDER[i];
                    s_scan_cancel = false;
                    s_scan_req = true;
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    scan_prepare(s_scan_group);           /* shows the channels to check at once */
                    xSemaphoreGive(s_lock);
                    draw_wifi_page();
                } else if (i >= 0) {
                    choose(GROUP_ORDER[i], -1, false);
                    back_to_main(now);
                }
            } else if (!t && touching) {
                touching = false;
            }
        } else if (t && !touching) {
            touching = true;
            long_done = false;
            touch_t0 = now;
            touch_hit = hit_test(s_view, (int)tx, (int)ty);
            if (touch_hit == HIT_SETUP) {
                page = PG_SETUP;
                s_edit = s_set;
                s_ui.draw_setup(setup_view());
            } else if (touch_hit == HIT_MODE) {
                page = PG_MODES;
                s_ui.draw_modes(modes_view());
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
        if (page != PG_MAIN) continue;        /* keep measuring, draw nothing */
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
    s_wl = (WifiListen *)big_alloc(sizeof(WifiListen));
    s_scan = (WifiView *)big_alloc(sizeof(WifiView));
    s_scan_view = (WifiView *)big_alloc(sizeof(WifiView));
    if (!s_wl || !s_scan || !s_scan_view) halt("no memory for the Wi-Fi page");
    memset(s_scan, 0, sizeof(*s_scan));
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
