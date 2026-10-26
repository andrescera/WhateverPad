/* GPL-3.0-or-later. Real product failure handling and HTTP Retry, offline USB. */
#include "../src/host.h"
host_t *fixture_host_prepare(hci_t, const char *, const host_events *);
#if !defined(__FreeBSD__)
#define SYS_thr_set_name 0
#endif
#define main unused_payload_main
#define host_prepare fixture_host_prepare
#include "../src/ps5_main.c"
#undef main
#undef host_prepare
#include "../src/host_int.h"

static void usb_service(void *ledger) { service_bt(ledger); }
static int bind_service(hci_t *io, own_ledger *ledger, void (*idle)(void *), void *ud)
{
    (void)idle; (void)ud;
    return hci_usb_bind_ownership(io, ledger, usb_service, ledger);
}
#define hci_usb_bind_ownership bind_service
#define main integration_fixture_main
#include "test_integration_gen.c"
#undef main
#undef hci_usb_bind_ownership

host_t *fixture_host_prepare(hci_t io, const char *path, const host_events *events)
{
    REQUIRE(!strcmp(path, DB_PATH));
    return host_prepare(io, db, events); /* Only the console's disk path is replaced. */
}

static int http_request(const char *method, const char *path)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(f.port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    char text[256];
    int n = snprintf(text, sizeof text, "%s %s HTTP/1.1\r\n"
        "Host: localhost\r\nX-WhateverPad: 1\r\n\r\n", method, path);
    REQUIRE(sendto(fd, text, (size_t)n, 0, NULL, 0) == n);
    REQUIRE(shutdown(fd, SHUT_WR) == 0);
    return fd;
}

static void reply(int fd, char *out, size_t max, int expected)
{
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    web_run(g_web);
    web_run(g_web);
    size_t bytes = 0;
    for (;;) {
        REQUIRE(poll(&ready, 1, 1000) > 0);
        int n = (int)recv(fd, out + bytes, max - 1 - bytes, 0);
        REQUIRE(n >= 0);
        if (!n) break;
        bytes += (size_t)n;
        REQUIRE(bytes < max - 1);
    }
    out[bytes] = 0;
    int code = 0;
    REQUIRE(sscanf(out, "HTTP/1.1 %d", &code) == 1 && code == expected);
    REQUIRE(close(fd) == 0);
}

int main(int argc, char **argv)
{
    int native = argc == 2 && !strcmp(argv[1], "native");
    if (argc > 2 || (argc == 2 && !native)) return 2;
    signal(SIGPIPE, SIG_IGN);
    snprintf(directory, sizeof directory, "/tmp/runtime-failure-XXXXXX");
    REQUIRE(mkdtemp(directory) != NULL);
    snprintf(db, sizeof db, "%s/bonds", directory);
    g_tune.ps_light = 0; /* Marvell also skips page-scan tuning: no runtime restore here. */
    begin(0);
    if (native) {
        REQUIRE(host_pair(f.host, 60));
        long end = now_ms() + 30000;
        while (!f.connects && host_admits(f.host) && now_ms() < end) host_poll(f.host, 4);
        REQUIRE(f.connects == 1 && host_connected(f.host, 0));
        REQUIRE(host_pair(f.host, 0));
        const unsigned char quiet[] = {0};
        sim_set_report(f.classic, quiet, 0); /* No fresh-input Disconnect lease at takeover. */
        end = now_ms() + T_YIELD + 1;
        while (host_admits(f.host) && now_ms() < end) host_poll(f.host, 20);
        REQUIRE(host_admits(f.host) && !attrib_get()->timeout && !attrib_get()->unanswered);
    }
    web_stop(f.web);
    g_host = f.host;
    g_vpad_ok = 1;
    g_spanish = 0;
    g_bt_state = WEB_BT_OK;
    web_cfg cfg = {.host = &g_host, .stop = &g_web_stop, .retry = &g_retry,
        .bt_state = &g_bt_state, .bt_reason = g_bt_reason};
    g_web = web_start(&cfg, 0);
    REQUIRE(g_web != NULL);

    unsigned long from = log_seq();
    if (native) {
        const unsigned char peer[] = {0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48};
        sim_peer_request(f.classic, peer, 0x002508);
        host_poll(g_host, 4);
    } else {
        /* Lose only the runtime scan reply, after the real USB token commit. */
        REQUIRE(host_pair(g_host, 60));
        const unsigned char enable[] = {1, 0};
        delay_events(0x0e, 1, -1);
        REQUIRE(!hci_sync(g_host, 0x200c, enable, sizeof enable));
    }
    own_summary summary;
    own_snapshot(host_ownership(g_host), &summary);
    REQUIRE(!host_admits(g_host) && !g_host->dead);
    if (native) {
        REQUIRE(summary.runtime.reason == SESSION_NATIVE && !summary.quarantined);
        REQUIRE(!attrib_get()->timeout && !attrib_get()->unanswered);
    } else {
        REQUIRE(summary.quarantined && summary.failed_opcode == 0x200c && summary.reply_timeout);
    }
    printf("FAILURE_REPRO native=%d admission=%d usb_dead=%d lost=%d quarantined=%u\n",
           native, host_admits(g_host), g_host->dead, host_transport_lost(g_host), summary.quarantined);
    REQUIRE(host_transport_lost(g_host));
    char out[20000];
    reply(http_request("GET", "/api/state"), out, sizeof out, 200);
    REQUIRE(g_bt_state == WEB_BT_OK); /* Idle can serve before the outer close. */
    const char *reason = native ? "native_takeover" : "reply_timeout";
    REQUIRE(strstr(out, "\"bluetooth\":\"failed\"") && strstr(out, reason));
    REQUIRE(strstr(out, "\"pairing\":0") && strstr(out, "\"pairwait\":0"));
    REQUIRE(strstr(out, "\"pads\":[]") && strstr(out, "\"running\":false"));
    reply(http_request("POST", "/api/pair"), out, sizeof out, 503);
    if (g_host && host_transport_lost(g_host)) bt_lost();
    f.host = NULL;
    REQUIRE(!g_host && g_bt_state == WEB_BT_FAILED && f.closes == 1);
    if (native) f.controller.ops->pump(f.controller.ctx, T_YIELD + 1);
    reply(http_request("GET", "/api/state"), out, sizeof out, 200);
    REQUIRE(strstr(out, "\"bluetooth\":\"failed\"") != NULL);
    REQUIRE(strstr(out, "\"reason\":\"\"") == NULL && strstr(out, reason));
    log_since(from, out, sizeof out);
    REQUIRE(strstr(out, native ? "reason=native_takeover" : "opcode=0x200c reason=reply_timeout"));
    unsigned long logged = log_seq();
    web_run(g_web);
    REQUIRE(log_seq() == logged); /* A dead session is logged once, not every poll. */

    int fd = http_request("POST", "/api/retry");
    web_poll(g_web); /* Receive only; Retry cannot be armed from idle. */
    REQUIRE(!g_retry);
    reply(fd, out, sizeof out, 200);
    REQUIRE(g_retry == 1); /* Actual web_take_pending RETRY_READY completion. */
    web_run(g_web);
    REQUIRE(g_retry == 1 && f.opens == 1);
    g_retry = 0;
    /* The one-open fixture retains kernel endpoint flags after UNINIT.
     * Verify its complete inventory before discarding those per-fd flags. */
    REQUIRE(!f.initialized && !f.fd_open && f.uninits == 1);
    for (unsigned i = 0; i < XFERS; i++) {
        REQUIRE(!f.busy[i] && f.start_count[i] == f.complete_count[i] + f.stop_count[i]);
        f.open[i] = f.cancelled[i] = 0;
    }
    bt_begin();
    REQUIRE(g_bt_state == WEB_BT_TRYING && !g_bt_reason[0]);
    bt_try(now_ms());
    REQUIRE(g_host && g_bt_state == WEB_BT_OK && f.opens == 2 && f.closes == 1);
    reply(http_request("GET", "/api/state"), out, sizeof out, 200);
    REQUIRE(strstr(out, "\"bluetooth\":\"ok\"") && strstr(out, "\"reason\":\"\""));
    reply(http_request("POST", "/api/pair"), out, sizeof out, 200);
    REQUIRE(host_pairing_left(g_host) > 0);
    reply(http_request("GET", "/api/state"), out, sizeof out, 200);
    REQUIRE(strstr(out, "\"pairing\":60") && strstr(out, "\"pairwait\":0"));
    REQUIRE(strstr(out, "Cache-Control: no-store"));
    f.controller.ops->pump(f.controller.ctx, 1500);
    reply(http_request("GET", "/api/state"), out, sizeof out, 200);
    REQUIRE(strstr(out, "\"pairing\":59"));
    f.controller.ops->pump(f.controller.ctx, 58500);
    reply(http_request("GET", "/api/state"), out, sizeof out, 200);
    REQUIRE(strstr(out, "\"pairing\":0"));
    REQUIRE(host_admits(g_host));
    if (!native) {
        reply(http_request("POST", "/api/pair"), out, sizeof out, 200);
        long end = now_ms() + 30000;
        while (!host_connected(g_host, 0) && host_admits(g_host) && now_ms() < end)
            host_poll(g_host, 4);
        REQUIRE(host_admits(g_host) && host_connected(g_host, 0));
    }

    web_stop(g_web); g_web = NULL;
    host_close(g_host); g_host = NULL;
    REQUIRE(f.closes == 2 && f.uninits == 2);
    REQUIRE(vpad_creds_restore());
    REQUIRE(close(f.logpipe[0]) == 0 && close(f.logpipe[1]) == 0);
    f.controller.ops->close(f.controller.ctx);
    sim_free(f.classic);
    REQUIRE(unlink(db) == 0 || errno == ENOENT);
    char irk[180];
    snprintf(irk, sizeof irk, "%s.irk", db);
    REQUIRE(unlink(irk) == 0 || errno == ENOENT);
    REQUIRE(rmdir(directory) == 0);
    printf("%u runtime failure checks, 0 failures (native=%d)\n", checks, native);
    return 0;
}
