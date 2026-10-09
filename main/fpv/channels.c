#include "channels.h"
#include <ctype.h>
#include <stdio.h>

const char FPV_BAND_LETTERS[FPV_NUM_BANDS] = {'A', 'B', 'E', 'F', 'R', 'L'};

static const short BANDS[FPV_NUM_BANDS][FPV_CH_PER_BAND] = {
    {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},  /* A */
    {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},  /* B */
    {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},  /* E */
    {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},  /* F */
    {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},  /* R */
    {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621},  /* L */
};

int fpv_band_freq(int band, int number) {
    if (band < 0 || band >= FPV_NUM_BANDS || number < 1 || number > FPV_CH_PER_BAND)
        return 0;
    return BANDS[band][number - 1];
}

fpv_channel_t fpv_channel(int band, int number) {
    fpv_channel_t c;
    snprintf(c.name, sizeof(c.name), "%c%d", FPV_BAND_LETTERS[band], number);
    c.freq_mhz = fpv_band_freq(band, number);
    return c;
}

int fpv_channel_parse(const char *name, fpv_channel_t *out) {
    if (!name || !name[0] || !name[1] || name[2]) return -1;
    char b = (char)toupper((unsigned char)name[0]);
    int n = name[1] - '0';
    for (int i = 0; i < FPV_NUM_BANDS; i++) {
        if (FPV_BAND_LETTERS[i] == b && n >= 1 && n <= FPV_CH_PER_BAND) {
            *out = fpv_channel(i, n);
            return 0;
        }
    }
    return -1;
}
