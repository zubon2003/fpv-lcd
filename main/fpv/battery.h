/* XIAO ESP32-C5 battery voltage: GPIO26 switches the on-board 1:2 divider
 * (TPS22916) onto GPIO6 / ADC1 channel 5. */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

bool bat_init(void);
/* Battery voltage in mV (16-sample average), or -1 on an ADC error. Below
 * about 1 V there is no battery (running from USB). */
int bat_read_mv(void);
/* Switch the divider off (it draws ~20 uA) and hold it off in deep sleep. */
void bat_park(void);

#ifdef __cplusplus
}
#endif
