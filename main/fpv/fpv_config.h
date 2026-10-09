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
 *   LED                    3V3
 *   VCC                    5V     (J1 open)  or 3V3 with J1 closed
 *   T_IRQ                  -      (not used, the touch is polled)
 *
 * D2/D3 (GPIO25/7) are strapping pins and D6/D7 (GPIO11/12) belong to the
 * ESP-SDR UART bridge, so none of them is used. */
#pragma once

#define FPV_PIN_SCLK 8
#define FPV_PIN_MOSI 10
#define FPV_PIN_MISO 9
#define FPV_PIN_LCD_CS 1
#define FPV_PIN_LCD_DC 0
#define FPV_PIN_LCD_RST 23
#define FPV_PIN_TOUCH_CS 24

#define FPV_LCD_SPI_HZ 40000000
#define FPV_TOUCH_SPI_HZ 1000000
/* 1 or 3: landscape, pins on the left or on the right. */
#define FPV_LCD_ROTATION 1

/* Receiver: the settings the PC monitor uses on the C5. */
#define FPV_SAMPLES 8192        /* per hop */
#define FPV_STEP_MHZ 25         /* LO spacing */
#define FPV_USABLE_MHZ 28.0f    /* kept half-width of each hop */
#define FPV_DC_KHZ 500.0f       /* dropped around each LO */
#define FPV_DEFAULT_GAIN 40     /* gain-table index; 55+ saturates on the C5 */
#define FPV_MARGIN_MHZ 15.0f    /* span beyond the outer channels / 1ch span */

/* Measurement (fpv-band-monitor-c5 defaults). */
#define FPV_AVG_SWEEPS 10       /* measurement = mean of the last N sweeps */
#define FPV_SMOOTH 0.3f         /* EMA on the displayed centre */
#define FPV_DEV_OK_MHZ 0.5f     /* deviation shown blue up to this */
#define FPV_DEV_WARN_MHZ 1.0f   /* red from this; yellow in between */

/* Display. */
#define FPV_BW_MARK_INNER_MHZ 9.0f
#define FPV_BW_MARK_OUTER_MHZ 17.0f
#define FPV_WF_ROW_MS 500       /* one waterfall row */
