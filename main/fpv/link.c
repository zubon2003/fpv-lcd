#include "link.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "fpv_config.h"

uint16_t fpv_code(float lin) {
    if (!dsp_valid(lin)) return FPV_CODE_NONE;
    float db = dsp_db(lin);
    float c = (db + 200.0f) * 100.0f + 0.5f;
    if (c < 0.0f) return 0;
    if (c > 65534.0f) return 65534;
    return (uint16_t)c;
}

void fpv_encode(const float *lin, int n, uint16_t *out) {
    for (int k = 0; k < n; k++) out[k] = fpv_code(lin[k]);
}

static const char *skip(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* word at s into w (upper-cased), returns the rest */
static const char *word(const char *s, char *w, size_t n) {
    s = skip(s);
    size_t i = 0;
    while (*s && *s != ' ' && *s != '\t') {
        if (i + 1 < n) w[i++] = (char)toupper((unsigned char)*s);
        s++;
    }
    w[i] = 0;
    return s;
}

static bool to_int(const char *w, int *v) {
    char *end;
    long x = strtol(w, &end, 10);
    if (!*w || *end) return false;
    *v = (int)x;
    return true;
}

static fpv_cmd_kind_t bad(fpv_cmd_t *c, const char *why) {
    c->kind = FC_BAD;
    c->err = why;
    return FC_BAD;
}

fpv_cmd_kind_t fpv_parse(const char *line, const char *const *single, int nsingle, fpv_cmd_t *c) {
    memset(c, 0, sizeof(*c));
    char w[24], a[24];
    const char *p = word(line, w, sizeof(w));
    if (!strcmp(w, "FPV?")) return c->kind = FC_HELLO;
    if (strcmp(w, "FPV")) return c->kind = FC_NONE;
    p = word(p, w, sizeof(w));
    if (!strcmp(w, "STATE?")) return c->kind = FC_STATE;
    if (!strcmp(w, "NOLCD")) return c->kind = FC_NOLCD;
    if (!strcmp(w, "LCD")) return c->kind = FC_LCD;
    if (!strcmp(w, "TOUCH?")) return c->kind = FC_TOUCHLOG;
    p = word(p, a, sizeof(a));
    if (!strcmp(w, "WIFICHECK")) {
        if (!strcmp(a, "4CH")) c->ival = 0;           /* values of ui.hpp's Mode */
        else if (!strcmp(a, "3CH")) c->ival = 1;
        else if (!strcmp(a, "4CH2")) c->ival = 3;
        else if (!strcmp(a, "4CH3")) c->ival = 4;
        else return bad(c, "WIFICHECK 4CH|4CH2|4CH3|3CH");
        return c->kind = FC_WIFICHECK;
    }
    if (!strcmp(w, "WIFI")) {
        char m[24];
        word(p, m, sizeof(m));
        c->ms = 2000;
        if (!to_int(a, &c->ival) || !((c->ival >= 1 && c->ival <= 14) || (c->ival >= 36 && c->ival <= 177)) ||
            (m[0] && (!to_int(m, &c->ms) || c->ms < 50 || c->ms > 10000)))
            return bad(c, "WIFI <1..14|36..177> [50..10000 ms]");
        return c->kind = FC_WIFI;
    }
    if (!strcmp(w, "STREAM")) {
        if (!strcmp(a, "ON")) c->ival = 1;
        else if (!strcmp(a, "OFF")) c->ival = 0;
        else return bad(c, "STREAM ON|OFF");
        return c->kind = FC_STREAM;
    }
    if (!strcmp(w, "MODE")) {
        if (!strcmp(a, "4CH")) c->ival = 0;
        else if (!strcmp(a, "3CH")) c->ival = 1;
        else if (!strcmp(a, "1CH")) c->ival = 2;
        else if (!strcmp(a, "4CH2")) c->ival = 3;     /* values of ui.hpp's Mode */
        else if (!strcmp(a, "4CH3")) c->ival = 4;
        else return bad(c, "MODE 4CH|4CH2|4CH3|3CH|1CH");
        return c->kind = FC_MODE;
    }
    if (!strcmp(w, "CH")) {
        for (int i = 0; i < nsingle; i++)
            if (!strcmp(a, single[i])) { c->ival = i; return c->kind = FC_CH; }
        return bad(c, "CH <a channel of the current group>");
    }
    if (!strcmp(w, "GAIN")) {
        if (!strcmp(a, "AGC")) c->ival = -1;
        else if (!to_int(a, &c->ival) || c->ival < 0 || c->ival > FPV_GAIN_MAX) return bad(c, "GAIN 0..60|AGC");
        return c->kind = FC_GAIN;
    }
    if (!strcmp(w, "SNR")) {
        if (!to_int(a, &c->ival) || c->ival < FPV_SNR_MIN_DB || c->ival > FPV_SNR_MAX_DB)
            return bad(c, "SNR 6..30");
        return c->kind = FC_SNR;
    }
    if (!strcmp(w, "SPAN")) {
        char b[24], list[80];
        p = word(p, b, sizeof(b));
        char *e1, *e2;
        c->start = strtof(a, &e1);
        c->stop = strtof(b, &e2);
        if (!*a || *e1 || !*b || *e2) return bad(c, "SPAN <start> <stop> [ch,...]");
        sweep_plan_t plan;
        if (!(c->stop > c->start) || c->start < 4800.0f || c->stop > 6000.0f ||
            sweep_plan(&plan, c->start, c->stop, FPV_STEP_MHZ))
            return bad(c, "span outside 4800..6000 MHz or wider than 175 MHz");
        p = word(p, list, sizeof(list));
        for (char *t = list; *t && c->nch <= FPV_LINK_MAX_CH;) {
            char *comma = strchr(t, ',');
            if (comma) *comma = 0;
            if (*t) {
                if (c->nch == FPV_LINK_MAX_CH) return bad(c, "at most 4 channels");
                if (fpv_channel_parse(t, &c->ch[c->nch])) return bad(c, "unknown channel");
                c->nch++;
            }
            if (!comma) break;
            t = comma + 1;
        }
        return c->kind = FC_SPAN;
    }
    return bad(c, "unknown FPV command");
}
