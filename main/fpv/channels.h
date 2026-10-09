/* 5.8 GHz analogue FPV channel table (same as fpv-band-monitor-c5). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define FPV_NUM_BANDS 6
#define FPV_CH_PER_BAND 8

typedef struct {
    char name[4];      /* "F3" */
    int freq_mhz;
} fpv_channel_t;

/* Band letters in table order: A B E F R L. */
extern const char FPV_BAND_LETTERS[FPV_NUM_BANDS];
int fpv_band_freq(int band, int number);          /* number 1..8 */
fpv_channel_t fpv_channel(int band, int number);
/* "F3" -> channel; returns 0 on success. */
int fpv_channel_parse(const char *name, fpv_channel_t *out);

#ifdef __cplusplus
}
#endif
