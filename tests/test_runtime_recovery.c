/* GPL-3.0-or-later. Runtime recovery through real USB, product and web code. */
#define RUNTIME_FAILURE_FIXTURE
#include "test_runtime_failure.c"

#ifdef TEST_RECOVERY_RED
/* Baseline adapter: the unchanged menu Retry block in ps5_main's main loop. */
static void bt_retry(void)
{
    if (g_retry) {
        g_retry = 0;
        if (g_vpad_ok && g_bt_state == WEB_BT_FAILED) {
            log_line("retrying Bluetooth at the user's request");
            bt_begin();
        } else {
            log_line("retry ignored (virtual pads not available, or not failed)");
        }
    }
}
#endif

static void recovery_begin(unsigned seed)
{
    g_host = NULL; g_web = NULL;
    g_stop = g_web_stop = g_retry = 0;
    g_vpad_ok = 1; g_spanish = 0;
    g_bt_attempt = g_bt_plain_creds = 0; g_bt_next = 0;
    g_bt_reason[0] = 0;
    memset(g_pad_id, 0, sizeof g_pad_id);
#ifndef TEST_RECOVERY_RED
    g_auto_retry = 0;
    g_auto_pair_until = 0;
#endif
    snprintf(directory, sizeof directory, "/dev/shm/runtime-recovery-XXXXXX");
    REQUIRE(mkdtemp(directory) != NULL);
    snprintf(db, sizeof db, "%s/bonds", directory);
    g_tune.ps_light = 0;
    begin(seed); /* Only DS4/8BitDo scenarios: shared close_fixture is Classic. */
    REQUIRE(f.host && host_admits(f.host));
    web_stop(f.web); f.web = NULL;
    g_host = f.host;
    g_bt_state = WEB_BT_OK;
    web_cfg cfg = {.host = &g_host, .stop = &g_web_stop, .retry = &g_retry,
        .bt_state = &g_bt_state, .bt_reason = g_bt_reason};
    g_web = web_start(&cfg, 0);
    REQUIRE(g_web != NULL);
    /* The simulator clock is process-wide; age the private rolling budget
     * between independent cases, without resetting product history. */
    f.controller.ops->pump(f.controller.ctx, 60001);
}

static void recovery_end(void)
{
    int opens = f.opens;
    close_fixture(opens);
    f.host = NULL;
    REQUIRE(f.opens == opens && f.closes == opens && f.uninits == opens);
    REQUIRE(!f.fd_open && !f.initialized);
    for (unsigned i = 0; i < XFERS; i++)
        REQUIRE(!f.busy[i] && f.start_count[i] == f.complete_count[i] + f.stop_count[i]);
    for (int i = 0; i < f.ndevices; i++)
        REQUIRE(f.devices[i].acquired && f.devices[i].identified && f.devices[i].deletes == 1);
    REQUIRE(f.authid == 0x4800000000000001ull);
    static const unsigned char caps[16] = {0};
    REQUIRE(!memcmp(f.caps, caps, sizeof caps));
}

#ifndef TEST_RECOVERY_RED
static unsigned fault_opcode, fault_sends;
static int fault_command;
static int (*controller_command)(void *, unsigned, const void *, int);
static hci_ops fault_ops;

/* The USB token has already committed. Losing the command here is distinct
 * from executing it and losing its reply; startup never sees either fault. */
static int recovery_command(void *ctx, unsigned op, const void *p, int n)
{
    if (op == fault_opcode) {
        fault_sends++;
        if (fault_command) return 1;
        delay_events(op == 0x0401 ? 0x0f : 0x0e, 1, -1);
    }
    return controller_command(ctx, op, p, n);
}

static void usb_finalized(void)
{
    REQUIRE(!f.initialized && !f.fd_open && f.closes == f.opens && f.uninits == f.opens);
    for (unsigned i = 0; i < XFERS; i++) {
        REQUIRE(!f.busy[i] && f.start_count[i] == f.complete_count[i] + f.stop_count[i]);
        /* Same per-fd fixture reset as test_runtime_failure, after inventory. */
        f.open[i] = f.cancelled[i] = 0;
    }
}

static void runtime_loss(unsigned op, int command_lost)
{
    REQUIRE(g_host && host_admits(g_host) && !g_host->dead);
    if (op) {
        const unsigned char scan[] = {(unsigned char)!g_host->le_scan_want, 0};
        const unsigned char inquiry[] = {0x33, 0x8b, 0x9e, 8, 0};
        const void *p = op == 0x200c ? scan : op == 0x0401 ? inquiry : NULL;
        int n = op == 0x200c ? 2 : op == 0x0401 ? 5 : 0;
        fault_ops = *f.controller.ops;
        controller_command = f.controller.ops->cmd;
        fault_ops.cmd = recovery_command;
        const hci_ops *original = f.controller.ops;
        f.controller.ops = &fault_ops;
        fault_opcode = op; fault_sends = 0; fault_command = command_lost;
        int ok = hci_sync(g_host, op, p, n);
        if (ok) fprintf(stderr, "fault was not applied: opcode=0x%04x sends=%u\n", op, fault_sends);
        REQUIRE(!ok);
        fault_opcode = 0;
        f.controller.ops = original;
        unsigned expected_sends = op == 0x0401 ? 3 : 1;
        if (fault_sends != expected_sends) fprintf(stderr, "fault opcode=0x%04x sends=%u\n", op, fault_sends);
        REQUIRE(fault_sends == expected_sends); /* Scan remains one-shot; Inquiry has its bounded exception. */
    } else {
        if (command_lost == 2) REQUIRE(sim_foreign_command(f.classic, 0x1001, NULL, 0));
        else attrib_overflow();
        host_poll(g_host, 0);
    }
    own_summary s;
    own_snapshot(host_ownership(g_host), &s);
    REQUIRE(!host_admits(g_host) && !g_host->dead && host_transport_lost(g_host));
    REQUIRE(s.failed_opcode == op);
    if (op) REQUIRE(s.reply_timeout && s.quarantined);
    else REQUIRE(s.runtime.reason == SESSION_AMBIGUOUS && !s.reply_timeout);
    char reason[256], expected[80];
    host_failure_reason(g_host, reason, sizeof reason);
    snprintf(expected, sizeof expected, "opcode=0x%04x reason=%s",
             op, op ? "reply_timeout" : "ambiguous_evidence");
    REQUIRE(strstr(reason, expected) != NULL);
    REQUIRE(own_invariant(host_ownership(g_host)) == NULL);
}

static void restart_pending(void)
{
    int opens = f.opens;
    REQUIRE(!g_host && g_bt_state == WEB_BT_FAILED && g_auto_retry && !g_retry);
    REQUIRE(f.authid == 0x4800000000000001ull);
    static const unsigned char caps[16] = {0};
    REQUIRE(!memcmp(f.caps, caps, sizeof caps));
    usb_finalized();
    bt_retry();
    REQUIRE(!g_auto_retry && !g_retry && g_bt_state == WEB_BT_TRYING);
    REQUIRE(f.opens == opens && !g_host);
    bt_retry();
    REQUIRE(g_bt_state == WEB_BT_TRYING && f.opens == opens);
    bt_try(now_ms());
    REQUIRE(g_host && g_bt_state == WEB_BT_OK && !g_bt_reason[0]);
    REQUIRE(f.opens == opens + 1 && f.closes == opens && f.uninits == opens);
    own_summary s;
    own_snapshot(host_ownership(g_host), &s);
    REQUIRE(s.runtime.state == SESSION_ACTIVE && !s.quarantined && !s.ambiguity_count);
    REQUIRE(!s.links && !s.failed_opcode && !s.reply_timeout);
    REQUIRE(own_invariant(host_ownership(g_host)) == NULL);
}

static int usable_input(long deadline)
{
    while (g_host && host_admits(g_host) && now_ms() < deadline &&
           (!host_connected(g_host, 0) || !vpad_live(0) || !f.inputs)) {
        host_poll(g_host, 4);
        vpad_poll(now_ms());
    }
    if (!g_host || !host_admits(g_host) || !host_connected(g_host, 0) ||
        !vpad_live(0) || !f.inputs || now_ms() >= deadline) return 0;
    pad_info info;
    pad_state state;
    REQUIRE(host_pad_get(g_host, 0, &info, &state));
    REQUIRE(state.buttons & PAD_CROSS);
    REQUIRE(f.ndevices == 1 && f.devices[0].identified && !f.devices[0].deletes);
    return 1;
}

static int recovery_case(unsigned seed, unsigned op, int command_lost, unsigned delay)
{
    recovery_begin(seed);
    f.controller.ops->pump(f.controller.ctx, (int)delay);
    /* An Inquiry without its Status still produces discovery events. Keep
     * that case outside Pair so it really remains an empty eligible host. */
    long until = 0;
    if (op != 0x0401) {
        REQUIRE(host_pair(g_host, 60));
        until = g_host->pair_until;
    }
    runtime_loss(op, command_lost);
    bt_lost(); f.host = NULL;
    restart_pending();
    if (until) {
        REQUIRE(g_host->pair_until == until && host_pairing_left(g_host) > 0);
    } else {
        REQUIRE(!host_pairing_left(g_host));
        char out[20000];
        reply(http_request("POST", "/api/pair"), out, sizeof out, 200);
        until = g_host->pair_until;
        REQUIRE(until > now_ms());
    }
    int found = usable_input(until < now_ms() + 30000 ? until : now_ms() + 30000);
    recovery_end();
    return found;
}

static void retry_budget(void)
{
    recovery_begin(0);
    REQUIRE(host_pair(g_host, 60));
    long until = g_host->pair_until, first = now_ms();
    for (unsigned round = 1; round <= 4; round++) {
        runtime_loss(0x200c, 0);
        bt_lost(); f.host = NULL;
        if (round <= 3) {
            restart_pending();
            REQUIRE(g_host->pair_until == until);
        } else {
            char reason[sizeof g_bt_reason];
            snprintf(reason, sizeof reason, "%s", g_bt_reason);
            bt_retry();
            REQUIRE(!g_retry && !g_auto_retry && !g_host && g_bt_state == WEB_BT_FAILED);
            REQUIRE(f.opens == 4 && f.closes == 4 && f.uninits == 4);
            REQUIRE(reason[0] && !strcmp(g_bt_reason, reason));
            unsigned long logged = log_seq();
            bt_retry(); web_run(g_web); bt_retry();
            REQUIRE(log_seq() == logged && f.opens == 4 && !g_auto_retry && !g_retry);
        }
    }
    REQUIRE(now_ms() - first < 60000);
    /* Actual delivered HTTP Retry bypasses the automatic budget. */
    char out[20000];
    int fd = http_request("POST", "/api/retry");
    web_poll(g_web);
    REQUIRE(!g_retry);
    reply(fd, out, sizeof out, 200);
    REQUIRE(g_retry);
    usb_finalized();
    bt_retry();
    REQUIRE(g_bt_state == WEB_BT_TRYING && !g_retry && !g_auto_retry);
    bt_try(now_ms());
    REQUIRE(g_host && f.opens == 5 && g_bt_state == WEB_BT_OK);
    REQUIRE(!host_pairing_left(g_host)); /* Manual Retry cancels automatic resume. */
    f.controller.ops->pump(f.controller.ctx, 60001);
    runtime_loss(0x200c, 1);
    bt_lost(); f.host = NULL;
    restart_pending(); /* Old timestamps have left the rolling window. */
    recovery_end();
}

static void manual_precedence(void)
{
    recovery_begin(0);
    REQUIRE(host_pair(g_host, 60));
    runtime_loss(0x200c, 0);
    bt_lost(); f.host = NULL;
    REQUIRE(g_auto_retry && !g_retry);
    char out[20000];
    int fd = http_request("POST", "/api/retry");
    web_poll(g_web);
    REQUIRE(g_auto_retry && !g_retry); /* Receive is not delivered admission. */
    reply(fd, out, sizeof out, 200);
    REQUIRE(g_retry && g_auto_retry);
    usb_finalized();
    bt_retry();
    REQUIRE(!g_retry && !g_auto_retry && g_bt_state == WEB_BT_TRYING);
    bt_try(now_ms());
    REQUIRE(g_host && g_bt_state == WEB_BT_OK && f.opens == 2);
    REQUIRE(!host_pairing_left(g_host));
    reply(http_request("POST", "/api/pair"), out, sizeof out, 200);
    REQUIRE(usable_input(now_ms() + 30000));
    recovery_end();
}

enum recovery_guard {
    GUARD_NATIVE, GUARD_STOP, GUARD_REST, GUARD_SIGNAL, GUARD_WEB_STOP,
    GUARD_VPAD, GUARD_CONNECTED, GUARD_RESERVED, GUARD_STORE, GUARD_COMMITTED,
    GUARD_PAD_ID, GUARD_DEAD, GUARD_EVER_LINKED, GUARD_OPCODE, GUARD_COUNT
};

static void guarded(enum recovery_guard guard)
{
    recovery_begin(0);
    if (guard == GUARD_CONNECTED || guard == GUARD_EVER_LINKED) {
        REQUIRE(host_pair(g_host, 60));
        REQUIRE(usable_input(now_ms() + 30000));
        REQUIRE(host_pair(g_host, 0));
        if (guard == GUARD_EVER_LINKED) {
            sim_disconnect(f.classic);
            long end = now_ms() + 5000;
            while (g_host->links[0].used && host_admits(g_host) && now_ms() < end) {
                host_poll(g_host, 4); vpad_poll(now_ms());
            }
            REQUIRE(!g_host->links[0].used && !vpad_live(0));
            REQUIRE(host_forget_all(g_host) == 1);
            own_summary s;
            own_snapshot(host_ownership(g_host), &s);
            REQUIRE(!s.links && !g_host->store.count && !g_host->committed.count);
        }
    } else if (guard == GUARD_RESERVED) {
        own_peer peer = {.wire = {1, 2, 3, 4, 5, 6}};
        link_ref link;
        REQUIRE(own_reserve_link(host_ownership(g_host), &peer, &link) == OWN_OK);
        own_summary s;
        own_snapshot(host_ownership(g_host), &s);
        REQUIRE(s.links == 1);
    } else if (guard == GUARD_STORE || guard == GUARD_COMMITTED) {
        db_rec record = {.addr = {1, 2, 3, 4, 5, 6}, .vid = 0x054c,
            .pid = 0x05c4, .complete = 1};
        memset(record.key, 1, sizeof record.key);
        bond_store *store = guard == GUARD_STORE ? &g_host->store : &g_host->committed;
        REQUIRE(bond_store_put(store, &record) == BOND_COMMITTED);
    }
    if (guard <= GUARD_REST) {
        enum session_reason why = guard == GUARD_NATIVE ? SESSION_NATIVE :
                                  guard == GUARD_STOP ? SESSION_STOP : SESSION_REST;
        REQUIRE(own_release(host_ownership(g_host), why, (uint64_t)now_ms()));
    } else {
        runtime_loss(guard == GUARD_OPCODE ? 0x0c19 :
                     guard == GUARD_CONNECTED || guard == GUARD_EVER_LINKED ? 0x0c1b : 0x200c, 0);
    }
    if (guard == GUARD_SIGNAL) g_stop = 1;
    if (guard == GUARD_WEB_STOP) g_web_stop = 1;
    if (guard == GUARD_VPAD) g_vpad_ok = 0;
    if (guard == GUARD_PAD_ID) g_pad_id[0].vid = 0x054c;
    if (guard == GUARD_DEAD) g_host->dead = 1;
    bt_lost(); f.host = NULL;
    REQUIRE(!g_host && g_bt_state == WEB_BT_FAILED && !g_auto_retry && !g_retry);
    REQUIRE(g_bt_reason[0]);
    usb_finalized();
    bt_retry(); bt_retry();
    REQUIRE(g_bt_state == WEB_BT_FAILED && f.opens == 1 && !g_auto_retry && !g_retry);
    recovery_end();
}

static void startup_failure(void)
{
    recovery_begin(0);
    host_close(g_host); g_host = f.host = NULL;
    usb_finalized();
    sim_go_deaf(f.classic);
    bt_begin();
    for (int attempt = 1; attempt <= OPEN_TRIES; attempt++) {
        if (g_bt_next > now_ms())
            f.controller.ops->pump(f.controller.ctx, (int)(g_bt_next - now_ms()));
        bt_try(now_ms());
        REQUIRE(!g_host && g_bt_attempt == attempt && !g_auto_retry && !g_retry);
        REQUIRE(f.opens == attempt + 1);
        usb_finalized();
    }
    REQUIRE(g_bt_state == WEB_BT_FAILED);
    bt_retry();
    REQUIRE(g_bt_state == WEB_BT_FAILED && f.opens == OPEN_TRIES + 1);
    recovery_end();
}
#endif

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    REQUIRE(log_open("/dev/null"));
#ifdef TEST_RECOVERY_RED
    recovery_begin(0);
    REQUIRE(host_pair(g_host, 60));
    const unsigned char enable[] = {1, 0};
    unsigned op = argc == 2 ? (unsigned)strtoul(argv[1], NULL, 0) : 0x200c;
    if (op) {
        delay_events(0x0e, 1, -1);
        REQUIRE(!hci_sync(g_host, op, op == 0x200c ? enable : NULL, op == 0x200c ? 2 : 0));
    } else {
        attrib_overflow();
        host_poll(g_host, 0);
    }
    bt_lost(); f.host = NULL;
    REQUIRE(!g_host && g_bt_state == WEB_BT_FAILED && !g_retry);
    bt_retry();
    REQUIRE(g_bt_state == WEB_BT_TRYING); /* Must fail baseline, not candidate. */
    recovery_end();
#else
    unsigned seeds = 0;
    if (argc == 2 && !strcmp(argv[1], "--full")) seeds = 10000;
    else if (argc == 3 && !strcmp(argv[1], "--seeds")) {
        char *end;
        errno = 0;
        unsigned long n = strtoul(argv[2], &end, 10);
        if (errno || end == argv[2] || *end || !n || n > 1000000) return 2;
        seeds = (unsigned)n;
    } else if (argc != 1) return 2;
    /* A: buffer query; B: LE scan; C: detector with no outstanding opcode. */
    static const unsigned ops[] = {0x1005, 0x200c, 0, 0x0c1b, 0x0401};
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++)
        for (unsigned lost = 0; lost < 2; lost++)
            REQUIRE(recovery_case(lost, ops[i], (int)lost, 0));
    REQUIRE(recovery_case(0, 0, 2, 0)); /* Unsolicited Complete, not a lost local reply. */
    retry_budget();
    manual_precedence();
    for (unsigned guard = 0; guard < GUARD_COUNT; guard++)
        guarded((enum recovery_guard)guard);
    startup_failure();
    unsigned found = 0;
    for (unsigned seed = 0; seed < seeds; seed++) {
        unsigned rng = seed + 0x9e3779b9u;
        unsigned word = random_next(&rng);
        found += (unsigned)recovery_case((seed + 1) * 16 + (seed & 1),
                    ops[word % (sizeof ops / sizeof ops[0])],
                    (int)((word >> 8) & 1), (word >> 16) % 257);
    }
    REQUIRE((unsigned long long)found * 10000 >= (unsigned long long)seeds * 9990);
    printf("RECOVERY policy=%d seeds=%u found-and-input=%u checks=%u\n",
           WHATEVERPAD_ATTRIB_POLICY, seeds, found, checks);
#endif
    log_close();
    return 0;
}
