#include "config.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *key; const char *shown; uint32_t bit; } k_buttons[] = {
    { "l1", "L1", PAD_L1 }, { "r1", "R1", PAD_R1 }, { "l2", "L2", PAD_L2 }, { "r2", "R2", PAD_R2 },
    { "l3", "L3", PAD_L3 }, { "r3", "R3", PAD_R3 },
    { "cross", "cruz", PAD_CROSS }, { "circle", "círculo", PAD_CIRCLE },
    { "square", "cuadrado", PAD_SQUARE }, { "triangle", "triángulo", PAD_TRIANGLE },
    { "up", "arriba", PAD_UP }, { "down", "abajo", PAD_DOWN },
    { "left", "izquierda", PAD_LEFT }, { "right", "derecha", PAD_RIGHT },
    { "options", "OPTIONS", PAD_OPTIONS }, { "create", "CREATE", PAD_CREATE },
    { "touchpad", "panel táctil", PAD_TOUCHPAD },
};
#define N_BUTTONS (sizeof k_buttons / sizeof k_buttons[0])

void config_defaults(whateverpad_config *c)
{
    c->menu_combo = PAD_OPTIONS | PAD_L2 | PAD_R2;
    c->menu_hold_ms = 1500;
    c->auto_reconnect = 1;
    c->lan_access = 0;
    c->developer_mode = 0;
    c->notifications = 1;
}

static int parse_buttons(const char *text, uint32_t *mask, int least);

int config_parse_combo(const char *text, uint32_t *mask)
{
    return parse_buttons(text, mask, 2);
}

int config_parse_keys(const char *text, uint32_t *mask)
{
    return parse_buttons(text, mask, 1);
}

static int parse_buttons(const char *text, uint32_t *mask, int least)
{
    char buf[96], *tok, *save = NULL;
    uint32_t m = 0;
    int n = 0;

    if (!text || strlen(text) >= sizeof buf) return 0;
    strcpy(buf, text);
    for (tok = strtok_r(buf, "+ ", &save); tok; tok = strtok_r(NULL, "+ ", &save)) {
        size_t i;
        char *p;
        for (p = tok; *p; p++) *p = (char)tolower((unsigned char)*p);
        for (i = 0; i < N_BUTTONS; i++)
            if (strcmp(tok, k_buttons[i].key) == 0) break;
        if (i == N_BUTTONS) return 0;               /* unknown name */
        if (!(m & k_buttons[i].bit)) n++;
        m |= k_buttons[i].bit;
    }
    if (n < least) return 0;
    *mask = m;
    return 1;
}

void config_combo_text(uint32_t mask, char *out, size_t max)
{
    size_t i, n = 0;

    if (!max) return;
    out[0] = '\0';
    for (i = 0; i < N_BUTTONS; i++) {
        int w;
        if (!(mask & k_buttons[i].bit)) continue;
        w = snprintf(out + n, max - n, "%s%s", n ? " + " : "", k_buttons[i].shown);
        if (w < 0 || (size_t)w >= max - n) break;
        n += (size_t)w;
    }
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    return s;
}

static int parse_line(whateverpad_config *c, char *line)
{
    char *eq, *key = trim(line), *val;
    int *boolean = NULL;

    if (!*key || *key == '#') return 0;
    eq = strchr(key, '=');
    if (!eq) return 1;
    *eq = '\0';
    val = trim(eq + 1);
    key = trim(key);
    if (strcmp(key, "menu_combo") == 0) {
        uint32_t m;
        if (config_parse_combo(val, &m)) {
            c->menu_combo = m;
            return 0;
        }
    } else if (strcmp(key, "menu_hold_ms") == 0) {
        char *end;
        long v;
        errno = 0;
        v = strtol(val, &end, 10);
        if (!errno && end != val && !*end && v >= 500 && v <= 5000) {
            c->menu_hold_ms = (int)v;
            return 0;
        }
    } else if (strcmp(key, "auto_reconnect") == 0) boolean = &c->auto_reconnect;
    else if (strcmp(key, "lan_access") == 0) boolean = &c->lan_access;
    else if (strcmp(key, "developer_mode") == 0) boolean = &c->developer_mode;
    else if (strcmp(key, "notifications") == 0) boolean = &c->notifications;
    if (boolean && (val[0] == '0' || val[0] == '1') && !val[1]) {
        *boolean = val[0] - '0';
        return 0;
    }
    return 1;
}

int config_parse(whateverpad_config *c, const char *bytes, size_t len)
{
    size_t pos = 0;
    int bad = 0;

    if (len > CONFIG_MAX_BYTES) return 1;
    while (pos < len) {
        char line[CONFIG_LINE_MAX + 1];
        size_t start = pos, n;
        while (pos < len && bytes[pos] != '\n') pos++;
        n = pos - start;
        if (pos < len) pos++;
        if (n > CONFIG_LINE_MAX || memchr(bytes + start, '\0', n)) {
            bad++;
            continue;
        }
        memcpy(line, bytes + start, n);
        line[n] = '\0';
        bad += parse_line(c, line);
    }
    return bad;
}

int config_load(whateverpad_config *c, const char *path)
{
    char bytes[CONFIG_MAX_BYTES + 1];
    FILE *f = fopen(path, "rb");
    size_t n;
    int bad;

    if (!f) return errno == ENOENT ? 0 : 1;
    n = fread(bytes, 1, sizeof bytes, f);
    bad = ferror(f) != 0;
    if (fclose(f)) bad = 1;
    if (!bad) bad = config_parse(c, bytes, n);
    if (bad) log_line("config %s: %d invalid line(s) or read error, prior settings kept", path, bad);
    return bad;
}

void config_combo_keys(uint32_t mask, char *out, size_t max)
{
    size_t i, n = 0;

    if (!max) return;
    out[0] = '\0';
    for (i = 0; i < N_BUTTONS; i++) {
        int w;
        if (!(mask & k_buttons[i].bit)) continue;
        w = snprintf(out + n, max - n, "%s%s", n ? "+" : "", k_buttons[i].key);
        if (w < 0 || (size_t)w >= max - n) break;
        n += (size_t)w;
    }
}

/* ---- «Configure PS» -------------------------------------------------------- */

static struct { uint16_t vid, pid; uint32_t mask; } g_pscombo[PSCOMBO_MODELS];
static int g_npscombo;

static int count_bits(uint32_t m)
{
    int n = 0;

    for (; m; m &= m - 1) n++;
    return n;
}

int pscombo_load(const char *path)
{
    char line[160];
    FILE *f = fopen(path, "r");

    g_npscombo = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && g_npscombo < PSCOMBO_MODELS) {
        unsigned vid, pid;
        char combo[100];
        uint32_t mask;
        if (sscanf(line, "%4x:%4x %99s", &vid, &pid, combo) != 3) continue;
        if (!config_parse_keys(combo, &mask) || count_bits(mask) > PSCOMBO_MAX_BUTTONS) continue;
        g_pscombo[g_npscombo].vid = (uint16_t)vid;
        g_pscombo[g_npscombo].pid = (uint16_t)pid;
        g_pscombo[g_npscombo].mask = mask;
        g_npscombo++;
    }
    fclose(f);
    return g_npscombo;
}

uint32_t pscombo_get(uint16_t vid, uint16_t pid)
{
    int i;

    for (i = 0; i < g_npscombo; i++)
        if (g_pscombo[i].vid == vid && g_pscombo[i].pid == pid) return g_pscombo[i].mask;
    return 0;
}

int pscombo_set(const char *path, uint16_t vid, uint16_t pid, uint32_t mask)
{
    FILE *f;
    int i;

    if (mask && (count_bits(mask) > PSCOMBO_MAX_BUTTONS || (mask & PAD_PS))) return 0;
    for (i = 0; i < g_npscombo; i++)
        if (g_pscombo[i].vid == vid && g_pscombo[i].pid == pid) break;
    if (i == g_npscombo) {
        if (!mask) return 1;                    /* nothing to remove */
        if (g_npscombo == PSCOMBO_MODELS) return 0;
        g_npscombo++;
    }
    if (mask) {
        g_pscombo[i].vid = vid;
        g_pscombo[i].pid = pid;
        g_pscombo[i].mask = mask;
    } else {
        g_pscombo[i] = g_pscombo[--g_npscombo];
    }
    if (!path) return 1;
    f = fopen(path, "w");
    if (!f) return 0;
    for (i = 0; i < g_npscombo; i++) {
        char keys[100];
        config_combo_keys(g_pscombo[i].mask, keys, sizeof keys);
        fprintf(f, "%04x:%04x %s\n", g_pscombo[i].vid, g_pscombo[i].pid, keys);
    }
    fclose(f);
    return 1;
}

uint32_t pscombo_apply(uint32_t combo, uint32_t buttons)
{
    if (!combo || (buttons & combo) != combo) return buttons;
    return (buttons & ~combo) | PAD_PS;
}
