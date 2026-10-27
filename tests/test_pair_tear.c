/* GPL-3.0-or-later. Real startup, Pair, USB tear and product automatic Retry.
 * Reuse the integration fixture; only endpoint identity and incoming pieces differ. */
#define RUNTIME_FAILURE_FIXTURE
#define test_usb_ioctl tear_fixture_ioctl
#include "test_runtime_failure.c"
#undef test_usb_ioctl

static const unsigned char *piece;
static int piece_len;
static int after_empty;
static unsigned supported_heads, supported_tails;

int test_usb_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == USB_FS_COMPLETE && piece_len && !after_empty) {
        for (unsigned i = 0; i < 40; i++) {
            if (!f.busy[i] || f.cancelled[i]) continue;
            REQUIRE(piece_len <= (int)f.ep[i].pLength[0]);
            memcpy(f.ep[i].ppBuffer[0], piece, (size_t)piece_len);
            f.ep[i].pLength[0] = (unsigned)piece_len;
            f.ep[i].aFrames = 1; f.ep[i].status = 0;
            ((struct usb_fs_complete *)arg)->ep_index = (unsigned char)i;
            f.busy[i] = 0; f.complete_count[i]++;
            piece_len = 0;
            return 0;
        }
    }
    int rc = tear_fixture_ioctl(fd, request, arg);
    if (request == USB_FS_COMPLETE && rc < 0 && errno == EAGAIN) after_empty = 0;
    if (!rc && request == USB_GET_DEVICE_DESC) {
        struct usb_device_descriptor *d = arg;
        USETW(d->idVendor, 0x1286); USETW(d->idProduct, 0x2059);
    }
    if (!rc && request == USB_GET_FULL_DESC) {
        struct usb_gen_descriptor *d = arg;
        ((unsigned char *)d->ugd_data)[22] = 64;
    }
    if (!rc && request == USB_FS_COMPLETE) {
        unsigned i = ((struct usb_fs_complete *)arg)->ep_index;
        if (i < 40 && f.ep[i].aFrames) {
            const unsigned char *p = f.ep[i].ppBuffer[0];
            supported_heads += f.ep[i].pLength[0] == 64 &&
                               p[0] == 0x0e && le16(p + 3) == 0x1002;
            supported_tails += f.ep[i].pLength[0] == 6 &&
                               event_len == 70 && event_offset == 70;
        }
    }
    return rc;
}

static void incoming(const unsigned char *p, int n)
{
    REQUIRE(!piece_len);
    REQUIRE(f.transport.ops->pump(f.transport.ctx, 0) >= 0);
    piece = p; piece_len = n;
    REQUIRE(f.transport.ops->pump(f.transport.ctx, 0) >= 0);
    REQUIRE(!piece_len);
}

static void result(unsigned char event[257], int gamepad)
{
    memset(event, 0, 257);
    event[0] = 0x2f; event[1] = 255; event[2] = 1;
    memcpy(event + 3, "\1\2\3\4\5\6", 6);
    event[9] = 1;
    event[11] = gamepad ? 8 : 0;
    event[12] = gamepad ? 5 : 1;
    event[17] = 2; event[18] = 9; event[19] = 'P'; /* complete name, not empty EIR */
}

static void whole(const unsigned char event[257])
{
    for (int off = 0; off < 257; off += 64)
        incoming(event + off, 257 - off < 64 ? 257 - off : 64);
}

static void search(void)
{
    sim_delay_events(f.classic, 0x2f, 100, -1); /* supply the discovery packets below */
    REQUIRE(host_pair(g_host, 60));
    for (unsigned i = 0; i < 32 && (!g_host->inquiring ||
         !own_quiet(host_ownership(g_host))); i++) {
        host_poll(g_host, 0);
        f.controller.ops->pump(f.controller.ctx, 1);
    }
    REQUIRE(g_host->inquiring && own_quiet(host_ownership(g_host)));
}

static void tear(int gamepad, int rearm, int retain)
{
    unsigned char event[257];
    static const unsigned char terminal[] = {1, 1, 0};
    result(event, 0);
    whole(event); whole(event);
    host_poll(g_host, 0);
    if (rearm) {
        result(event, 1); event[17] = 0;
        whole(event); host_poll(g_host, 0);
        REQUIRE(g_host->defer_at);
        f.transport.ops->event_reads(f.transport.ctx, 1);
        g_host->reads_now = 1;
        f.controller.ops->pump(f.controller.ctx, 20001);
        result(event, 0); whole(event); /* queued traffic prevents an idle watchdog query */
    }
    incoming(event, 64);
    f.controller.ops->pump(f.controller.ctx, 101);
    if (rearm) {
        /* First pump sees no completion; increasing event reads then reaps the tear
         * before host_poll calls its now-expired deferred gamepad candidate. */
        piece = terminal; piece_len = sizeof terminal; after_empty = 1;
    } else {
        incoming(terminal, sizeof terminal);
        if (gamepad) { result(event, 1); whole(event); }
    }
    int commands = f.commands;
    host_poll(g_host, 0);
    own_summary s;
    own_snapshot(host_ownership(g_host), &s);
    long until = -1;
    int automatic = host_auto_retry(g_host, &until);
    printf("PAIR-TEAR policy=%d pad=%d links=%u auto=%d commands_after=%d expiry_kept=%d\n",
           WHATEVERPAD_ATTRIB_POLICY, gamepad, s.links, automatic,
           f.commands - commands, until == g_host->pair_until);
    REQUIRE(host_transport_lost(g_host) && !host_admits(g_host));
    REQUIRE(automatic == !retain);
    if (!retain) REQUIRE(until == g_host->pair_until);
    else REQUIRE(g_host->store.count == 1 && g_host->committed.count == 1);
    REQUIRE(!s.links && !g_host->had_link && f.commands == commands);
    REQUIRE(!own_invariant(host_ownership(g_host)));
    host_poll(g_host, 0);
    own_snapshot(host_ownership(g_host), &s);
    REQUIRE(s.ambiguity_count == 1 && !s.links && f.commands == commands);
}

static void restart(void)
{
    long until = g_host->pair_until;
    int opens = f.opens;
    bt_lost(); f.host = NULL;
    REQUIRE(!g_host && g_auto_retry && !g_retry && g_auto_pair_until == until);
    for (unsigned i = 0; i < XFERS; i++) {
        REQUIRE(!f.busy[i] && f.start_count[i] == f.complete_count[i] + f.stop_count[i]);
        f.open[i] = f.cancelled[i] = 0;
    }
    bt_retry();
    REQUIRE(g_bt_state == WEB_BT_TRYING && !g_auto_retry);
    bt_try(now_ms());
    REQUIRE(g_host && host_admits(g_host) && g_bt_state == WEB_BT_OK);
    REQUIRE(f.opens == opens + 1 && g_host->pair_until == until && !g_auto_pair_until);
    REQUIRE(supported_heads >= 2 && supported_tails == supported_heads);
}

static void scenario(int repeat, int gamepad, int rearm, int retain)
{
    g_host = NULL; g_web = NULL;
    g_stop = g_web_stop = g_retry = g_auto_retry = 0;
    g_vpad_ok = 1; g_spanish = 0; g_auto_pair_until = 0;
    memset(g_auto_retry_at, 0, sizeof g_auto_retry_at);
    memset(g_pad_id, 0, sizeof g_pad_id);
    supported_heads = supported_tails = 0;
    snprintf(directory, sizeof directory, "/tmp/pair-tear-XXXXXX");
    REQUIRE(mkdtemp(directory));
    snprintf(db, sizeof db, "%s/bonds", directory);
    g_tune.ps_light = 0;
    begin(0);
    REQUIRE(f.host && host_admits(f.host) && supported_heads == 2 && supported_tails == 2);
    web_stop(f.web); f.web = NULL;
    g_host = f.host; g_bt_state = WEB_BT_OK;
    web_cfg cfg = {.host = &g_host, .stop = &g_web_stop, .retry = &g_retry,
        .bt_state = &g_bt_state, .bt_reason = g_bt_reason};
    g_web = web_start(&cfg, 0);
    REQUIRE(g_web);
    const hci_ops *ops = g_host->hci.ops;
    hci_ops no_signal = *ops;
    no_signal.needs_reopen = NULL;
    g_host->hci.ops = &no_signal;
    host_poll(g_host, 0); /* The signal is optional for the other transports. */
    REQUIRE(host_admits(g_host));
    g_host->hci.ops = ops;
    search();
    if (retain) {
        db_rec saved = {.addr = {1,2,3,4,5,6}, .complete = 1};
        memset(saved.key, 0xa5, sizeof saved.key);
        REQUIRE(bond_store_put(&g_host->store, &saved) == BOND_COMMITTED);
        g_host->committed = g_host->store;
    }
    long until = g_host->pair_until, first = now_ms();
    for (unsigned i = 0; i < (repeat ? 4u : 1u); i++) {
        tear(gamepad, rearm, retain);
        REQUIRE(g_host->pair_until == until && now_ms() - first < 60000);
        if (retain) break; /* A saved bond intentionally disqualifies automatic Retry. */
        if (repeat && i == 3) {
            bt_lost(); f.host = NULL;
            bt_retry();
            REQUIRE(!g_host && !g_auto_retry && !g_auto_pair_until);
            REQUIRE(g_bt_state == WEB_BT_FAILED && f.opens == 4 && f.closes == 4);
        } else {
            restart();
            if (repeat) {
                for (unsigned j = 0; j < 32 && (!g_host->inquiring ||
                     !own_quiet(host_ownership(g_host))); j++) {
                    host_poll(g_host, 0);
                    f.controller.ops->pump(f.controller.ctx, 1);
                }
                REQUIRE(g_host->inquiring && own_quiet(host_ownership(g_host)));
            }
        }
    }
    if (!repeat && !retain) {
        sim_delay_events(f.classic, 0x2f, 0, 0);
        long end = now_ms() + 30000;
        while (host_admits(g_host) && now_ms() < end &&
               (!host_connected(g_host, 0) || !f.inputs)) {
            host_poll(g_host, 4);
            vpad_poll(now_ms());
        }
        REQUIRE(host_admits(g_host) && host_connected(g_host, 0) && f.inputs && vpad_live(0));
    }
    close_fixture(f.opens);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    REQUIRE(log_open("/dev/null"));
    if (argc == 2)
        scenario(!strcmp(argv[1], "repeat"), strcmp(argv[1], "nopad") != 0,
                 !strcmp(argv[1], "rearm") || !strcmp(argv[1], "bond"), !strcmp(argv[1], "bond"));
    else {
        scenario(0, 1, 0, 0); scenario(0, 0, 0, 0); scenario(1, 1, 0, 0);
        scenario(0, 1, 1, 0); scenario(0, 1, 1, 1);
    }
    printf("Pair tear recovery: policy=%d checks=%u failures=0\n",
           WHATEVERPAD_ATTRIB_POLICY, checks);
    log_close();
    return 0;
}
