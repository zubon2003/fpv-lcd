#include "battery.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "fpv_config.h"

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_ch;
static bool s_ok;

bool bat_init(void) {
    if (s_ok) return true;
    gpio_hold_dis(FPV_PIN_BAT_EN);
    gpio_config_t io = {.pin_bit_mask = 1ULL << FPV_PIN_BAT_EN, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&io) != ESP_OK) return false;
    gpio_set_level(FPV_PIN_BAT_EN, 0);

    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(FPV_PIN_BAT_ADC, &unit, &s_ch) != ESP_OK) return false;
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return false;
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_config_channel(s_adc, s_ch, &ccfg) != ESP_OK) return false;
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
    s_ok = true;
    return true;
}

int bat_read_mv(void) {
    if (!s_ok) return -1;
    gpio_set_level(FPV_PIN_BAT_EN, 1);
    esp_rom_delay_us(1000);                    /* let the divider settle */
    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_ch, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3300 / 4095;         /* uncalibrated fallback */
        n++;
    }
    gpio_set_level(FPV_PIN_BAT_EN, 0);
    return n ? 2 * sum / n : -1;               /* 1:2 divider */
}

void bat_park(void) {
    gpio_set_level(FPV_PIN_BAT_EN, 0);
    gpio_hold_en(FPV_PIN_BAT_EN);
}
