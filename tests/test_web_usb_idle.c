/* Real product idle callback, host and USB precommit service; only OS/controller
 * boundaries come from the integration fixture. The payload main is not run. */
#define main unused_payload_main
#include "../src/ps5_main.c"
#undef main
#include "../src/host_int.h"

static void usb_service(void *ledger);
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

static int exercising, precommit_seen;
static unsigned pair_calls;
int __real_host_pair(host_t *host, int seconds);
int __wrap_host_pair(host_t *host, int seconds)
{
    pair_calls++;
    return __real_host_pair(host, seconds);
}

static void usb_service(void *ledger)
{
    if (exercising && f.host->submitting && !f.host->sync.serial) precommit_seen++;
    service_bt(ledger); /* The actual product path through on_idle. */
}

static int post_pair(void)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(f.port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    const char request[] = "POST /api/pair HTTP/1.1\r\n"
        "Host: localhost\r\nX-WhateverPad: 1\r\n\r\n";
    REQUIRE(sendto(fd, request, sizeof request - 1, 0, NULL, 0) == sizeof request - 1);
    REQUIRE(shutdown(fd, SHUT_WR) == 0);
    return fd;
}

static void pair_reply(int fd, int code)
{
    unsigned before = pair_calls;
    web_run(g_web);
    web_run(g_web);
    char reply[2048];
    int bytes = 0, actual = 0;
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    for (;;) {
        REQUIRE(poll(&ready, 1, 1000) > 0);
        int n = (int)recv(fd, reply + bytes, sizeof reply - 1 - (size_t)bytes, 0);
        REQUIRE(n >= 0);
        if (!n) break;
        bytes += n;
        REQUIRE(bytes < (int)sizeof reply - 1);
    }
    reply[bytes] = 0;
    REQUIRE(sscanf(reply, "HTTP/1.1 %d", &actual) == 1 && actual == code);
    REQUIRE(strstr(reply, code == 200 ? "{\"ok\":true}" : "{\"error\":") != NULL);
    REQUIRE(pair_calls == before + (code == 200));
    REQUIRE((host_pairing_left(g_host) > 0) == (code == 200));
    f.controller.ops->pump(f.controller.ctx, 1);
    web_run(g_web);
    REQUIRE(pair_calls == before + (code == 200)); /* A consumed Pair never repeats. */
    REQUIRE(close(fd) == 0);
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "inside") && strcmp(argv[1], "outside")))
        return 2;
    signal(SIGPIPE, SIG_IGN);
    snprintf(directory, sizeof directory, "/tmp/web-usb-idle-XXXXXX");
    REQUIRE(mkdtemp(directory) != NULL);
    snprintf(db, sizeof db, "%s/bonds", directory);
    begin(0);
    g_host = f.host; g_web = f.web;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(f.port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    const char request[] = "POST /api/hci?op=0C1B HTTP/1.1\r\n"
        "Host: localhost\r\nX-WhateverPad: 1\r\n\r\n";
    REQUIRE(sendto(fd, request, sizeof request - 1, 0, NULL, 0) == sizeof request - 1);
    REQUIRE(shutdown(fd, SHUT_WR) == 0);
    long before = now_ms();
    int commands = f.commands;
    exercising = 1;
    int ok = hci_sync(f.host, 0x0c19, NULL, 0);
    exercising = 0;
    own_summary summary;
    own_snapshot(host_ownership(f.host), &summary);
    printf("USB_IDLE outer_ok=%d elapsed_ms=%ld state=%s precommit=%d commands=%d\n",
           ok, now_ms() - before, session_state_name(&summary.runtime),
           precommit_seen, f.commands - commands);
    REQUIRE(precommit_seen > 0);
    REQUIRE(ok && now_ms() == before);
    REQUIRE(summary.runtime.state == SESSION_ACTIVE && !summary.runtime.cleanup_pending);
    REQUIRE(f.commands == commands + 1); /* Only the outer command ran. */
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    REQUIRE(poll(&ready, 1, 0) == 0); /* Idle read/queued it, without applying. */
    web_run(g_web);
    REQUIRE(f.commands == commands + 2); /* Safe outer drain applies it now. */
    web_run(g_web);
    REQUIRE(poll(&ready, 1, 0) > 0);
    char reply[2048];
    int n = (int)recv(fd, reply, sizeof reply - 1, 0);
    REQUIRE(n > 0);
    reply[n] = 0;
    REQUIRE(strstr(reply, "HTTP/1.1 200 OK") && strstr(reply, "\"reply\":\"0000081200\""));
    REQUIRE(close(fd) == 0);

    REQUIRE(host_pair(f.host, 60));
    long end = now_ms() + 30000;
    while (!f.connects && now_ms() < end) host_poll(f.host, 4);
    REQUIRE(f.connects == 1);
    REQUIRE(host_pair(f.host, 0));
    REQUIRE(host_pairing_left(f.host) == 0 && host_pair_wait_left(f.host) == 0);
    fd = post_pair();
    unsigned calls = pair_calls;
    before = now_ms();
    if (argc == 1) {
        sim_set_page_scan(f.classic, 0x0800, 0x0400);
        f.host->ps_check_at = 0;
        int lightened = f.host->ps_lightened;
        delay_events(0x0e, 2, 1500);
        exercising = 1;
        host_poll(f.host, 4); /* Real idle queues Pair during two valid slow replies. */
        exercising = 0;
        REQUIRE(now_ms() - before == 3004);
        REQUIRE(f.host->ps_lightened == lightened + 1);
    } else {
        on_idle(NULL); /* Real product polling admits but never applies the request. */
        int inside = !strcmp(argv[1], "inside");
        f.controller.ops->pump(f.controller.ctx, WEB_REQUEST_MS + (inside ? -1 : 1));
    }
    own_snapshot(host_ownership(f.host), &summary);
    REQUIRE(summary.runtime.state == SESSION_ACTIVE && !summary.runtime.cleanup_pending);
    REQUIRE(pair_calls == calls);
    printf("PAIR_WAIT mode=%s age_ms=%ld budget_ms=%d\n",
           argc == 1 ? "slow-replies" : argv[1], now_ms() - before, WEB_REQUEST_MS);
    pair_reply(fd, argc == 1 || !strcmp(argv[1], "inside") ? 200 : 503);

    g_host = NULL; g_web = NULL;
    web_stop(f.web);
    host_close(f.host);
    REQUIRE(f.closes == 1 && f.uninits == 1);
    REQUIRE(vpad_creds_restore());
    REQUIRE(close(f.logpipe[0]) == 0 && close(f.logpipe[1]) == 0);
    f.controller.ops->close(f.controller.ctx);
    sim_free(f.classic);
    REQUIRE(unlink(db) == 0 || errno == ENOENT);
    char irk[180];
    snprintf(irk, sizeof irk, "%s.irk", db);
    REQUIRE(unlink(irk) == 0 || errno == ENOENT);
    REQUIRE(rmdir(directory) == 0);
    printf("%u USB idle checks, 0 failures\n", checks);
    return 0;
}
