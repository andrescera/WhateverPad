#include "web.h"
#include "attrib.h"
#include "log.h"
#include "util.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WEB_CLIENTS     8
#define REQ_MAX         8192
#define T_REQUEST       3000    /* ms for a client to send its request */
#define LOG_TAIL        6144
#define T_WRITE         1000
#define RESPONSE_MAX    (8192 + ATTRIB_JSON_MAX)

extern const unsigned char web_page_html[];
extern const unsigned int web_page_html_len;

enum client_phase { READING, ACTION_PENDING, ACTION_TAKEN, ACTION_RUNNING, WRITING, RETRY_READY };
enum action_kind { PAIR, FORGET, PRESS_PS, PS_COMBO, TUNE, RAW_HCI, RETRY };

typedef struct {
    int fd;
    enum client_phase phase;
    long deadline, write_by;
    uint64_t id, attempt, revision, sequence;
    int len;
    char buf[REQ_MAX + 1];
    enum action_kind action;
    int slot, plen, retry_reserved;
    unsigned opcode;
    long value;
    unsigned char params[255], addr[6];
    char text[100];
    session_retry_ticket retry;
    char head[320], body[RESPONSE_MAX];
    const char *data;
    size_t head_len, head_sent, body_len, body_sent;
} client;

struct web {
    web_cfg cfg;
    int fd;
    int polling, applying;
    client c[WEB_CLIENTS];
    client rejected; /* Response only; never a ninth request/action slot. */
};

static int g_allow_any_peer;            /* tests only */
static uint64_t g_next_id, g_sequence;  /* survive listener rebinds */

static void web_poll_once(web_t *w, long now);

/* ---- helpers ------------------------------------------------------------- */

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void service(web_t *w)
{
    if (w->cfg.service) w->cfg.service(w->cfg.ud);
    if (w->cfg.session && w->cfg.stop && *w->cfg.stop)
        session_request(w->cfg.session, SESSION_STOP, (uint64_t)now_ms());
}

static void close_client(web_t *w, client *c)
{
    if (c->retry_reserved && w->cfg.retry_lock)
        w->cfg.retry_lock(w->cfg.ud, 0);
    c->retry_reserved = 0;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static void respond(client *c, int code, const char *type, const char *body, size_t len)
{
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" :
        code == 403 ? "Forbidden" : code == 404 ? "Not Found" :
        code == 408 ? "Request Timeout" : code == 409 ? "Conflict" :
        code == 429 ? "Too Many Requests" : code == 503 ? "Service Unavailable" : "Error";
    c->head_len = (size_t)snprintf(c->head, sizeof c->head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                     "X-Snapshot-Sequence: %" PRIu64 "\r\n"
                     "Connection: close\r\n\r\n",
                     code, msg, type, len, c->sequence);
    if (body == (const char *)web_page_html) c->data = body;
    else {
        memcpy(c->body, body, len);
        c->data = c->body;
    }
    c->body_len = len;
    c->body_sent = c->head_sent = 0;
    c->write_by = now_ms() + T_WRITE;
    c->phase = WRITING;
}

static void respond_text(client *c, int code, const char *text)
{
    respond(c, code, "application/json", text, strlen(text));
}

/* A bounded nonblocking step; partial writes retain their exact offsets. */
static void write_response(web_t *w, client *c, long now)
{
    for (int i = 0; i < 4; i++) {
        if (now_ms() >= c->write_by) { close_client(w, c); return; }
        int header_left = c->head_sent < c->head_len;
        const char *p = header_left ? c->head + c->head_sent : c->data + c->body_sent;
        size_t n = header_left ? c->head_len - c->head_sent : c->body_len - c->body_sent;
        if (!n) {
            if (c->retry_reserved) c->phase = RETRY_READY;
            else close_client(w, c);
            return;
        }
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t k = send(c->fd, p, n, flags);
        if (k > 0) {
            if (header_left) c->head_sent += (size_t)k;
            else c->body_sent += (size_t)k;
        } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
                   now < c->write_by) {
            return;
        } else {
            close_client(w, c);
            return;
        }
    }
}

/* The value of a header, name compared without regard to case. */
static int header(const char *req, const char *name, char *out, size_t max)
{
    size_t nl = strlen(name);
    const char *p = strstr(req, "\r\n");

    while (p && p[2] != '\r') {
        const char *line = p + 2, *end = strstr(line, "\r\n");
        if (!end) return 0;
        if ((size_t)(end - line) > nl && line[nl] == ':' && strncasecmp(line, name, nl) == 0) {
            const char *v = line + nl + 1;
            size_t k = 0;
            while (*v == ' ') v++;
            while (v < end && k + 1 < max) out[k++] = *v++;
            out[k] = '\0';
            return 1;
        }
        p = end;
    }
    return 0;
}

/* A Host that is an IPv4 address (or localhost), with an optional port: not
 * a name. A web page on the Internet cannot make a browser use one to reach
 * the console (DNS rebinding hands the browser the console's address under
 * the attacker's name, and the name is what arrives here). */
static int host_is_literal(const char *h)
{
    unsigned a, b, c, d;
    char tail;
    int n;

    if (strncmp(h, "localhost", 9) == 0 && (h[9] == '\0' || h[9] == ':')) return 1;
    n = sscanf(h, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail);
    if (n == 4) return a < 256 && b < 256 && c < 256 && d < 256;
    return n == 5 && tail == ':' && a < 256 && b < 256 && c < 256 && d < 256;
}

/* Loopback or a private range: the page is for the local network only. */
static int peer_is_local(const struct sockaddr_in *sin)
{
    uint32_t a = ntohl(sin->sin_addr.s_addr);

    return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == 0xAC1 ||
           (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE;
}

static int parse_addr(const char *s, unsigned char a[6])
{
    unsigned v[6];
    int i;

    if (strlen(s) != 17 ||
        sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return 0;
    for (i = 0; i < 17; i++)
        if (i % 3 == 2 ? s[i] != ':' : !strchr("0123456789abcdefABCDEF", s[i])) return 0;
    for (i = 0; i < 6; i++) a[5 - i] = (unsigned char)v[i];   /* shown MSB first */
    return 1;
}

static void fmt_addr(char *out, const unsigned char *a)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* ---- API --------------------------------------------------------------------- */

/* Copies `s` into out as JSON string content: quotes, backslashes and
 * control characters are dropped, so nothing can break the document. */
static void json_text(char *out, size_t max, const char *s)
{
    size_t n = 0;

    for (; s && *s && n + 1 < max; s++)
        if ((unsigned char)*s >= 0x20 && *s != '"' && *s != '\\') out[n++] = *s;
    out[n] = '\0';
}

static void api_state(web_t *w, client *c)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { PAD_CROSS, "cross" }, { PAD_CIRCLE, "circle" }, { PAD_SQUARE, "square" },
        { PAD_TRIANGLE, "triangle" }, { PAD_L1, "l1" }, { PAD_R1, "r1" }, { PAD_L2, "l2" },
        { PAD_R2, "r2" }, { PAD_L3, "l3" }, { PAD_R3, "r3" }, { PAD_UP, "up" },
        { PAD_DOWN, "down" }, { PAD_LEFT, "left" }, { PAD_RIGHT, "right" },
        { PAD_OPTIONS, "options" }, { PAD_CREATE, "create" }, { PAD_PS, "ps" },
        { PAD_TOUCHPAD, "touchpad" },
    };
    host_t *h = w->cfg.host ? *w->cfg.host : NULL;
    char out[8192 + ATTRIB_JSON_MAX], a[18], combo[100];
    size_t n = 0;
    int i, first = 1;

#define PUT(...) do { int k_ = snprintf(out + n, sizeof out - n, __VA_ARGS__); \
                      if (k_ > 0) n += (size_t)k_ < sizeof out - n ? (size_t)k_ : sizeof out - n - 1; } while (0)

    {
        static const char *const bt[] = { "trying", "ok", "failed" };
        char reason[200], combo[100];
        uint32_t pb = 0;
        int perr = 0, readable = w->cfg.physical ? w->cfg.physical(&pb, &perr) : 0;
        int st = w->cfg.bt_state ? *w->cfg.bt_state : WEB_BT_TRYING;
        size_t b;
        int fb = 1;

        json_text(reason, sizeof reason, w->cfg.bt_reason);
        json_text(combo, sizeof combo, w->cfg.combo_text);
        PUT("{\"sequence\":%" PRIu64 ",\"runtime\":\"%s\",\"version\":\"%s\",\"lang\":\"%s\",\"running\":%s,\"pairing\":%d,\"pairwait\":%d,\"pause\":%d,\"chip\":\"%s\",\"bluetooth\":\"%s\","
            "\"reason\":\"%s\",\"combo\":\"%s\",\"physical\":{\"readable\":%s,\"error\":\"%08x\",\"buttons\":[",
            c->sequence, w->cfg.session ? session_state_name(w->cfg.session) :
                st == WEB_BT_OK ? "active" : st == WEB_BT_FAILED ? "failed" : "starting",
            w->cfg.version ? w->cfg.version : "", w->cfg.lang ? w->cfg.lang : "es", h ? "true" : "false", h ? host_pairing_left(h) : 0,
            h ? host_pair_wait_left(h) : 0, h ? host_pause_left(h) : 0, w->cfg.chip ? w->cfg.chip : "",
            bt[st >= 0 && st <= 2 ? st : 0], reason, combo, readable ? "true" : "false",
            (unsigned)perr);
        for (b = 0; readable && b < sizeof names / sizeof names[0]; b++)
            if (pb & names[b].bit) {
                PUT("%s\"%s\"", fb ? "" : ",", names[b].name);
                fb = 0;
            }
        PUT("]},\"pads\":[");
    }
    for (i = 0; h && i < HOST_MAX_PADS; i++) {
        pad_info info;
        pad_state st;
        size_t b;
        int fb = 1;
        if (!host_pad_get(h, i, &info, &st)) continue;
        fmt_addr(a, info.addr);
        combo[0] = 0;
        if (w->cfg.ps_combo_get) w->cfg.ps_combo_get(i, combo, sizeof combo);
        PUT("%s{\"slot\":%d,\"profile\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"addr\":\"%s\","
            "\"battery\":%d,\"charging\":%s,\"lx\":%u,\"ly\":%u,\"rx\":%u,\"ry\":%u,\"l2\":%u,\"r2\":%u,"
            "\"pscombo\":\"%s\",\"buttons\":[",
            first ? "" : ",", i + 1, info.profile, info.vid, info.pid, a,
            st.has_battery ? st.battery_pct : -1, st.charging ? "true" : "false",
            st.lx, st.ly, st.rx, st.ry, st.l2, st.r2, combo);
        for (b = 0; b < sizeof names / sizeof names[0]; b++)
            if (st.buttons & names[b].bit) {
                PUT("%s\"%s\"", fb ? "" : ",", names[b].name);
                fb = 0;
            }
        PUT("]}");
        first = 0;
    }
    PUT("],\"paired\":[");
    first = 1;
    for (i = 0; h && i < host_paired_count(h); i++) {
        host_paired_info p;
        if (!host_paired_get(h, i, &p)) break;
        fmt_addr(a, p.addr);
        PUT("%s{\"addr\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"le\":%s,\"connected\":%s}",
            first ? "" : ",", a, p.vid, p.pid, p.le ? "true" : "false",
            p.connected ? "true" : "false");
        first = 0;
    }
    PUT("],\"radio_left\":%u,\"attrib\":", host_radio_left(h));
    n += attrib_json(out + n, sizeof out - n - 2);
    PUT("}");
#undef PUT
    respond(c, 200, "application/json", out, n);
}

/* The end of the log, as plain text with anything unprintable dropped. */
static void api_log(web_t *w, client *c)
{
    char buf[LOG_TAIL];
    FILE *f = w->cfg.log_path ? fopen(w->cfg.log_path, "rb") : NULL;
    size_t n = 0, i;
    long size;

    if (!f) {
        respond(c, 200, "text/plain; charset=utf-8", "", 0);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, size > LOG_TAIL ? size - LOG_TAIL : 0, SEEK_SET);
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (i = 0; i < n; i++)
        if ((unsigned char)buf[i] < 0x20 && buf[i] != '\n') buf[i] = ' ';
    respond(c, 200, "text/plain; charset=utf-8", buf, n);
}

/* Dialog mode: the log lines written since line `from` (?from=N), with the
 * number to ask from next time on the first row ("#next N"). */
static void api_live(client *c, unsigned long from)
{
    static char buf[48 * 1024];
    char head[32];
    unsigned long next;
    int k;

    next = log_since(from, buf + 32, sizeof buf - 32);    /* the lines go after room for the first row */
    k = snprintf(head, sizeof head, "#next %lu\n", next);
    memcpy(buf + 32 - k, head, (size_t)k);
    respond(c, 200, "text/plain; charset=utf-8", buf + 32 - k, (size_t)k + strlen(buf + 32));
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Dialog mode: one HCI command, /api/hci?op=0C1B&p=0008 (hex). The reply's
 * return parameters come back as hex; anything else shows in the log. */
static void api_hci(host_t *h, client *c)
{
    unsigned char reply[300];
    char out[700];
    int rlen = 0, i, n;

    if (!host_raw_cmd(h, c->opcode, c->params, c->plen, reply, &rlen)) {
        respond_text(c, 503, "{\"ok\":false,\"error\":\"command unavailable\"}");
        return;
    }
    n = snprintf(out, sizeof out, "{\"ok\":true,\"reply\":\"");
    for (i = 0; i < rlen && n < (int)sizeof out - 8; i++) n += snprintf(out + n, sizeof out - (size_t)n, "%02x", reply[i]);
    n += snprintf(out + n, sizeof out - (size_t)n, "\"}");
    respond(c, 200, "application/json", out, (size_t)n);
}

static int number(const char *s, int base, unsigned long *value)
{
    char *end;
    if (!*s) return 0;
    for (const char *p = s; *p; p++)
        if (base == 16 ? hexval(*p) < 0 : *p < '0' || *p > '9') return 0;
    errno = 0;
    *value = strtoul(s, &end, base);
    return !errno && !*end;
}

static int status(web_t *w, enum session_request_kind kind)
{
    if (w->cfg.session)
        return session_response_status(w->cfg.session, kind, (uint64_t)now_ms(),
                                       w->cfg.retry_lock != NULL);
    if (kind == SESSION_INFO || kind == SESSION_RELEASE) return 200;
    if (kind == SESSION_RETRY)
        return w->cfg.bt_state && *w->cfg.bt_state == WEB_BT_FAILED &&
               w->cfg.retry && !*w->cfg.retry &&
               (!w->cfg.stop || !*w->cfg.stop) ? 200 : 409;
    return (!w->cfg.stop || !*w->cfg.stop) && w->cfg.bt_state &&
           *w->cfg.bt_state == WEB_BT_OK ? 200 : 503;
}

static void unavailable(client *c, int code)
{
    respond_text(c, code, code == 409 ? "{\"error\":\"invalid state\"}" :
                                      "{\"error\":\"unavailable\"}");
}

static void handle(web_t *w, client *c)
{
    char method[8], path[768], protocol[16], hostname[64], custom[8];
    char *query, *line = strstr(c->buf, "\r\n");
    unsigned long value = 0;
    int used = 0;
    c->sequence = ++g_sequence;
    if (!line || sscanf(c->buf, "%7s %767s %15s%n", method, path, protocol, &used) != 3 ||
        c->buf + used != line ||
        (strcmp(protocol, "HTTP/1.1") && strcmp(protocol, "HTTP/1.0"))) {
        respond_text(c, 400, "{\"error\":\"bad request\"}");
        return;
    }
    if (!header(c->buf, "Host", hostname, sizeof hostname) || !host_is_literal(hostname)) {
        respond_text(c, 400, "{\"error\":\"host\"}");
        return;
    }
    query = strchr(path, '?');
    if (query) *query++ = 0;
    if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
        if (query) respond_text(c, 400, "{\"error\":\"query\"}");
        else respond(c, 200, "text/html; charset=utf-8", (const char *)web_page_html, web_page_html_len);
        return;
    }
    if (strncmp(path, "/api/", 5)) {
        respond_text(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    if (!strcmp(method, "POST") &&
        (!header(c->buf, "X-WhateverPad", custom, sizeof custom) || strcmp(custom, "1"))) {
        respond_text(c, 403, "{\"error\":\"forbidden\"}");
        return;
    }
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/api/live")) {
            if (query && (strncmp(query, "from=", 5) || !number(query + 5, 10, &value)))
                respond_text(c, 400, "{\"error\":\"from\"}");
            else api_live(c, value);
        } else if (!strcmp(path, "/api/state") || !strcmp(path, "/api/log") || !strcmp(path, "/api/tune")) {
            if (query) respond_text(c, 400, "{\"error\":\"query\"}");
            else if (!strcmp(path, "/api/state")) api_state(w, c);
            else if (!strcmp(path, "/api/log")) api_log(w, c);
            else {
                char out[256];
                int n = host_tune_json(out, sizeof out);
                respond(c, 200, "application/json", out, (size_t)n);
            }
        } else respond_text(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    if (strcmp(method, "POST")) {
        respond_text(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    if (query && (!strcmp(path, "/api/stop") || !strcmp(path, "/api/pair") ||
                  !strcmp(path, "/api/retry"))) goto invalid;
    if (!strcmp(path, "/api/stop") && !query) {
        log_line("web: stop requested");
        if (w->cfg.stop) *w->cfg.stop = 1;
        if (w->cfg.session) session_request(w->cfg.session, SESSION_STOP, (uint64_t)now_ms());
        if (w->cfg.host && *w->cfg.host)
            own_release(host_ownership(*w->cfg.host), SESSION_STOP, (uint64_t)now_ms());
        respond_text(c, 200, "{\"ok\":true}");
        return;
    }
    if (!strcmp(path, "/api/pair") && !query) c->action = PAIR;
    else if (!strcmp(path, "/api/retry") && !query) c->action = RETRY;
    else if (!strcmp(path, "/api/forget")) {
        if (!query || strncmp(query, "addr=", 5) || !parse_addr(query + 5, c->addr)) goto invalid;
        c->action = FORGET;
    } else if (!strcmp(path, "/api/ps") || !strcmp(path, "/api/pscombo")) {
        char *combo = query ? strstr(query, "&combo=") : NULL;
        int mapping = !strcmp(path, "/api/pscombo");
        if (mapping) {
            if (!combo || !combo[7] || strlen(combo + 7) >= sizeof c->text) goto invalid;
            *combo = 0;
            for (size_t k = 0; combo[7 + k]; k++) {
                char ch = combo[7 + k];
                if (!strchr("abcdefghijklmnopqrstuvwxyz0123456789,+", ch)) goto invalid;
                c->text[k] = ch == ',' ? '+' : ch;
                c->text[k + 1] = 0;
            }
        }
        if (!query || strncmp(query, "slot=", 5) || !number(query + 5, 10, &value) ||
            value < 1 || value > HOST_MAX_PADS) goto invalid;
        c->slot = (int)value - 1;
        c->action = mapping ? PS_COMBO : PRESS_PS;
    } else if (!strcmp(path, "/api/tune")) {
        char *eq = query ? strchr(query, '=') : NULL, *end;
        if (!eq || eq == query || (size_t)(eq - query) >= 32 || !eq[1]) goto invalid;
        *eq = 0;
        for (const char *p = query; *p; p++)
            if (!strchr("abcdefghijklmnopqrstuvwxyz_", *p)) goto invalid;
        errno = 0;
        c->value = strtol(eq + 1, &end, 0);
        if (errno || *end || eq[1] == ' ' || eq[1] == '+') goto invalid;
        strcpy(c->text, query);
        c->action = TUNE;
    } else if (!strcmp(path, "/api/hci")) {
        char *params = query ? strstr(query, "&p=") : NULL;
        if (params) *params = 0;
        if (!query || strncmp(query, "op=", 3) || strlen(query + 3) > 4 ||
            !number(query + 3, 16, &value) || !value || value == 0x0C03 ||
            value == 0x0402 || value == 0x0408 || value == 0x200E) goto invalid;
        c->opcode = (unsigned)value;
        if (params) {
            size_t len = strlen(params + 3);
            if (len % 2 || len > sizeof c->params * 2) goto invalid;
            for (size_t k = 0; k < len; k += 2) {
                int hi = hexval(params[3 + k]), lo = hexval(params[4 + k]);
                if (hi < 0 || lo < 0) goto invalid;
                c->params[c->plen++] = (unsigned char)(hi * 16 + lo);
            }
        }
        c->action = RAW_HCI;
    } else {
        respond_text(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    {
        int code = status(w, c->action == RETRY ? SESSION_RETRY : SESSION_MUTATE);
        if (code != 200) { unavailable(c, code); return; }
    }
    if (c->action == RETRY) {
        for (int i = 0; i < WEB_CLIENTS; i++)
            if (&w->c[i] != c && w->c[i].fd >= 0 && w->c[i].phase != READING &&
                w->c[i].action == RETRY) {
                unavailable(c, 409);
                return;
            }
    }
    if (w->cfg.session) {
        c->attempt = w->cfg.session->attempt;
        c->revision = w->cfg.session->revision;
    }
    c->phase = ACTION_PENDING;
    return;
invalid:
    respond_text(c, 400, "{\"error\":\"invalid query\"}");
}

static int admit(web_t *w, client *c)
{
    int code = status(w, c->action == RETRY ? SESSION_RETRY : SESSION_MUTATE);
    if (now_ms() >= c->deadline) code = 503;
    if (code == 200 && w->cfg.session &&
        (c->attempt != w->cfg.session->attempt || c->revision != w->cfg.session->revision))
        code = 409;
    if (code != 200) { unavailable(c, code); return 0; }
    return 1;
}

int web_take_pending(web_t *w, web_action *action)
{
    if (!w || w->polling || w->applying) return 0;
    service(w);
    client *pending = NULL;
    for (int i = 0; i < WEB_CLIENTS; i++) {
        client *c = &w->c[i];
        if (c->fd < 0) continue;
        if (c->phase == RETRY_READY) {
            if (admit(w, c) && (!w->cfg.session ||
                session_apply_retry(w->cfg.session, &c->retry, (uint64_t)now_ms(), 1))) {
                if (w->cfg.retry) *w->cfg.retry = 1;
                log_line("web: Bluetooth retry requested");
                c->retry_reserved = 0; /* The outer owner now holds the lock. */
            }
            close_client(w, c);
        } else if (c->phase == ACTION_PENDING && admit(w, c) &&
                   (!pending || c->id < pending->id)) pending = c;
    }
    if (!pending) return 0;
    pending->phase = ACTION_TAKEN;
    *action = (web_action){pending->id};
    return 1;
}

void web_apply_pending(web_t *w, web_action action)
{
    if (!w || w->polling || w->applying) return;
    w->applying = 1;
    service(w);
    for (int i = 0; i < WEB_CLIENTS; i++) {
        client *c = &w->c[i];
        if (c->fd < 0 || c->phase != ACTION_TAKEN || c->id != action.id) continue;
        if (!admit(w, c)) break;
        host_t *h = w->cfg.host ? *w->cfg.host : NULL;
        /* Claim before callbacks: reentry cannot apply this action twice. */
        c->phase = ACTION_RUNNING;
        switch (c->action) {
        case PAIR:
            if (!h) unavailable(c, 503);
            else if (!host_pair(h, 60)) {
                log_line("web: pairing refused");
                respond_text(c, 503, "{\"error\":\"pairing refused\"}");
            }
            else {
                log_line("web: pairing started");
                respond_text(c, 200, "{\"ok\":true}");
            }
            break;
        case FORGET:
            if (!h) unavailable(c, 503);
            else if (!host_forget(h, c->addr)) respond_text(c, 404, "{\"error\":\"not paired\"}");
            else respond_text(c, 200, "{\"ok\":true}");
            break;
        case PRESS_PS:
            if (!w->cfg.press_ps) unavailable(c, 503);
            else if (!w->cfg.press_ps(c->slot)) respond_text(c, 404, "{\"error\":\"no pad\"}");
            else respond_text(c, 200, "{\"ok\":true}");
            break;
        case PS_COMBO:
            if (!w->cfg.ps_combo_set) unavailable(c, 503);
            else if (!w->cfg.ps_combo_set(c->slot, c->text)) respond_text(c, 400, "{\"error\":\"combo\"}");
            else respond_text(c, 200, "{\"ok\":true}");
            break;
        case TUNE:
            if (!host_tune_set(h, c->text, c->value))
                respond_text(c, 400, "{\"error\":\"unknown name or bad value\"}");
            else respond_text(c, 200, "{\"ok\":true}");
            break;
        case RAW_HCI:
            if (!h) unavailable(c, 503);
            else api_hci(h, c);
            break;
        case RETRY:
            if (!w->cfg.retry) { unavailable(c, 503); break; }
            if (w->cfg.retry_lock && !w->cfg.retry_lock(w->cfg.ud, 1)) {
                unavailable(c, 409);
                break;
            }
            c->retry_reserved = 1;
            service(w);
            int admitted = admit(w, c);
            if (admitted && w->cfg.session &&
                !session_queue_retry(w->cfg.session, (uint64_t)now_ms(),
                                     (uint64_t)c->deadline, 1, &c->retry)) {
                unavailable(c, 503);
                admitted = 0;
            }
            if (!admitted) {
                if (w->cfg.retry_lock) w->cfg.retry_lock(w->cfg.ud, 0);
                c->retry_reserved = 0;
                break;
            }
            respond_text(c, 200, "{\"ok\":true}");
            break;
        }
        break;
    }
    w->applying = 0;
}

/* ---- serving ------------------------------------------------------------------- */

web_t *web_start(const web_cfg *cfg, int port)
{
    struct sockaddr_in sin;
    web_t *w;
    int one = 1, i;

    w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->cfg = *cfg;
    for (i = 0; i < WEB_CLIENTS; i++) w->c[i].fd = -1;
    w->rejected.fd = -1;

    w->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (w->fd < 0) {
        free(w);
        return NULL;
    }
    setsockopt(w->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(w->fd, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(w->fd, 4) != 0) {
        log_line("web: port %d not available (errno %d), no web page", port, errno);
        close(w->fd);
        free(w);
        return NULL;
    }
    set_nonblock(w->fd);
    log_line("web: listening on port %d", port);
    return w;
}

void web_poll(web_t *w)
{
    if (!w || w->polling) return;
    w->polling = 1;
    service(w);
    web_poll_once(w, now_ms());
    w->polling = 0;
}

void web_run(web_t *w)
{
    web_action action;
    web_poll(w);
    while (web_take_pending(w, &action)) web_apply_pending(w, action);
}

static void web_poll_once(web_t *w, long now)
{
    int i;

    for (int accepted = 0; accepted <= WEB_CLIENTS && w->rejected.fd < 0; accepted++) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(w->fd, (struct sockaddr *)&peer, &plen), slot = -1;
        if (fd < 0) break;
        if (plen < sizeof peer || (!g_allow_any_peer && !peer_is_local(&peer))) {
            close(fd);                  /* not from the local network */
            continue;
        }
#ifdef SO_NOSIGPIPE
        {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        }
#endif
        set_nonblock(fd);
        for (i = 0; i < WEB_CLIENTS; i++) if (w->c[i].fd < 0) { slot = i; break; }
        if (slot < 0) {
            client *c = &w->rejected;
            memset(c, 0, sizeof *c);
            c->fd = fd;
            c->deadline = now + T_REQUEST;
            break;
        }
        memset(&w->c[slot], 0, sizeof w->c[slot]);
        w->c[slot].fd = fd;
        w->c[slot].deadline = now + T_REQUEST;
        w->c[slot].id = ++g_next_id;
    }

    for (i = 0; i <= WEB_CLIENTS; i++) {
        client *c = i == WEB_CLIENTS ? &w->rejected : &w->c[i];
        ssize_t k;

        if (c->fd < 0) continue;
        if (c->phase == ACTION_PENDING || c->phase == ACTION_TAKEN) {
            if (now >= c->deadline) unavailable(c, 503);
            else if (status(w, c->action == RETRY ? SESSION_RETRY : SESSION_MUTATE) != 200)
                unavailable(c, status(w, c->action == RETRY ? SESSION_RETRY : SESSION_MUTATE));
        }
        if (c->phase == WRITING) {
            write_response(w, c, now);
            continue;
        }
        if (c->phase != READING) continue;
        k = recv(c->fd, c->buf + c->len, (size_t)(REQ_MAX - c->len), 0);
        if (k > 0) c->len += (int)k;
        c->buf[c->len] = '\0';

        if (strstr(c->buf, "\r\n\r\n")) {
            if (now >= c->deadline) unavailable(c, 503);
            else if (i == WEB_CLIENTS) {
                c->sequence = ++g_sequence;
                respond_text(c, 429, "{\"error\":\"busy\"}");
            }
            else handle(w, c);
        } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            respond_text(c, 400, "{\"error\":\"incomplete request\"}");
        } else if (c->len >= REQ_MAX) {
            respond_text(c, 400, "{\"error\":\"too large\"}");
        } else if (now <= c->deadline) {
            continue;                   /* still coming */
        } else {
            respond_text(c, 408, "{\"error\":\"request timeout\"}");
        }
        if (c->phase == WRITING) write_response(w, c, now);
    }
}

int web_alive(const web_t *w, int port)
{
    struct sockaddr_in sin;
    int v = 0, fd, ok;
    socklen_t l = sizeof v;

    if (!w || w->fd < 0) return 0;
    if (getsockopt(w->fd, SOL_SOCKET, SO_ACCEPTCONN, &v, &l) == 0 && !v) return 0;   /* answers: not listening */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 1;                       /* cannot ask: do not call it dead */
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = connect(fd, (struct sockaddr *)&sin, sizeof sin) == 0;
    close(fd);
    return ok;
}

void web_stop(web_t *w)
{
    int i;

    if (!w) return;
    for (i = 0; i < WEB_CLIENTS; i++) close_client(w, &w->c[i]);
    close_client(w, &w->rejected);
    close(w->fd);
    free(w);
}
