#pragma once
#include "sdkconfig.h"

/* Receiver and capture must be stopped. Measure fresh RX DC at mhz and rebuild
 * the gain tables while retaining the IQ correction established at startup.
 * Retuning must not start a transmitting loopback IQ calibration. Caller restores
 * channel, filter, RX power and gain settings afterwards. Stock startup and
 * background PHY calibration remain outside this receive-only retune path. */
void rx_recalibrate(unsigned mhz);

#include <stdbool.h>
/* True if the requested-LO DC calibration is missing or has been replaced.
 * Check at capture preparation, including when the requested LO is unchanged.
 * Recovery is deferred until capture has stopped; this is not a capture lock. */
bool rx_recalibration_stale(void);

#if FPV_LCD_MONITOR && CONFIG_IDF_TARGET_ESP32C5
/* Like rx_recalibrate, but reuses the DC measurement last made at the same
 * LO while the PHY has not replaced DC calibration in between (and it is
 * younger than RX_DC_CACHE_MAX_AGE_US). Returns true on a cache hit. */
bool rx_recalibrate_cached(unsigned mhz);
/* Runtime switch for A/B checks (CALCACHE 0|1); on by default. */
extern bool rx_dc_cache_enabled;
#define RX_DC_SNAPSHOT_BYTES 324
void rx_dc_snapshot(unsigned char *buf);
void rx_dc_restore_locked(unsigned mhz, const unsigned char *buf);
#endif

/* H2 only: call once during boot, before exposing receive commands. Its PHY
 * needs separate startup IQ corrections for each RF gain group. */
void rx_h2_calibrate_iq_at_boot(void);
