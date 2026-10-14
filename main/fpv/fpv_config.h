/* Wiring and defaults for the XIAO ESP32-C5 + 2.8" ILI9341/XPT2046 board.
 *
 *   LCD / touch pin        XIAO   GPIO
 *   SCK  + T_CLK           D8     8
 *   SDI(MOSI) + T_DIN      D10    10
 *   T_DO                   D9     9     (LCD SDO left unconnected)
 *   CS                     D0     1
 *   D/C                    D1     0
 *   RESET                  D4     23
 *   T_CS                   D5     24
 *   LED                    D6     11    (backlight, switched off at shutdown)
 *   VCC                    3V3    (J1 closed; the 5V pin is dead on battery)
 *   T_IRQ                  -      (not used, the touch is polled)
 *
 * D2/D3 (GPIO25/7) are strapping pins and stay unused. D6/D7 (GPIO11/12) are
 * the ESP-SDR UART bridge in the stock firmware, which this build disables. */
#pragma once

#if FPV_PANEL_RESTOUCH35
/* 3.5" build: the pins of the XIAO -> Pico-ResTouch adapter board
 * (hardware/xiao_restouch35_adapter: one copper layer, no crossings). */
#define FPV_PIN_SCLK 23              /* D4 -> GP10 LCD_CLK */
#define FPV_PIN_MOSI 0               /* D1 -> GP11 MOSI */
#define FPV_PIN_MISO 1               /* D0 -> GP12 MISO */
#define FPV_PIN_LCD_CS 24            /* D5 -> GP9  LCD_CS */
#define FPV_PIN_LCD_DC 11            /* D6 -> GP8  LCD_DC */
#define FPV_PIN_LCD_RST 9            /* D9 -> GP15 LCD_RST */
#define FPV_PIN_TOUCH_CS 12          /* D7 -> GP16 TP_CS */
#define FPV_PIN_BACKLIGHT 10         /* D10 -> GP13 LCD_BL */
#elif FPV_PANEL_CAPTOUCH35
/* 3.5" capacitive build: the pins of the carrier board
 * (hardware/xiao_captouch35_board): the 15-pin header runs up the left edge
 * and each XIAO pad steps to its pin without crossings. */
#define FPV_PIN_SCLK 9               /* D9 */
#define FPV_PIN_MOSI 10              /* D10 */
#define FPV_PIN_MISO -1              /* not wired: nothing is read back */
#define FPV_PIN_LCD_CS 8             /* D8 */
#define FPV_PIN_LCD_DC 12            /* D7 */
#define FPV_PIN_LCD_RST 11           /* D6 */
#define FPV_PIN_TOUCH_CS -1          /* I2C touch: no chip select */
#define FPV_PIN_TOUCH_SDA 23         /* D4 */
#define FPV_PIN_TOUCH_SCL 0          /* D1 */
#define FPV_PIN_TOUCH_RST 1          /* D0 */
#define FPV_PIN_BACKLIGHT 24         /* D5 */
#else
#define FPV_PIN_SCLK 8
#define FPV_PIN_MOSI 10
#define FPV_PIN_MISO 9
#define FPV_PIN_LCD_CS 1
#define FPV_PIN_LCD_DC 0
#define FPV_PIN_LCD_RST 23
#define FPV_PIN_TOUCH_CS 24
#define FPV_PIN_BACKLIGHT 11         /* -1 if LED is tied to 3V3 */
#endif
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

#if FPV_PANEL_RESTOUCH35
/* Waveshare Pico-ResTouch-LCD-3.5 (480 x 320, ILI9488 + XPT2046) on the
 * adapter board (pins above):
 *
 *   module (Pico pin)          XIAO   GPIO
 *   LCD_DC   GP8  (11)         D6     11
 *   LCD_CS   GP9  (12)         D5     24
 *   LCD_CLK  GP10 (14)         D4     23
 *   MOSI     GP11 (15)         D1     0
 *   MISO     GP12 (16)         D0     1     (touch data)
 *   LCD_BL   GP13 (17)         D10    10
 *   LCD_RST  GP15 (20)         D9     9
 *   TP_CS    GP16 (21)         D7     12
 *   GND      (28, 38)          GND
 *   VSYS     (39)              wire from 5V (USB) or BAT+ (battery)
 *   TP_IRQ GP17, SD_CS GP22    -      (not used; leave the SD slot empty)
 *
 * The shift registers limit the clock: 20 MHz is what others run on ESP32;
 * Waveshare quotes 60 MHz writes on the Pico. */
#define FPV_LCD_SPI_HZ 20000000
#define FPV_LCD_INVERT true          /* this panel needs inversion on */
#define FPV_TCAL_KEY "tcal35"        /* own touch calibration */
#elif FPV_PANEL_CAPTOUCH35
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
 * panel pixels directly, so it needs no calibration. */
#define FPV_LCD_SPI_HZ 40000000
#define FPV_LCD_INVERT true
#define FPV_TOUCH_I2C_HZ 400000
#define FPV_TOUCH_I2C_ADDR 0x38
#define FPV_TCAL_KEY "tcalcap"       /* only if calibrated by hand */
#define FPV_TOUCH_CAL_FIRST_BOOT 0   /* no calibration prompt on first boot */
#else
#define FPV_LCD_SPI_HZ 40000000
#define FPV_TCAL_KEY "tcal"
#endif
#ifndef FPV_TOUCH_CAL_FIRST_BOOT
#define FPV_TOUCH_CAL_FIRST_BOOT 1
#endif
#define FPV_TOUCH_SPI_HZ 1000000
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
#define FPV_SAT_PERMILLE 10     /* "SAT" when a hop has this many saturated samples per 1000 */
#define FPV_BAT_EMPTY_MV 3300   /* battery icon: empty .. full */
#define FPV_BAT_FULL_MV 4150

/* Display. */
#define FPV_BW_MARK_INNER_MHZ 9.0f
#define FPV_BW_MARK_OUTER_MHZ 17.0f
#define FPV_WF_ROW_MS 500       /* one waterfall row */
