/* Wiring and defaults for the XIAO ESP32-C5 + Waveshare 3.5inch Capacitive
 * Touch LCD (ST7796S + FT6336U); the cable's pin order is further down.
 *
 * D2/D3 (GPIO25/7) are strapping pins and stay unused. D6/D7 (GPIO11/12) are
 * the ESP-SDR UART bridge in the stock firmware, which this build disables. */
#pragma once

/* The pins of the carrier board (hardware/captouch35_ffc). */
#define FPV_PIN_SCLK 9               /* D9 */
#define FPV_PIN_MOSI 10              /* D10 */
#define FPV_PIN_MISO -1              /* not wired: nothing is read back */
#define FPV_PIN_LCD_CS 8             /* D8 */
#define FPV_PIN_LCD_DC 12            /* D7 */
#define FPV_PIN_LCD_RST 11           /* D6 */
#define FPV_PIN_TOUCH_SDA 23         /* D4 */
#define FPV_PIN_TOUCH_SCL 0          /* D1 */
#define FPV_PIN_TOUCH_RST 1          /* D0 */
#define FPV_PIN_BACKLIGHT 24         /* D5 */
#define FPV_BACKLIGHT_ON 1           /* level that lights it */

/* Battery (XIAO ESP32-C5 on-board divider). */
#define FPV_PIN_BAT_ADC 6
#define FPV_PIN_BAT_EN 26
#define FPV_BAT_CUTOFF_MV 3300       /* shut down below this, under load; 0 = never */
#define FPV_BAT_LOW_READS 3          /* consecutive low readings before shutting down */
#define FPV_BAT_CHECK_MS 2000
#define FPV_BAT_BOOT_MV 3400         /* power-on / reset needs at least this */
#define FPV_BAT_RESUME_MV 3700       /* after a low-battery shutdown: wait until charged to this */
#define FPV_BAT_WAKE_S 60            /* while shut down, check the battery this often */
#define FPV_BAT_ABSENT_MV 1000       /* below: no battery, running from USB */

/* Waveshare 3.5inch Capacitive Touch LCD (SKU 29318; 320 x 480 IPS, ST7796S
 * on SPI + FT6336U on I2C, level shifters on board), "Interface 2" 15-pin
 * cable, in the module's pin order (labels as printed on the cable; checked
 * on the real module 2026-10-14):
 *
 *   pin  module     XIAO   GPIO
 *    1   VCC        3V3          (both VCC and 3V3: logic level = supply)
 *    2   3V3        3V3
 *    3   GND        GND
 *    4   MISO       -            (not used)
 *    5   MOSI       D10    10
 *    6   SCLK       D9     9
 *    7   SD_CS      -            (leave the TF slot empty)
 *    8   LCD_CS     D8     8
 *    9   LCD_DC     D7     12
 *   10   LCD_RST    D6     11
 *   11   LCD_BL     D5     24    (high = on)
 *   12   TP_SDA     D4     23
 *   13   TP_SCL     D1     0
 *   14   INT        -            (touch interrupt; not used, the touch is polled)
 *   15   RST        D0     1     (touch reset)
 *
 * Waveshare's demo runs the panel with inversion on (IPS). The touch reports
 * panel pixels directly. */
#define FPV_LCD_SPI_HZ 40000000
#define FPV_LCD_INVERT true
#define FPV_TOUCH_I2C_HZ 400000
#define FPV_TOUCH_I2C_ADDR 0x38
/* 1 or 3: landscape, pins on the left or on the right. */
#define FPV_LCD_ROTATION 1

/* Receiver: the settings the PC monitor uses on the C5. */
#define FPV_SAMPLES 8192        /* per hop */
#define FPV_STEP_MHZ 25         /* LO spacing */
#define FPV_USABLE_MHZ 28.0f    /* kept half-width of each hop */
#define FPV_DC_KHZ 500.0f       /* dropped around each LO */
#define FPV_DEFAULT_GAIN 25     /* gain-table index; 55+ saturates on the C5 */
#define FPV_GAIN_MAX 60         /* setup screen: one step past this = AGC */
#define FPV_MARGIN_MHZ 15.0f    /* span beyond the outer channels / 1ch span */

/* Measurement (fpv-band-monitor-c5 defaults). */
#define FPV_AVG_SWEEPS 10       /* measurement = mean of the last N sweeps */
#define FPV_DEFAULT_SNR_DB 18   /* signal present: peak this far above the floor */
#define FPV_SNR_MIN_DB 6
#define FPV_SNR_MAX_DB 30
#define FPV_SMOOTH 0.3f         /* EMA on the displayed centre */
#define FPV_DEV_OK_MHZ 0.5f     /* deviation shown blue up to this */
#define FPV_DEV_WARN_MHZ 1.0f   /* red from this; yellow in between */
#define FPV_WIFI_RSSI_STRONG -50 /* Wi-Fi page: full colour from this (dBm) ... */
#define FPV_WIFI_RSSI_WEAK -85   /* ... faint at and below this */
#define FPV_WIFI_DWELL_MS 1200  /* Wi-Fi page: listening time per channel (beacons every ~100 ms) */
#define FPV_GROUP_ADJ_MHZ 20    /* Wi-Fi page: warn when two of the group's channels are this close
                                  * (1st order, inclusive) ... */
#define FPV_GROUP_IMD_MHZ 15    /* ... or a 2 x A - B / A + B - C product of them is this close to
                                  * another (3rd order, inclusive) */
#define FPV_SAT_PERMILLE 10     /* "SAT" when a hop has this many saturated samples per 1000 */
#define FPV_BAT_EMPTY_MV 3300   /* battery icon: empty .. full */
#define FPV_BAT_FULL_MV 4150

/* Display. */
#define FPV_BW_MARK_INNER_MHZ 9.0f
#define FPV_BW_MARK_OUTER_MHZ 17.0f
#define FPV_WF_ROW_MS 500       /* one waterfall row */
