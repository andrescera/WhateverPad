/* Read-budget decisions from the 19422e9 console run, not a radio simulation. */
#include "../src/event_reads.h"
#include "../src/host_int.h"
#include "../src/util.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void test_budget(void)
{
    /* task-7-partB-whateverpad.log:14598-14618, Xbox reconnect then ready. */
    static const unsigned char connected[] = {
        0x03, 0x0b, 0x00, 0x01, 0x00, 0x06, 0xfa, 0xab, 0xea, 0xb6, 0x98, 0x01, 0x00
    };
    /* Same log:14623-14628, two native requests, both followed by a yield. */
    static const unsigned char incoming[] = {
        0x04, 0x0a, 0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48, 0x08, 0x25, 0x00, 0x01
    };
    static const long request_at[] = { 944769, 989681 };
    static const long yielded_at[] = { 944771, 989684 };
    host_t h = {0};
    int i;

    h.links[0].used = h.links[0].connected = h.links[0].ready = 1;
    h.links[0].handle = le16(connected + 3) & 0x0fff;
    memcpy(h.links[0].addr, connected + 5, 6);
    CHECK(link_by_addr(&h, incoming + 2) == NULL);

    puts("ready own pads retain full event reads");
    for (i = 0; i < 2; i++)
        CHECK(event_reads_wanted(&h, request_at[i], g_tune.idle_reads) == HOST_EV_READS);

    puts("own setup takes precedence over a foreign pause");
    for (i = 0; i < 2; i++) {
        h.yield_at = yielded_at[i];
        h.links[0].ready = 0;
        h.pair_until = yielded_at[i] + 60000;
        CHECK(event_reads_wanted(&h, yielded_at[i], 1) == HOST_EV_READS);
        CHECK(event_reads_wanted(&h, yielded_at[i] + T_YIELD - 1, 1) == HOST_EV_READS);
        CHECK(event_reads_wanted(&h, yielded_at[i] + T_YIELD, 1) == HOST_EV_READS);
        h.links[0].ready = 1;
        h.pair_until = 0;
        CHECK(event_reads_wanted(&h, yielded_at[i] + T_YIELD, g_tune.idle_reads) == HOST_EV_READS);
    }
    h.yield_at = 0;

    puts("searches, reconnect setup, disconnects and command replies keep full reads");
    h.links[0].ready = 0;             /* reconnecting, not a new pairing */
    CHECK(event_reads_wanted(&h, 900813, 1) == HOST_EV_READS);
    h.links[0].ready = 1;
    h.links[0].disconnecting = 1;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.links[0].disconnecting = 0;
    h.pair_until = 944770;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.pair_until = 0;
    h.inquiring = 1;                 /* still awaiting Inquiry Complete */
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.inquiring = 0;
    h.repage_at = 944769;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.repage_at = 0;
    h.defer_at = 944769;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.defer_at = 0;
    h.pair_pending_s = 60;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.pair_pending_s = 0;
    h.links[0].used = 0;
    h.full_reads_until = 927347 + 2000; /* logged Read Buffer Size, existing reply interval */
    CHECK(event_reads_wanted(&h, h.full_reads_until - 1, 1) == HOST_EV_READS);
    CHECK(event_reads_wanted(&h, h.full_reads_until, 1) == 1);

    puts("the existing explicit idle-read tuning remains available");
    CHECK(event_reads_wanted(&h, 944769, 0) == 0);
    CHECK(event_reads_wanted(&h, 944769, HOST_EV_READS) == HOST_EV_READS);
    CHECK(event_reads_wanted(&h, 944769, g_tune.idle_reads) == 1);
    h.yield_at = 944769;
    CHECK(event_reads_wanted(&h, 944770, 1) == 0);
    CHECK(event_reads_wanted(&h, 944769 + T_YIELD, 1) == 1);
}

/* Captured Part A/B frames. Combined-state cases are controlled unit inputs,
 * not claims that these frames coincided on the console. No replies are generated. */
static const unsigned char foreign[] = { 4, 10, 0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48, 8, 0x25, 0, 1 };
static const unsigned char request[] = { 4, 10, 6, 0xfa, 0xab, 0xea, 0xb6, 0x98, 8, 5, 0, 1 };
static const unsigned char complete[] = { 3, 11, 0, 1, 0, 6, 0xfa, 0xab, 0xea, 0xb6, 0x98, 1, 0 };
static const unsigned char key[] = { 0x17, 6, 6, 0xfa, 0xab, 0xea, 0xb6, 0x98 };
static const unsigned char capability[] = { 0x31, 6, 6, 0xfa, 0xab, 0xea, 0xb6, 0x98 };
static const unsigned char authenticated[] = { 6, 3, 0, 1, 0 };
static const unsigned char encrypted[] = { 8, 4, 0, 1, 0, 1 };
static const unsigned char disconnected[] = { 5, 4, 0, 1, 0, 8 };
static const unsigned char buffer_reply[] = { 0x0e, 11, 1, 5, 0x10, 0, 0xfd, 3, 0xf0, 7, 0, 3, 0 };

static long tick;
static long clock_value(void) { return tick; }

/* Transport boundary recorder: no event queue, peer, radio or success reply. */
typedef struct {
    host_t *host;
    unsigned ops[16];
    long cmd_at[16];
    int ncmd, reads, yield_ms, pauses, order, read_order, pump_order, foreign_seen;
    unsigned char params[32];
    int plen, inject_foreign;
} calls;

static int capture_cmd(void *ctx, unsigned op, const void *p, int len)
{
    calls *c = ctx;
    int order = ++c->order;

    CHECK(c->reads == HOST_EV_READS && c->yield_ms == 0);
    CHECK(c->pump_order > c->read_order && c->pump_order < order);
    CHECK(c->ncmd < 16 && len <= (int)sizeof c->params);
    if (c->ncmd < 16) {
        c->ops[c->ncmd] = op;
        c->cmd_at[c->ncmd++] = tick;
    }
    c->plen = len;
    if (len > 0 && len <= (int)sizeof c->params) memcpy(c->params, p, (size_t)len);
    return 1;                       /* write accepted, not a controller reply */
}

static int capture_pump(void *ctx, int timeout_ms)
{
    calls *c = ctx;

    c->pump_order = ++c->order;
    tick += timeout_ms;              /* exercise actual timeout code without waiting */
    if (c->inject_foreign && c->ncmd == 3 && !c->foreign_seen &&
        tick >= c->cmd_at[2] + 2200) {
        c->foreign_seen = 1;
        host_on_event(c->host, foreign, sizeof foreign);
    }
    return 0;
}

static int no_packets(void *ctx, unsigned char *out, int max)
{
    (void)ctx; (void)out; (void)max;
    return 0;
}

static void capture_reads(void *ctx, int n)
{
    calls *c = ctx;
    c->read_order = ++c->order;
    c->reads = n;
}

static void capture_yield(void *ctx, int ms)
{
    calls *c = ctx;
    c->yield_ms = ms;
    if (ms > 0) c->pauses++;
}

static int marvell(void *ctx) { (void)ctx; return 0x1286; }

static const hci_ops record_ops = {
    .cmd = capture_cmd, .pump = capture_pump,
    .next_event = no_packets, .next_acl = no_packets,
    .yield = capture_yield, .event_reads = capture_reads, .chip_vendor = marvell
};

static void begin(host_t *h, calls *c, long at)
{
    memset(h, 0, sizeof *h);
    memset(c, 0, sizeof *c);
    tick = at;
    ph_clock_hook = clock_value;
    c->host = h;
    c->reads = h->reads_now = HOST_EV_READS;
    h->hci.ops = &record_ops;
    h->hci.ctx = c;
    h->last_rx = at;
}

static void start_pause(host_t *h, calls *c)
{
    begin(h, c, 944769);
    host_poll(h, 0);
    CHECK(c->reads == 1);
    host_on_event(h, foreign, sizeof foreign);
    CHECK(c->yield_ms == T_YIELD && c->reads == 0);
    tick++;
}

static void test_protected_transitions(void)
{
    host_t h;
    calls c;

    puts("actual host transitions cancel or refuse foreign pauses");
    start_pause(&h, &c);
    CHECK(link_new(&h, request + 2) != NULL);
    host_poll(&h, 0);
    CHECK(h.yield_at == 0 && c.yield_ms == 0 && c.reads == HOST_EV_READS);
    CHECK(event_reads_wanted(&h, tick, 1) == HOST_EV_READS);

    start_pause(&h, &c);
    CHECK(host_pair(&h, 60));
    CHECK(h.pair_until == tick + 60000 && h.pair_pending_s == 0);
    CHECK(h.yield_at == 0 && c.yield_ms == 0 && c.reads == HOST_EV_READS);
    host_on_event(&h, foreign, sizeof foreign);
    CHECK(c.pauses == 1 && h.yield_at == 0); /* consumed; no second pause or native command */
    CHECK(c.ncmd == 0);

    start_pause(&h, &c);
    CHECK(send_cmd(&h, OP_READ_BUFFER_SIZE, NULL, 0));
    CHECK(h.yield_at == 0 && c.yield_ms == 0);
    CHECK(h.full_reads_until == tick + 2000);
    tick++;
    CHECK(send_cmd(&h, OP_READ_BUFFER_SIZE, NULL, 0)); /* already full: refresh the deadline */
    CHECK(h.full_reads_until == tick + 2000);
    host_on_event(&h, foreign, sizeof foreign);
    CHECK(c.pauses == 1 && c.reads == HOST_EV_READS && c.ncmd == 2);

    begin(&h, &c, 927347);
    host_poll(&h, 0);
    CHECK(c.reads == 1);
    CHECK(send_cmd(&h, OP_READ_BUFFER_SIZE, NULL, 0)); /* one idle read -> posted full reads */
    CHECK(c.reads == HOST_EV_READS && c.pump_order > c.read_order);

    start_pause(&h, &c);
    tick = h.yield_at + T_YIELD;      /* USB's pause may have begun after the host timestamp */
    CHECK(send_cmd(&h, OP_READ_BUFFER_SIZE, NULL, 0));
    CHECK(h.yield_at == 0 && c.yield_ms == 0);

    start_pause(&h, &c);
    /* An already-deferred Pair request, as the old host_pair left it. */
    h.pair_pending_s = 60;
    h.pair_wait_until = tick;
    host_poll(&h, 0);
    CHECK(h.yield_at == 0 && c.yield_ms == 0 && c.reads == HOST_EV_READS);
    CHECK(h.pair_pending_s == 0 && h.pair_until == tick + 60000);
}

static void test_captured_security(void)
{
    host_t h;
    calls c;
    link_t *l;

    puts("captured connection, key, capability, authentication and encryption");
    begin(&h, &c, 762833);
    CHECK(host_pair(&h, 60));
    l = link_new(&h, request + 2);
    CHECK(l != NULL);
    if (!l) return;
    l->pairing = 1;
    tick = 770146;
    host_on_event(&h, complete, sizeof complete);
    CHECK(l->connected && l->handle == 1 && l->t_conn == tick);
    tick = 770154;
    host_on_event(&h, key, sizeof key);
    CHECK(l->ssp_stage == 1 && c.ops[c.ncmd - 1] == OP_LINK_KEY_NEG_REPLY);
    tick = 770161;
    host_on_event(&h, capability, sizeof capability);
    CHECK(l->ssp_stage == 2 && c.ops[c.ncmd - 1] == OP_IO_CAP_REPLY);
    CHECK(c.plen == 9 && memcmp(c.params, request + 2, 6) == 0);
    host_on_event(&h, foreign, sizeof foreign);
    CHECK(c.pauses == 0 && h.yield_at == 0 && c.reads == HOST_EV_READS);
    tick = 771103;
    host_on_event(&h, authenticated, sizeof authenticated);
    CHECK(l->auth_ok);
    tick = 771107;
    host_poll(&h, 0);
    CHECK(l->enc_sent && c.ops[c.ncmd - 1] == OP_SET_ENCRYPTION);
    tick = 771153;
    host_on_event(&h, encrypted, sizeof encrypted);
    CHECK(l->enc_on && event_reads_wanted(&h, tick, 1) == HOST_EV_READS);
}

static void test_idle_loss_and_probe(void)
{
    host_t h;
    calls c;
    link_t *l;

    puts("a controller probe does not recover a missed own disconnect");
    begin(&h, &c, 905061);
    h.ndb = 1;
    memcpy(h.db[0].addr, request + 2, 6);
    l = link_new(&h, request + 2);
    CHECK(l != NULL);
    if (!l) return;
    host_on_event(&h, complete, sizeof complete);
    /* Initial fixture: the log's already-ready Xbox, not a test of HID setup. */
    l->ready = l->auth_ok = l->enc_on = l->ids_known = l->init_done = 1;
    l->t_secure = tick;
    l->ctrl.st = l->intr.st = CH_OPEN;
    l->prof = profile_find(0x045e, 0x02e0);
    host_poll(&h, 0);
    CHECK(c.reads == HOST_EV_READS);
    tick = 927347;
    host_poll(&h, 0);
    CHECK(c.ncmd == 1 && c.ops[0] == OP_READ_BUFFER_SIZE && h.probes == 1);
    tick = 927350;
    host_on_event(&h, buffer_reply, sizeof buffer_reply);
    CHECK(h.cc_op == OP_READ_BUFFER_SIZE && h.last_rx == tick);
    tick++;
    host_poll(&h, 0);
    CHECK(h.probes == 0 && !h.dead);
    CHECK(l->used && l->connected && l->ready); /* missing disconnect remains missing */
    CHECK(c.reads == HOST_EV_READS);           /* do not reduce or pause this traffic */
    host_on_event(&h, foreign, sizeof foreign);
    CHECK(c.pauses == 0);

    tick = 989681;
    host_on_event(&h, request, sizeof request); /* received later request replaces stale link */
    l = link_by_addr(&h, request + 2);
    CHECK(l && !l->connected && c.ops[c.ncmd - 1] == OP_ACCEPT_CONNECTION);
    tick++;
    host_on_event(&h, complete, sizeof complete);
    host_on_event(&h, key, sizeof key);
    CHECK(l && l->connected && c.ops[c.ncmd - 1] == OP_LINK_KEY_REPLY);
    CHECK(c.plen == 22 && memcmp(c.params, request + 2, 6) == 0);
    host_on_event(&h, disconnected, sizeof disconnected);
    CHECK(link_by_addr(&h, request + 2) == NULL);
    CHECK(event_reads_wanted(&h, tick + 3000, 1) == HOST_EV_READS); /* stored pad may reconnect */
}

static void test_timeout_recovery_limits(void)
{
    host_t h;
    calls c;
    link_t *l;
    int i;

    puts("real synchronous retry deadline protects its entire final attempt");
    begin(&h, &c, 927347);
    c.inject_foreign = 1;
    CHECK(!hci_sync(&h, OP_READ_BUFFER_SIZE, NULL, 0)); /* no Command Complete supplied */
    CHECK(c.ncmd == 3 && tick == 927347 + 5000 && c.foreign_seen);
    CHECK(c.pauses == 0);
    CHECK(h.full_reads_until >= c.cmd_at[2] + 3000);

    puts("three lost controller probes report failure, not pad recovery");
    begin(&h, &c, 927347);
    h.last_rx = tick - T_PROBE_IDLE - 1;
    host_poll(&h, 0);
    for (i = 0; i < PROBE_TRIES; i++) {
        tick += T_PROBE_WAIT + 1;
        host_poll(&h, 0);
    }
    CHECK(h.dead && h.probes == PROBE_TRIES && c.ncmd == PROBE_TRIES);

    puts("missing Connection Complete drops the attempt at the real page timeout");
    begin(&h, &c, 900719);
    l = link_new(&h, request + 2);
    CHECK(l != NULL);
    tick += T_CONN_TIMEOUT + 1;
    host_on_event(&h, buffer_reply, sizeof buffer_reply); /* chip alive, page still unanswered */
    host_poll(&h, 0);
    CHECK(!h.dead && link_by_addr(&h, request + 2) == NULL);
}

int main(void)
{
    test_budget();
    test_protected_transitions();
    test_captured_security();
    test_idle_loss_and_probe();
    test_timeout_recovery_limits();
    ph_clock_hook = NULL;
    printf(g_fail ? "%d check(s) failed\n" : "all event-read checks passed\n", g_fail);
    return g_fail != 0;
}
