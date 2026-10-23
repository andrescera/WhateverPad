/* The web page's server against a host with a simulated pad connected. */
#include "sim.h"
#include "../src/host.h"
#include "../src/web.h"
#include "../src/util.h"
#include "../src/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, pending, strict_mode;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void expected_failure(int todo, const char *name, int desired, const char *effect)
{
    printf("%s expected-failing-until-todo-%d %s\n", desired ? "XPASS" : "XFAIL", todo, name);
    if (!desired) {
        pending++;
        if (strict_mode) printf("FAIL %s: %s (todo-%d)\n", name, effect, todo);
    }
}
#define EFFECT(todo, name, c) expected_failure(todo, name, (c), #c)

/* A port per run, so two runs at once do not collide. */
static int g_port;
#define PORT g_port

static host_t *g_host;
static web_t *g_web;
static volatile int g_bt_state = WEB_BT_OK, g_retry;
static char g_reason[200] = "";
static int g_phys_readable = 1;

static int fake_physical(uint32_t *buttons, int *err)
{
    *err = g_phys_readable ? 0 : (int)0xFFFF0003;
    *buttons = PAD_L1 | PAD_R1;
    return g_phys_readable;
}

static int g_ps_calls, g_ps_slot = -1, g_ps_result = 1;

static int fake_press_ps(int slot)
{
    g_ps_calls++;
    g_ps_slot = slot;
    return g_ps_result;
}

static void web_idle(void *ud) { (void)ud; web_poll(g_web); }

static void run(int ms)
{
    long end = sim_now() + ms;
    while (sim_now() < end) {
        host_poll(g_host, 5);
        web_poll(g_web);
    }
}

/* Sends a raw request and serves it; returns the response (status line on). */
static int request(const char *raw, size_t rawlen, char *out, int max)
{
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_STREAM, 0), n = 0;
    struct pollfd ready = {.fd = fd, .events = POLLIN};

    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(PORT);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sin, sizeof sin) != 0) {
        close(fd);
        return -1;
    }
    if (rawlen) CHECK(send(fd, raw, rawlen, 0) == (ssize_t)rawlen);
    CHECK(shutdown(fd, SHUT_WR) == 0);
    web_poll(g_web);                      /* all request bytes or EOF are available */
    web_poll(g_web);                      /* truncated input now exposes the write-half EOF */
    while (n < max - 1) {
        ssize_t k;
        if (poll(&ready, 1, 1000) <= 0) { CHECK(0); break; }
        k = recv(fd, out + n, (size_t)(max - 1 - n), 0);
        if (k > 0) n += (int)k;
        else { CHECK(k == 0 || (k < 0 && errno == ECONNRESET)); break; }
    }
    out[n] = '\0';
    close(fd);
    return n;
}

static int get(const char *method, const char *path, char *out, int max)
{
    char raw[512];
    int n = snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: 192.168.1.50:8095\r\nX-WhateverPad: 1\r\n\r\n",
                     method, path);
    return request(raw, (size_t)n, out, max);
}

static int get_with(const char *head, char *out, int max)
{
    return request(head, strlen(head), out, max);
}

static int complete_response(const char *out, int bytes, int code)
{
    const char *body = strstr(out, "\r\n\r\n");
    const char *length = strstr(out, "\r\nContent-Length: ");
    unsigned n = 0;
    int actual = 0;
    return bytes > 0 && sscanf(out, "HTTP/1.1 %d", &actual) == 1 && actual == code &&
        body && length && sscanf(length, "\r\nContent-Length: %u", &n) == 1 &&
        (size_t)bytes == (size_t)(body + 4 - out) + n;
}

static int matrix_combo_set(int slot, const char *text)
{
    return slot == 0 && !strcmp(text, "none");
}

/* F / T:152-202: use an actual failure, then age the old cleanup budget. */
static void test_failed_startup_reports_after_cleanup_deadline(void)
{
    sim_pad pad = {0};
    sim_t *sim = sim_new(&pad);
    hci_t io = sim_hci(sim);
    volatile int stop = 0;
    char out[2048];
    web_cfg cfg = {.host = &g_host, .stop = &stop, .retry = &g_retry,
        .bt_state = &g_bt_state, .bt_reason = "startup failed", .version = "failure"};
    unlink("build/test_web_failed.db");
    sim_go_deaf(sim);
    g_host = host_open(io, "build/test_web_failed.db", NULL);
    CHECK(!g_host);
    EFFECT(15, "failed-startup-transport-cleanup", sim_resources(sim).closes == 1);
    io.ops->close(io.ctx); /* M's caller must finish the failed attempt. */
    CHECK(sim_resources(sim).closes == 1);
    g_bt_state = WEB_BT_FAILED; g_retry = 0;
    g_web = web_start(&cfg, PORT);
    CHECK(g_web != NULL);
    if (g_web) {
        int bytes = get("GET", "/api/state", out, sizeof out);
        CHECK(complete_response(out, bytes, 200));
        io.ops->pump(io.ctx, 10001);
        bytes = get("GET", "/api/state", out, sizeof out);
        CHECK(complete_response(out, bytes, 200));
        CHECK(strstr(out, "\"bluetooth\":\"failed\"") && strstr(out, "\"reason\":\"startup failed\""));
        bytes = get("POST", "/api/retry", out, sizeof out);
        CHECK(complete_response(out, bytes, 200) && strstr(out, "{\"ok\":true}") && g_retry == 1);
        web_stop(g_web); g_web = NULL;
    }
    sim_free(sim); g_retry = 0; g_bt_state = WEB_BT_OK;
    unlink("build/test_web_failed.db");
}

/* T:236-357: M's expressible states; session-only rows are in task-2.md. */
static void test_routes_reply_completely_in_available_states(void)
{
    static const struct { const char *method, *path; int active, absent; } routes[] = {
        {"GET", "/", 200, 200}, {"GET", "/index.html", 200, 200},
        {"GET", "/api/state", 200, 200}, {"GET", "/api/tune", 200, 200},
        {"GET", "/api/log", 200, 200}, {"GET", "/api/live?from=0", 200, 200},
        {"POST", "/api/hci?op=1009", 200, 404}, {"POST", "/api/hci?op=0C03", 400, 404},
        {"POST", "/api/forget?addr=06:05:04:03:02:01", 404, 404},
        {"POST", "/api/forget?addr=zz", 400, 404},
        {"POST", "/api/ps?slot=1", 200, 200}, {"POST", "/api/ps?slot=9", 400, 400},
        {"POST", "/api/pscombo?slot=1&combo=none", 200, 200},
        {"POST", "/api/pscombo?slot=1", 400, 400},
        {"POST", "/api/tune?trace=0", 200, 200}, {"POST", "/api/tune?bad", 400, 400},
        {"POST", "/api/missing", 404, 404}, {"GET", "/api/missing", 404, 404},
        {"GET", "/missing", 404, 404},
        {"GET", "/api/retry", 404, 404}, {"OPTIONS", "/api/stop", 404, 404},
        {"POST", "/api/pair", 200, 404}
    };
    extern const unsigned int web_page_html_len;
    int capacity = (int)web_page_html_len + 65536;
    char *out = malloc((size_t)capacity);
    CHECK(out != NULL);
    if (!out) return;
    {
        FILE *log = fopen("build/test_web.log", "w");
        CHECK(log != NULL);
        if (log) { fputs("matrix\n", log); fclose(log); }
    }
    for (int state = 0; state < 3; state++) {
        sim_pad pad = {0};
        sim_t *sim = sim_new(&pad);
        hci_t io = sim_hci(sim);
        volatile int stop = 0;
        web_cfg cfg = {.host = &g_host, .stop = &stop, .retry = &g_retry,
            .bt_state = &g_bt_state, .version = "matrix", .press_ps = fake_press_ps,
            .log_path = "build/test_web.log", .ps_combo_set = matrix_combo_set};
        g_host = state == 1 ? host_open(io, "build/test_web_matrix.db", NULL) : NULL;
        if (state == 1) CHECK(g_host != NULL);
        g_bt_state = state == 0 ? WEB_BT_TRYING : state == 1 ? WEB_BT_OK : WEB_BT_FAILED;
        g_web = web_start(&cfg, PORT); CHECK(g_web != NULL);
        if (g_web) {
            for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; i++) {
                int bytes = get(routes[i].method, routes[i].path, out, capacity);
                if (!complete_response(out, bytes, state == 1 ? routes[i].active : routes[i].absent))
                    printf("ROUTE state=%d path=%s bytes=%d header=%.80s\n", state, routes[i].path, bytes, out);
                CHECK(complete_response(out, bytes, state == 1 ? routes[i].active : routes[i].absent));
            }
            int bytes = get("POST", "/api/retry", out, capacity);
            CHECK(complete_response(out, bytes, state == 2 ? 200 : 400));
            CHECK(g_retry == (state == 2));
            g_retry = 0;
            web_stop(g_web); g_web = NULL;
        }
        if (g_host) host_close(g_host); else io.ops->close(io.ctx);
        g_host = NULL;
        CHECK(sim_resources(sim).closes == 1);
        sim_free(sim);
    }
    g_bt_state = WEB_BT_OK;
    g_ps_calls = 0; g_ps_slot = -1;
    free(out);
    unlink("build/test_web_matrix.db");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    static char out[70000];
    volatile int stop = 0;
    sim_pad pad;
    sim_t *sim;
    web_cfg wc;
    int i;

    strict_mode = argc == 2 && !strcmp(argv[1], "--expected-failures");
    if (argc > 2 || (argc == 2 && !strict_mode)) return 2;
    signal(SIGPIPE, SIG_IGN);
    g_port = 20000 + (int)(getpid() % 20000);
    test_failed_startup_reports_after_cleanup_deadline();
    test_routes_reply_completely_in_available_states();
    unlink("build/test_web.db");
    memset(&pad, 0, sizeof pad);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    memset(pad.report, 0, 78);
    pad.report[0] = 0x11;
    pad.report[3] = 0x80; pad.report[4] = 0x80; pad.report[5] = 0x80; pad.report[6] = 0x80;
    pad.report[7] = 0x20 | 0x08;            /* cross, hat centred */
    pad.report[32] = 0x08;
    pad.report_len = 78;
    pad.needs_output = 1;
    sim = sim_new(&pad);
    {
        /* As on the console: the host serves the page while it waits for the chip. */
        static host_events ev;
        ev.idle = web_idle;
        g_host = host_open(sim_hci(sim), "build/test_web.db", &ev);
    }
    memset(&wc, 0, sizeof wc);
    wc.host = &g_host;
    wc.stop = &stop;
    wc.retry = &g_retry;
    wc.bt_state = &g_bt_state;
    wc.bt_reason = g_reason;
    wc.log_path = "build/test_web.log";
    wc.version = "test";
    wc.combo_text = "L2 + R2 + OPTIONS";
    wc.physical = fake_physical;
    wc.press_ps = fake_press_ps;
    g_web = web_start(&wc, PORT);
    CHECK(g_host && g_web);
    if (!g_host || !g_web) return 1;
    {
        FILE *f = fopen("build/test_web.log", "w");
        if (f) { fputs("line one\nline two\n", f); fclose(f); }
    }

    printf("the page\n");
    get("GET", "/", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);
    CHECK(strstr(out, "<title>WhateverPad PS5</title>") != NULL);

    printf("pairing from the page, then the pad in the state\n");
    get("POST", "/api/pair", out, sizeof out);
    CHECK(strstr(out, "{\"ok\":true}") != NULL);
    run(8000);
    get("GET", "/api/state", out, sizeof out);
    CHECK(strstr(out, "\"profile\":\"DualShock 4\"") != NULL);
    CHECK(strstr(out, "\"addr\":\"06:05:04:03:02:01\"") != NULL);
    CHECK(strstr(out, "\"buttons\":[\"cross\"]") != NULL);
    CHECK(strstr(out, "\"battery\":100") != NULL);
    CHECK(strstr(out, "\"connected\":true") != NULL);

    CHECK(strstr(out, "\"bluetooth\":\"ok\"") != NULL);
    CHECK(strstr(out, "\"combo\":\"L2 + R2 + OPTIONS\"") != NULL);
    CHECK(strstr(out, "\"physical\":{\"readable\":true,\"error\":\"00000000\",\"buttons\":[\"l1\",\"r1\"]}") != NULL);

    printf("the user's pad not readable (the system has the input)\n");
    g_phys_readable = 0;
    get("GET", "/api/state", out, sizeof out);
    CHECK(strstr(out, "\"readable\":false,\"error\":\"ffff0003\",\"buttons\":[]") != NULL);
    g_phys_readable = 1;

    printf("no Bluetooth: the page still answers, says why, and can retry\n");
    {
        host_t *saved = g_host;
        g_host = NULL;
        g_bt_state = WEB_BT_FAILED;
        snprintf(g_reason, sizeof g_reason, "no reply \"0x1005\" \\ after 5 tries\n");
        get("GET", "/api/state", out, sizeof out);
        CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);
        CHECK(strstr(out, "\"running\":false") != NULL && strstr(out, "\"bluetooth\":\"failed\"") != NULL);
        CHECK(strstr(out, "\"reason\":\"no reply 0x1005  after 5 tries\"") != NULL);   /* nothing that breaks JSON */
        CHECK(strstr(out, "\"pads\":[]") != NULL && strstr(out, "\"paired\":[]") != NULL);
        CHECK(strstr(out, "\"lang\":\"es\"") != NULL);     /* no language given: Spanish */
        get("POST", "/api/pair", out, sizeof out);                  /* nothing to pair with */
        CHECK(strncmp(out, "HTTP/1.1 404", 12) == 0);
        CHECK(g_retry == 0);
        get("POST", "/api/retry", out, sizeof out);
        CHECK(strstr(out, "{\"ok\":true}") != NULL && g_retry == 1);
        g_retry = 0;
        g_bt_state = WEB_BT_OK;
        get("POST", "/api/retry", out, sizeof out);                 /* not failed: refused */
        CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0 && g_retry == 0);
        g_host = saved;
    }

    printf("the log tail\n");
    get("GET", "/api/log", out, sizeof out);
    CHECK(strstr(out, "line two") != NULL);

    printf("the PS button of a pad\n");
    get("POST", "/api/ps?slot=1", out, sizeof out);
    CHECK(strstr(out, "{\"ok\":true}") != NULL && g_ps_calls == 1 && g_ps_slot == 0);   /* slots are 1-based in the page */
    get("POST", "/api/ps?slot=9", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0 && g_ps_calls == 1);
    get("POST", "/api/ps?slot=0", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0 && g_ps_calls == 1);
    g_ps_result = 0;                                    /* no live virtual pad there */
    get("POST", "/api/ps?slot=2", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 404", 12) == 0 && g_ps_calls == 2);
    g_ps_result = 1;
    get_with("POST /api/ps?slot=1 HTTP/1.1\r\nHost: 10.0.0.1\r\n\r\n", out, sizeof out);   /* no X-WhateverPad header */
    CHECK(strncmp(out, "HTTP/1.1 403", 12) == 0 && g_ps_calls == 2);

    printf("forgetting the pad\n");
    get("POST", "/api/forget?addr=zz", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    get("POST", "/api/forget?addr=06:05:04:03:02:01", out, sizeof out);
    CHECK(strstr(out, "{\"ok\":true}") != NULL);
    run(300);
    CHECK(host_paired_count(g_host) == 0);
    get("GET", "/api/state", out, sizeof out);
    CHECK(strstr(out, "\"pads\":[]") != NULL && strstr(out, "\"paired\":[]") != NULL);

    printf("bad requests: unknown, garbage, oversized, silent, many at once\n");
    get("GET", "/nothing", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 404", 12) == 0);
    {
        static const char junk[] = "\x01\x02\x03 garbage\r\n\r\n";
        request(junk, sizeof junk - 1, out, sizeof out);
    }
    CHECK(strncmp(out, "HTTP/1.1 4", 10) == 0);
    {
        static char big[10000];
        memset(big, 'A', sizeof big);
        request(big, sizeof big, out, sizeof out);
        CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    }
    for (i = 0; i < 10; i++) request("GET / HTTP/1.1\r\n", 16, out, sizeof out);  /* never finished */
    get("GET", "/", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);         /* still serving */

    printf("defences that need no PIN\n");
    /* a POST without the page's own header, as a forged cross-site form would send */
    get_with("POST /api/stop HTTP/1.1\r\nHost: 192.168.1.50:8095\r\n\r\n", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 403", 12) == 0 && stop == 0);
    /* DNS rebinding: the browser's Host is the attacker's name */
    get_with("GET /api/state HTTP/1.1\r\nHost: evil.example.com\r\nX-WhateverPad: 1\r\n\r\n", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    get_with("GET / HTTP/1.1\r\n\r\n", out, sizeof out);          /* no Host at all */
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    get_with("OPTIONS /api/stop HTTP/1.1\r\nHost: 192.168.1.50\r\nOrigin: http://evil.example.com\r\n"
             "Access-Control-Request-Method: POST\r\n\r\n", out, sizeof out);
    CHECK(strstr(out, "Access-Control-Allow") == NULL);     /* a preflight is never agreed to */
    CHECK(stop == 0);

    printf("dialog mode: live log, settings and raw commands\n");
    log_line("dialog-test marker");
    get("GET", "/api/live?from=0", out, sizeof out);
    CHECK(strstr(out, "#next ") != NULL && strstr(out, "dialog-test marker") != NULL);
    {
        unsigned long nx = strtoul(strstr(out, "#next ") + 6, NULL, 10);
        char path[64];
        snprintf(path, sizeof path, "/api/live?from=%lu", nx);
        get("GET", path, out, sizeof out);
        CHECK(strstr(out, "dialog-test marker") == NULL);   /* only what came after */
    }
    get("POST", "/api/tune?inq_len=4", out, sizeof out);
    CHECK(strstr(out, "\"ok\":true") != NULL);
    get("GET", "/api/tune", out, sizeof out);
    CHECK(strstr(out, "\"inq_len\":4") != NULL);
    get("POST", "/api/tune?inq_len=999", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    get("POST", "/api/tune?inq_len=8", out, sizeof out);
    get("POST", "/api/hci?op=0C1B", out, sizeof out);
    CHECK(strstr(out, "\"reply\":\"0000081200\"") != NULL);   /* status 0, 0x0800, 0x0012 */
    get("POST", "/api/hci?op=0C03", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);              /* never a Reset */
    get_with("POST /api/tune?trace=1 HTTP/1.1\r\nHost: 192.168.1.50:8095\r\n\r\n", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 403", 12) == 0);              /* changes need the page's header */

    printf("random and mutated requests (under the sanitizers when run with make fuzz)\n");
    {
        static const char *seeds[] = {
            "GET /api/state HTTP/1.1\r\nHost: 10.0.0.1\r\nX-WhateverPad: 1\r\n\r\n",
            "POST /api/forget?addr=06:05:04:03:02:01 HTTP/1.1\r\nHost: 10.0.0.1\r\nX-WhateverPad: 1\r\n\r\n",
            "GET /api/log HTTP/1.1\r\nHost: localhost:8095\r\n\r\n",
        };
        unsigned seed = 99;
        int round;
        for (round = 0; round < 600; round++) {
            char req[400];
            size_t n;
            int k, m;
            seed = seed * 1103515245u + 12345u;
            snprintf(req, sizeof req, "%s", seeds[(seed >> 8) % 3]);
            n = strlen(req);
            m = (int)((seed >> 12) % 6);
            for (k = 0; k < m; k++) {
                seed = seed * 1103515245u + 12345u;
                req[(seed >> 8) % n] = (char)(seed >> 20);
            }
            if (round % 5 == 0) n = (seed >> 6) % n;            /* truncated */
            request(req, n, out, sizeof out);
        }
        get("GET", "/api/state", out, sizeof out);
        CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);           /* still alive and well */
    }

    printf("the page is served again after the listener is lost\n");
    CHECK(web_alive(g_web, PORT));
    CHECK(!web_alive(NULL, PORT));
    web_stop(g_web);                                    /* the listener goes away... */
    g_web = NULL;
    g_web = web_start(&wc, PORT);                       /* ...and comes back on the same port */
    CHECK(g_web != NULL && web_alive(g_web, PORT));
    run(200);
    get("GET", "/api/state", out, sizeof out);
    CHECK(strstr(out, "\"version\":\"test\"") != NULL);

    printf("stop from the page\n");
    get("POST", "/api/stop", out, sizeof out);
    CHECK(stop == 1);

    host_close(g_host);
    web_stop(g_web);
    sim_free(sim);
    g_host = NULL; g_web = NULL; ph_clock_hook = NULL;
    printf(g_fail ? "%d check(s) failed\n" : "all web checks passed\n", g_fail);
    printf("%d expected-failing web cases\n", pending);
    return g_fail != 0 || (strict_mode && pending != 0);
}
