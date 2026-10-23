/* Lifecycle seams for tasks 3-4. No replacement runtime policy lives here. */
#include "sim.h"
#include "sim_le.h"
#include "../src/host_int.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks, pending, strict_mode;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
static char directory[] = "/tmp/whateverpad-session-XXXXXX";
static char db[160];
static unsigned case_no;
static const unsigned char native[6] = { 0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48 };
static const unsigned char third_party[6] = { 1, 2, 3, 4, 5, 6 };

static void expected_failure(int task, const char *name, int desired)
{
    /* --expected-failures makes these real failing assertions, separately
     * from the green tests of today's transport and observable behavior. */
    int todo = task == 3 ? 15 : task == 4 ? 16 : task;
    printf("%s expected-failing-until-todo-%d %s\n", desired ? "XPASS" : "XFAIL", todo, name);
    if (!desired) {
        pending++;
        if (strict_mode) printf("FAIL %s: desired observable effect absent (todo-%d)\n", name, todo);
    }
}

static void expected_effect(int todo, const char *name, int desired, const char *effect)
{
    checks++;
    printf("%s expected-failing-until-todo-%d %s\n", desired ? "XPASS" : "XFAIL", todo, name);
    if (!desired) {
        pending++;
        if (strict_mode) printf("FAIL %s: %s (todo-%d)\n", name, effect, todo);
    }
}
#define EFFECT(todo, name, c) expected_effect(todo, name, (c), #c)

static void clear_db(void)
{
    char path[180];
    if (!db[0]) return;
    unlink(db);
    snprintf(path, sizeof path, "%s.new", db); unlink(path);
    snprintf(path, sizeof path, "%s.irk", db); unlink(path);
}

static void next_db(void)
{
    clear_db();
    snprintf(db, sizeof db, "%s/case-%u.db", directory, ++case_no);
    CHECK(access(db, F_OK) != 0);
}

static sim_pad pad_fixture(void)
{
    sim_pad p = {0};
    p.mode = SIM_WAIT_PAIRING;
    memcpy(p.addr, third_party, 6);
    p.cod = 0x002508; /* both native and admitted third-party peers use this class */
    p.vid = 0x054c; p.pid = 0x05c4;
    p.report_len = 78;
    p.report[0] = 0x11; p.report[1] = 0xc0;
    p.report[3] = p.report[4] = p.report[5] = p.report[6] = 0x80;
    p.report[7] = 8;
    return p;
}

typedef struct { int connects, disconnects, states; } seen;
static void connected(void *ud, int slot, const pad_info *info)
{
    (void)slot; (void)info; ((seen *)ud)->connects++;
}
static void disconnected(void *ud, int slot)
{
    (void)slot; ((seen *)ud)->disconnects++;
}
static host_events observers(seen *s)
{
    host_events ev = { connected, NULL, disconnected, s, NULL };
    return ev;
}

static void test_consume_once_and_distinct_peers(void)
{
    unsigned char packet[300];
    static const unsigned char request[] = { 0x04, 0x0a, 0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48, 8, 0x25, 0, 1 };
    sim_pad p = pad_fixture();
    sim_t *s = sim_new(&p);
    hci_t h = sim_hci(s);
    sim_lifetime life;

    sim_peer_request(s, third_party, p.cod);
    sim_peer_request(s, native, p.cod);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 12);
    CHECK(memcmp(packet + 2, third_party, 6) == 0);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == sizeof request);
    CHECK(memcmp(packet, request, sizeof request) == 0);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 0);
    CHECK(sim_consumed_requests(s) == 2);
    CHECK(sim_system_event(s, packet, sizeof packet) == 0);
    h.ops->yield(h.ctx, 30000);
    CHECK(sim_system_event(s, packet, sizeof packet) == 0); /* yield is not replay */
    sim_delay_events(s, 0x04, 1, -1);
    sim_peer_request(s, native, p.cod); /* a genuinely lost retry is NOT system success */
    CHECK(sim_system_event(s, packet, sizeof packet) == 0 && sim_left_to_system(s) == 0);
    sim_peer_request(s, native, p.cod); /* distinct later air event */
    CHECK(sim_system_event(s, packet, sizeof packet) == sizeof request);
    CHECK(memcmp(packet, request, sizeof request) == 0);
    CHECK(sim_system_event(s, packet, sizeof packet) == 0);
    CHECK(sim_left_to_system(s) == 1);
    life = sim_resources(s);
    CHECK(life.event_reads == 0 && life.acl_reads == 20 && life.closes == 0);
    h.ops->pump(h.ctx, 30000);
    CHECK(sim_resources(s).event_reads == 40);
    h.ops->close(h.ctx);
    h.ops->close(h.ctx);
    life = sim_resources(s);
    CHECK(life.opens == 1 && life.uninits == 1 && life.closes == 1);
    CHECK(life.event_reads == 0 && life.acl_reads == 0);
    CHECK(life.event_stops == 80 && life.acl_stops == 20);
    CHECK(!h.ops->cmd(h.ctx, 0x1009, NULL, 0));
    CHECK(!h.ops->acl_send(h.ctx, packet, 0));
    sim_peer_request(s, native, p.cod); /* only a new event reaches system after close */
    CHECK(sim_system_event(s, packet, sizeof packet) == sizeof request);
    CHECK(memcmp(packet, request, sizeof request) == 0 && sim_left_to_system(s) == 2);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 0);
    CHECK(sim_foreign_commands(s) == 0);
    printf("SESSION consume-once: bridge(third-party, native same-CoD) -> yield(no replay)"
           " -> lost-retry(system=0) -> new-retry(system=1) -> resume -> close"
           " -> new-retry(system=2); opens=1 stops=80/20 uninit=1 close=1\n");
    sim_free(s);
}

static void test_classic_terminal_order(void)
{
    unsigned char packet[300], page[13] = {0};
    static const unsigned char end[] = { 0, 0x41, 0, 0x13 };
    sim_pad p = pad_fixture();
    sim_t *s = sim_new(&p);
    hci_t h = sim_hci(s);
    memcpy(page, p.addr, 6);
    sim_delay_events(s, 0x03, 1, 100);
    sim_schedule_event(s, 0x05, end, sizeof end, 50); /* arranged before trigger */
    CHECK(h.ops->cmd(h.ctx, 0x0405, page, sizeof page));
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 6 && packet[0] == 0x0f);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 0);
    CHECK(h.ops->pump(h.ctx, 49) == 0);
    CHECK(h.ops->pump(h.ctx, 1) == 1);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 6 && packet[0] == 0x05);
    CHECK(h.ops->pump(h.ctx, 50) == 1);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 13 && packet[0] == 0x03);
    CHECK(memcmp(packet + 5, third_party, 6) == 0);
    sim_delay_events(s, 0x03, 1, -1);
    CHECK(h.ops->cmd(h.ctx, 0x0405, page, sizeof page));
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 6 && packet[0] == 0x0f);
    CHECK(h.ops->pump(h.ctx, 10000) == 0);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 0);
    CHECK(sim_left_to_system(s) == 0); /* loss is not a simulated system delivery */
    h.ops->close(h.ctx);
    printf("SESSION Classic terminals: Status(success) -> +49ms(none) -> +1ms Disconnect"
           " -> +50ms Connection Complete -> Status(success) -> +10000ms(lost terminal, system=0) -> close\n");
    sim_free(s);
}

static void test_expected_current_native_request_does_not_release(int mode)
{
    sim_pad p = pad_fixture();
    sim_t *s;
    hci_t transport;
    host_t *h;
    seen callbacks = {0};
    host_events ev = observers(&callbacks);
    unsigned char packet[300];
    long deadline;
    next_db();
    if (mode != 2) p.mode = SIM_RECONNECT; /* no inquiry result until setup case */
    s = sim_new(&p);
    transport = sim_hci(s);
    transport.ops->pump(transport.ctx, 31000); /* exceed the current yield rate limit */
    h = host_open(transport, db, &ev);
    CHECK(h != NULL);
    if (!h) { transport.ops->close(transport.ctx); sim_free(s); return; }
    if (mode) {
        sim_delay_events(s, 0x03, 1, 5000);
        CHECK(host_pair(h, 60));
        if (mode == 2) {
            deadline = sim_now() + 1000;
            while (!sim_create_requests(s) && sim_now() < deadline) host_poll(h, 5);
            CHECK(sim_create_requests(s) == 1 && callbacks.connects == 0);
        }
    }
    sim_peer_request(s, native, p.cod);
    host_poll(h, 5);
    CHECK(sim_consumed_requests(s) == 1); /* unrelated command replies may already be queued */
    CHECK(sim_system_event(s, packet, sizeof packet) == 0);
    CHECK(sim_foreign_commands(s) == 0 && sim_rejects(s) == 0);
    CHECK(sim_resources(s).closes == 0 && sim_resources(s).acl_reads == 20);
    CHECK(host_pause_left(h) > 0 || mode != 0);
    if (mode) CHECK(sim_resources(s).event_reads == 40); /* current code does not yield during Pair/setup */
    expected_failure(4, mode == 0 ? "native-idle-full-release" :
                     mode == 1 ? "native-during-pair-full-release" : "native-preempts-same-CoD-setup",
                     sim_resources(s).closes == 1);
    printf("SESSION expected-current native mode=%d: consume(request) -> system=0 foreign-commands=0"
           " -> event-reads=%d ACL-reads=20 close=0 -> explicit host_close\n", mode, sim_resources(s).event_reads);
    host_close(h);
    CHECK(sim_resources(s).closes == 1);
    sim_free(s);
}

static void test_expected_current_partial_startup_leaks_transport(void)
{
    sim_pad p = pad_fixture();
    sim_t *s = sim_new(&p);
    hci_t transport = sim_hci(s);
    next_db();
    sim_go_deaf(s);
    CHECK(host_open(transport, db, NULL) == NULL);
    CHECK(sim_resources(s).opens == 1 && sim_resources(s).closes == 0);
    expected_failure(3, "partial-startup-bounded-unwind", sim_resources(s).closes == 1);
    transport.ops->close(transport.ctx); /* caller cleanup, not a false host success */
    CHECK(sim_resources(s).uninits == 1 && sim_resources(s).closes == 1);
    printf("SESSION expected-current startup: open -> no replies -> host_open(NULL)"
           " -> close=0 -> caller-close -> uninit=1 close=1\n");
    sim_free(s);
}

static void test_classic_close_callbacks(int lose_terminal)
{
    sim_pad p = pad_fixture();
    sim_t *s = sim_new(&p);
    hci_t transport = sim_hci(s);
    host_t *h;
    seen callbacks = {0};
    host_events ev = observers(&callbacks);
    long deadline, start;
    next_db();
    h = host_open(transport, db, &ev);
    CHECK(h != NULL);
    if (!h) { transport.ops->close(transport.ctx); sim_free(s); return; }
    CHECK(host_pair(h, 30));
    deadline = sim_now() + 6000;
    while (!callbacks.connects && sim_now() < deadline) host_poll(h, 5);
    CHECK(callbacks.connects == 1);
    CHECK(callbacks.disconnects == 0);
    if (lose_terminal) sim_delay_events(s, 0x05, 1, -1);
    start = sim_now();
    host_close(h);
    CHECK(sim_now() - start <= 10000);
    CHECK(sim_resources(s).closes == 1 && sim_resources(s).uninits == 1);
    CHECK(sim_disconnect_requests(s) == 1);
    CHECK(callbacks.disconnects == (lose_terminal ? 0 : 1));
    if (lose_terminal) expected_failure(3, "missing-disconnect-callback-on-close", callbacks.disconnects == 1);
    printf("SESSION %s close: usable-input(connect=1) -> Disconnect -> %s"
           " -> close=1 uninit=1 callbacks=%d elapsed=%ldms\n",
           lose_terminal ? "expected-current" : "happy",
           lose_terminal ? "lost-terminal" : "terminal", callbacks.disconnects, sim_now() - start);
    sim_free(s);
}

static void test_le_terminal_seams(void)
{
    sim_le_pad p = {0};
    sim_le_t *s;
    hci_t h;
    unsigned char packet[300], conn[19] = {1, 0, 0x50};
    static const unsigned char req[] = { 0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48, 8, 0x25, 0, 1 };
    s = sim_le_new(&p);
    h = sim_le_hci(s);
    sim_le_peer_advertise(s, third_party, 0);
    sim_le_peer_advertise(s, third_party, 1);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 25 && packet[2] == 2 && packet[5] == 0);
    CHECK(memcmp(packet + 6, third_party, 6) == 0);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 25 && packet[2] == 2 && packet[5] == 1);
    CHECK(memcmp(packet + 6, third_party, 6) == 0);
    sim_le_delay_events(s, 0x3e, 9, 1, 100); /* DHKey completion only */
    sim_le_schedule_event(s, 0x3e, conn, sizeof conn, 50);
    CHECK(h.ops->cmd(h.ctx, 0x2026, NULL, 0));
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 6 && packet[0] == 0x0f);
    CHECK(h.ops->pump(h.ctx, 49) == 0);
    CHECK(h.ops->pump(h.ctx, 1) == 1);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 21 && packet[2] == 1);
    CHECK(h.ops->pump(h.ctx, 50) == 1);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 36 && packet[2] == 9);
    sim_le_delay_events(s, 0x3e, 9, 1, -1);
    CHECK(h.ops->cmd(h.ctx, 0x2026, NULL, 0));
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 6 && packet[0] == 0x0f);
    CHECK(h.ops->pump(h.ctx, 10000) == 0);
    CHECK(sim_le_system_event(s, packet, sizeof packet) == 0);
    sim_le_inject_event(s, 4, req, sizeof req);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 12);
    h.ops->yield(h.ctx, 30000);
    CHECK(sim_le_system_event(s, packet, sizeof packet) == 0);
    sim_le_inject_event(s, 4, req, sizeof req);
    CHECK(sim_le_system_event(s, packet, sizeof packet) == 12);
    CHECK(memcmp(packet + 2, native, 6) == 0);
    CHECK(sim_le_resources(s).event_reads == 0 && sim_le_resources(s).acl_reads == 20);
    h.ops->close(h.ctx); h.ops->close(h.ctx);
    CHECK(sim_le_resources(s).opens == 1 && sim_le_resources(s).closes == 1);
    CHECK(sim_le_resources(s).uninits == 1 && sim_le_resources(s).acl_stops == 20);
    CHECK(!h.ops->cmd(h.ctx, 0x1009, NULL, 0));
    CHECK(!h.ops->acl_send(h.ctx, packet, 0));
    printf("SESSION LE seams: same-address public/random adverts -> Status -> +50ms Connection Complete -> +50ms DHKey"
           " -> Status -> lost-DHKey(system=0) -> native consumed -> yield(no replay)"
           " -> separate native retry(system=1) -> close=1 uninit=1\n");
    sim_le_free(s);
}

static const hci_ops *pending_base;
static int create_statuses, create_terminals;
static int pending_event(void *ctx, unsigned char *out, int max)
{
    int n = pending_base->next_event(ctx, out, max);
    if (n == 6 && out[0] == 0x0f && out[2] == 0 && le16(out + 4) == 0x200d)
        create_statuses++;
    if (n == 21 && out[0] == 0x3e && out[2] == 1) create_terminals++;
    return n;
}

static void test_le_status_retains_pending_ownership(int lost_terminal)
{
    sim_le_pad p = {0};
    sim_le_t *s;
    hci_t transport;
    host_t *h;
    seen callbacks = {0};
    host_events ev = observers(&callbacks);
    unsigned char packet[300];
    long deadline;
    next_db();
    memcpy(p.addr, third_party, 6);
    p.vid = 0x045e; p.pid = 0x0b13;
    s = sim_le_new(&p);
    transport = sim_le_hci(s);
    hci_ops ops = *transport.ops;
    pending_base = transport.ops;
    ops.next_event = pending_event;
    transport.ops = &ops;
    create_statuses = create_terminals = 0;
    h = host_open(transport, db, &ev);
    CHECK(h != NULL);
    if (!h) { transport.ops->close(transport.ctx); sim_le_free(s); return; }
    sim_le_delay_events(s, 0x3e, 1, 1, lost_terminal ? -1 : 10000);
    CHECK(host_pair(h, 30));
    deadline = sim_le_now() + 1000;
    while (!sim_le_creates(s) && sim_le_now() < deadline) host_poll(h, 5);
    CHECK(sim_le_creates(s) == 1 && callbacks.connects == 0);
    CHECK(create_statuses == 1 && create_terminals == 0);
    long start = sim_le_now();
    host_close(h);
    CHECK(sim_le_now() - start <= 10000);
    CHECK(sim_le_cancels(s) == 0 && sim_le_resources(s).closes == 1);
    CHECK(sim_le_resources(s).uninits == 1 && create_terminals == 0);
    CHECK(callbacks.connects == 0 && callbacks.disconnects == 0);
    transport.ops->pump(transport.ctx, 10000);
    if (!lost_terminal) {
        CHECK(sim_le_system_event(s, packet, sizeof packet) == 21);
        CHECK(packet[0] == 0x3e && packet[2] == 1 && memcmp(packet + 8, third_party, 6) == 0);
    }
    CHECK(sim_le_system_event(s, packet, sizeof packet) == 0);
    printf("PASS inherited-control LE-Create-ownership-through-terminal:"
           " Status=1 terminal-before-close=0 cancel=0 close=1 callbacks=0 late-system=%d\n",
           !lost_terminal);
    sim_le_free(s);
}

static const hci_ops *effect_base;
static int dhkey_mode, dhkey_requests, dhkey_checks, encryptions;
static int dhkey_cmd(void *ctx, unsigned op, const void *p, int n)
{
    if (op == 0x2026) {
        dhkey_requests++;
        CHECK(n == 64);
        if (dhkey_mode) {
            static const unsigned char refused[] = {0x0c, 1, 0x26, 0x20};
            static const unsigned char foreign[34] = {9, 0};
            sim_le_inject_event(ctx, 0x0f, refused, sizeof refused);
            if (dhkey_mode == 1) sim_le_inject_event(ctx, 0x3e, foreign, sizeof foreign);
            return 1; /* Delivered to HCI, refused there; no local DHKey was generated. */
        }
    }
    if (op == 0x2019) encryptions++;
    return effect_base->cmd(ctx, op, p, n);
}

static int dhkey_acl(void *ctx, const unsigned char *p, int n)
{
    if (n >= 9 && le16(p + 6) == 6 && p[8] == 0x0d) dhkey_checks++;
    return effect_base->acl_send(ctx, p, n);
}

static void pairing_state(void *ud, int slot, const pad_state *state)
{
    (void)slot; (void)state; ((seen *)ud)->states++;
}

/* Review Appendix 3 / T:630-655, with accepted and no-foreign-result controls. */
static void test_rejected_dhkey_cannot_claim_foreign_result(void)
{
    for (dhkey_mode = 0; dhkey_mode < 3; dhkey_mode++) {
        sim_le_pad p = {0};
        memcpy(p.addr, third_party, 6);
        p.sc = 1; p.vid = 0x045e; p.pid = 0x0b13;
        p.map[0] = 5; p.map_len = 1; p.report_len = 16;
        p.rid_in = 1; p.rid_out = 3; p.mtu = 185;
        sim_le_t *sim = sim_le_new(&p);
        hci_t io = sim_le_hci(sim);
        hci_ops ops = *io.ops;
        effect_base = io.ops; ops.cmd = dhkey_cmd; ops.acl_send = dhkey_acl; io.ops = &ops;
        seen callbacks = {0};
        host_events ev = {connected, pairing_state, disconnected, &callbacks, NULL};
        dhkey_requests = dhkey_checks = encryptions = 0;
        next_db();
        host_t *h = host_open(io, db, &ev);
        CHECK(h != NULL);
        if (h) {
            CHECK(host_pair(h, 30));
            long end = sim_le_now() + (dhkey_mode ? 1000 : 8000);
            while (sim_le_now() < end) host_poll(h, 5);
            CHECK(dhkey_requests > 0);
            if (!dhkey_mode) {
                CHECK(dhkey_checks > 0 && encryptions > 0 && sim_le_encrypted(sim));
                CHECK(callbacks.connects == 1 && callbacks.states > 0 && !sim_le_error(sim));
            } else if (dhkey_mode == 1) {
                EFFECT(14, "rejected-dhkey-cannot-claim-foreign-result", dhkey_checks == 0 && encryptions == 0);
                CHECK(callbacks.connects == 0 && callbacks.states == 0);
            } else CHECK(dhkey_checks == 0 && encryptions == 0 && callbacks.connects == 0);
            printf("DHKEY mode=%d attempts=%d smp_dhkey_checks=%d encryptions=%d connects=%d states=%d\n",
                   dhkey_mode, dhkey_requests, dhkey_checks, encryptions, callbacks.connects, callbacks.states);
            host_close(h);
        } else io.ops->close(io.ctx);
        CHECK(sim_le_resources(sim).closes == 1);
        sim_le_free(sim);
    }
}

static int reuse_mode, reuse_requests, reuse_checks, reuse_encryptions;
static int reuse_cmd(void *ctx, unsigned op, const void *p, int n)
{
    if (op == 0x2026) {
        reuse_requests++;
        /* Only the first (retired) request may answer: 500 ms later in mode 0,
         * never in mode 1, and normally in mode 2's retained original link. */
        sim_le_delay_events(ctx, 0x3e, 9, 1, reuse_requests == 1 && reuse_mode != 1 ? 500 : -1);
    }
    if (op == 0x2019) reuse_encryptions++;
    return effect_base->cmd(ctx, op, p, n);
}

static int reuse_acl(void *ctx, const unsigned char *p, int n)
{
    if (n >= 9 && le16(p + 6) == 6 && p[8] == 0x0d) reuse_checks++;
    return effect_base->acl_send(ctx, p, n);
}

/* Review r2 Appendix A / T:455-484: a retired link's delayed result must not
 * drive the replacement's SMP, with lost-result and retained-link controls. */
static void test_late_dhkey_cannot_claim_retired_result(void)
{
    for (reuse_mode = 0; reuse_mode < 3; reuse_mode++) {
        sim_le_pad p = {0};
        memcpy(p.addr, third_party, 6);
        p.sc = 1; p.vid = 0x045e; p.pid = 0x0b13;
        p.map[0] = 5; p.map_len = 1; p.report_len = 16;
        p.rid_in = 1; p.rid_out = 3; p.mtu = 185;
        sim_le_t *sim = sim_le_new(&p);
        hci_t io = sim_le_hci(sim);
        hci_ops ops = *io.ops;
        effect_base = io.ops; ops.cmd = reuse_cmd; ops.acl_send = reuse_acl; io.ops = &ops;
        seen callbacks = {0};
        host_events ev = {connected, pairing_state, disconnected, &callbacks, NULL};
        reuse_requests = reuse_checks = reuse_encryptions = 0;
        next_db();
        host_t *h = host_open(io, db, &ev);
        CHECK(h != NULL);
        if (h) {
            CHECK(host_pair(h, 30));
            long end = sim_le_now() + 1000;
            while (!reuse_requests && sim_le_now() < end) host_poll(h, 5);
            CHECK(reuse_requests == 1);
            if (reuse_mode != 2) {
                sim_le_power_off(sim);      /* real Disconnection Complete */
                host_poll(h, 5);
                sim_le_power_on(sim);       /* replacement through normal admission */
            }
            end = sim_le_now() + (reuse_mode == 2 ? 8000 : 1000);
            while (sim_le_now() < end) host_poll(h, 5);
            CHECK(sim_le_creates(sim) == (reuse_mode == 2 ? 1 : 2));
            if (!reuse_mode) {
                EFFECT(14, "late-dhkey-cannot-claim-retired-result",
                       reuse_checks == 0 && reuse_encryptions == 0);
                CHECK(callbacks.connects == 0 && callbacks.states == 0);
            } else if (reuse_mode == 1) {
                CHECK(reuse_checks == 0 && reuse_encryptions == 0);
                CHECK(callbacks.connects == 0 && callbacks.states == 0);
            } else {
                CHECK(reuse_checks > 0 && reuse_encryptions > 0);
                CHECK(callbacks.connects == 1 && callbacks.states > 0 && sim_le_encrypted(sim));
            }
            printf("LATE_REUSE mode=%d creates=%d dhkey_requests=%d smp_checks=%d encryptions=%d connects=%d states=%d\n",
                   reuse_mode, sim_le_creates(sim), reuse_requests, reuse_checks,
                   reuse_encryptions, callbacks.connects, callbacks.states);
            host_close(h);
        } else io.ops->close(io.ctx);
        CHECK(sim_le_resources(sim).closes == 1);
        sim_le_free(sim);
    }
}

static int foreign_order, inquiry_commands, inquiry_cancels;
static int foreign_create(void *ctx, unsigned op, const void *p, int n)
{
    static const unsigned char success[] = {0, 1, 0x0d, 0x20};
    if (op == 0x200d && foreign_order == 1)
        sim_le_inject_event(ctx, 0x0f, success, sizeof success);
    if (op == 0x200d && foreign_order == 3)
        sim_le_delay_events(ctx, 0x0f, -1, 1, -1);
    int result = effect_base->cmd(ctx, op, p, n);
    if (op == 0x200d && foreign_order >= 2)
        sim_le_inject_event(ctx, 0x0f, success, sizeof success);
    return result;
}

/* Appendix A and T:675-717: retain real busy refusals in all four orders. */
static void test_foreign_create_status_never_authorizes_cancel(void)
{
    static const char *const names[] = {
        "foreign-create-busy-control", "foreign-create-success-before-busy",
        "foreign-create-success-after-busy", "foreign-create-local-status-lost"
    };
    for (foreign_order = 0; foreign_order < 4; foreign_order++) {
        sim_le_pad p = {0};
        memcpy(p.addr, third_party, 6);
        p.vid = 0x045e; p.pid = 0x0b13; p.busy = 1;
        sim_le_t *sim = sim_le_new(&p);
        hci_t io = sim_le_hci(sim);
        hci_ops ops = *io.ops;
        effect_base = io.ops; ops.cmd = foreign_create; io.ops = &ops;
        seen callbacks = {0};
        host_events ev = observers(&callbacks);
        next_db();
        host_t *h = host_open(io, db, &ev);
        CHECK(h != NULL);
        if (!h) { io.ops->close(io.ctx); sim_le_free(sim); continue; }
        CHECK(host_pair(h, 30));
        long end = sim_le_now() + 200;
        while (!sim_le_creates(sim) && sim_le_now() < end) host_poll(h, 5);
        host_poll(h, 5);
        CHECK(sim_le_creates(sim) == 1);
        sim_le_busy_for(sim, 600);
        host_close(h);
        io.ops->pump(io.ctx, 600);
        if (foreign_order == 0 || foreign_order == 2)
            EFFECT(14, names[foreign_order], sim_le_cancels(sim) == 0 && sim_le_foreign_finishes(sim) == 1);
        else CHECK(sim_le_cancels(sim) == 0 && sim_le_foreign_finishes(sim) == 1);
        CHECK(sim_le_resources(sim).closes == 1);
        CHECK(callbacks.connects == 0 && callbacks.disconnects == 0);
        sim_le_free(sim);
    }
}

static int busy_inquiry(void *ctx, unsigned op, const void *p, int n)
{
    if (op == OP_INQUIRY) {
        static const unsigned char busy[] = {0x0c, 1, 1, 4};
        inquiry_commands++;
        sim_inject_event(ctx, 0x0f, busy, sizeof busy);
        return 1;
    }
    if (op == OP_INQUIRY_CANCEL) inquiry_cancels++;
    return effect_base->cmd(ctx, op, p, n);
}

static void test_foreign_inquiry_untouched(void)
{
    sim_pad p = pad_fixture();
    sim_t *sim = sim_new(&p);
    hci_t io = sim_hci(sim);
    hci_ops ops = *io.ops;
    effect_base = io.ops; ops.cmd = busy_inquiry; io.ops = &ops;
    inquiry_commands = inquiry_cancels = 0;
    next_db();
    host_t *h = host_open(io, db, NULL);
    CHECK(h != NULL);
    if (h) {
        CHECK(host_pair(h, 30));
        for (int i = 0; i < 5; i++) host_poll(h, 5);
        CHECK(inquiry_commands > 0);
        host_close(h);
    } else io.ops->close(io.ctx);
    EFFECT(14, "foreign-inquiry-untouched", inquiry_cancels == 0);
    CHECK(sim_resources(sim).closes == 1);
    sim_free(sim);
}

static int forbidden_commands;
static int observe_forbidden(void *ctx, unsigned op, const void *p, int n)
{
    if (op == 0x0402 || op == 0x0408 || op == 0x200e) forbidden_commands++;
    return effect_base->cmd(ctx, op, p, n);
}

static void test_raw_cancels_never_reach_controller(void)
{
    static const unsigned opcodes[] = {0x0402, 0x0408, 0x200e};
    sim_pad p = {0};
    sim_t *sim = sim_new(&p);
    hci_t io = sim_hci(sim);
    hci_ops ops = *io.ops;
    effect_base = io.ops; ops.cmd = observe_forbidden; io.ops = &ops;
    next_db();
    host_t *h = host_open(io, db, NULL);
    CHECK(h != NULL);
    if (h) {
        for (unsigned i = 0; i < 3; i++) {
            unsigned char reply[300], peer[6] = {1, 2, 3, 4, 5, 6};
            int n;
            forbidden_commands = 0;
            int result = host_raw_cmd(h, opcodes[i], peer, opcodes[i] == 0x0408 ? 6 : 0, reply, &n);
            char name[64];
            snprintf(name, sizeof name, "raw-forbidden-cancel-%04x", opcodes[i]);
            EFFECT(14, name, !result && forbidden_commands == 0);
        }
        host_close(h);
    } else io.ops->close(io.ctx);
    CHECK(sim_resources(sim).closes == 1);
    sim_free(sim);
}

static int late_disconnects;
static int suppress_disconnect(void *ctx, unsigned op, const void *p, int n)
{
    if (op == OP_DISCONNECT) { late_disconnects++; return 1; }
    return effect_base->cmd(ctx, op, p, n);
}

static void test_late_link_during_close(void)
{
    sim_pad p = pad_fixture();
    sim_t *sim = sim_new(&p);
    hci_t io = sim_hci(sim);
    hci_ops ops = *io.ops;
    effect_base = io.ops; ops.cmd = suppress_disconnect; io.ops = &ops;
    seen callbacks = {0};
    host_events ev = observers(&callbacks);
    next_db();
    host_t *h = host_open(io, db, &ev);
    CHECK(h != NULL);
    if (!h) { io.ops->close(io.ctx); sim_free(sim); return; }
    sim_delay_events(sim, 0x03, 1, 300);
    CHECK(host_pair(h, 60));
    long end = sim_now() + 1000;
    while (!sim_create_requests(sim) && sim_now() < end) host_poll(h, 5);
    CHECK(sim_create_requests(sim) == 1);
    late_disconnects = 0;
    long start = sim_now();
    host_close(h);
    CHECK(sim_now() - start <= 10000);
    CHECK(late_disconnects == 0 && sim_resources(sim).closes == 1);
    CHECK(sim_foreign_commands(sim) == 0 && callbacks.connects == 0 && callbacks.disconnects == 0);
    io.ops->pump(io.ctx, 300);
    unsigned char packet[300];
    CHECK(sim_system_event(sim, packet, sizeof packet) == 13);
    CHECK(packet[0] == 0x03 && !memcmp(packet + 5, third_party, 6));
    sim_free(sim);
}

static int connection_le, connection_mode, connection_sends, connection_disconnects;
static int connection_cmd(void *ctx, unsigned op, const void *p, int n)
{
    if (op == (connection_le ? 0x200dU : OP_CREATE_CONNECTION)) {
        connection_sends++;
        if (connection_mode == 0) return 0;
        if (connection_le)
            sim_le_delay_events(ctx, connection_mode == 1 ? 0x0f : 0x3e,
                                connection_mode == 1 ? -1 : 1, 1, -1);
        else sim_delay_events(ctx, connection_mode == 1 ? 0x0f : 0x03, 1, -1);
    }
    if (op == OP_DISCONNECT) connection_disconnects++;
    return effect_base->cmd(ctx, op, p, n);
}

static void test_unacknowledged_connections_never_authorize_disconnect(void)
{
    for (connection_le = 0; connection_le < 2; connection_le++)
        for (connection_mode = 0; connection_mode < 3; connection_mode++) {
            sim_pad p = pad_fixture();
            sim_le_pad lp = {0};
            memcpy(lp.addr, third_party, 6); lp.vid = 0x045e; lp.pid = 0x0b13;
            void *sim = connection_le ? (void *)sim_le_new(&lp) : (void *)sim_new(&p);
            hci_t io = connection_le ? sim_le_hci(sim) : sim_hci(sim);
            hci_ops ops = *io.ops;
            effect_base = io.ops; ops.cmd = connection_cmd; io.ops = &ops;
            connection_sends = connection_disconnects = 0;
            seen callbacks = {0};
            host_events ev = observers(&callbacks);
            next_db();
            host_t *h = host_open(io, db, &ev);
            CHECK(h != NULL);
            if (h) {
                CHECK(host_pair(h, 30));
                long end = now_ms() + 1000;
                while (!connection_sends && now_ms() < end) host_poll(h, 5);
                CHECK(connection_sends == 1);
                host_poll(h, 1);
                host_close(h);
            } else io.ops->close(io.ctx);
            char name[80];
            snprintf(name, sizeof name, "unacknowledged-connection-%s-mode-%d",
                     connection_le ? "LE" : "Classic", connection_mode);
            if (connection_mode == 1)
                EFFECT(14, name, connection_disconnects == 0 && callbacks.connects == 0);
            else CHECK(connection_disconnects == 0 && callbacks.connects == 0);
            CHECK((connection_le ? sim_le_resources(sim).closes : sim_resources(sim).closes) == 1);
            if (connection_le) {
                CHECK(sim_le_cancels(sim) == 0);
                sim_le_free(sim);
            } else sim_free(sim);
        }
}

enum { SET_REFUSED, SET_ERROR, SET_LOST, SET_ACKED };
static unsigned setting_target;
static int setting_mode, setting_enabled, setting_attempts, setting_cleanup, closing_effects;
static int setting_cmd(void *ctx, unsigned op, const void *p, int n)
{
    if (closing_effects && (op == setting_target ||
        (setting_target == OP_WRITE_SCAN_ENABLE && op == OP_READ_SCAN_ENABLE) ||
        (setting_target == OP_WRITE_PAGE_SCAN_ACT && op == OP_READ_PAGE_SCAN_ACT)))
        setting_cleanup++;
    if (!closing_effects && setting_enabled && op == setting_target) {
        setting_attempts++;
        if (setting_mode == SET_REFUSED) return 0;
        if (setting_mode == SET_ERROR) {
            unsigned char refused[] = {1, (unsigned char)op, (unsigned char)(op >> 8), 0x0c};
            sim_inject_event(ctx, 0x0e, refused, sizeof refused);
            return 1;
        }
        if (setting_mode == SET_LOST) sim_drop_events(ctx, 0x0e, 1);
    }
    return effect_base->cmd(ctx, op, p, n);
}

static void test_only_acknowledged_settings_authorize_restoration(void)
{
    static const unsigned targets[] = {OP_SET_EVENT_MASK, OP_WRITE_SCAN_ENABLE, OP_WRITE_PAGE_SCAN_ACT, 0x200c};
    for (unsigned i = 0; i < 4; i++) for (int mode = SET_REFUSED; mode <= SET_ACKED; mode++) {
        sim_pad p = {0};
        sim_t *sim = sim_new(&p);
        hci_t io = sim_hci(sim);
        hci_ops ops = *io.ops;
        effect_base = io.ops; ops.cmd = setting_cmd; io.ops = &ops;
        setting_target = targets[i]; setting_mode = mode;
        setting_attempts = setting_cleanup = closing_effects = 0;
        setting_enabled = i < 2;
        next_db();
        host_t *h = host_open(io, db, NULL);
        if (i >= 2) {
            CHECK(h != NULL);
            if (h) {
                setting_enabled = 1;
                if (i == 2) sim_set_page_scan(sim, 0x029c, 0x014e);
                CHECK(host_pair(h, 30));
                if (i == 2) host_poll(h, 5); else le_poll(h, sim_now());
            }
        } else CHECK((h != NULL) == (mode == SET_ACKED));
        CHECK(setting_attempts > 0);
        closing_effects = 1;
        if (h) host_close(h);
        char name[80];
        snprintf(name, sizeof name, "setting-%04x-mode-%d", targets[i], mode);
        /* A wire ACK without the A8 barrier is not an undo lease. */
        if (h)
            EFFECT(13, name, setting_cleanup == 0);
        else CHECK(setting_cleanup == 0);
        if (!h)
            EFFECT(15, name, sim_resources(sim).closes == 1);
        else CHECK(sim_resources(sim).closes == 1);
        io.ops->close(io.ctx);
        CHECK(sim_resources(sim).closes == 1 && sim_foreign_commands(sim) == 0);
        sim_free(sim);
    }
}

static void test_failed_setting_rewrite_retires_previous_authority(void)
{
    static const unsigned targets[] = {OP_SET_EVENT_MASK, OP_WRITE_SCAN_ENABLE, OP_WRITE_PAGE_SCAN_ACT, 0x200c};
    static const unsigned char mask[8] = {0xf7,0xff,0xff,0xff,0xff,0xff,0xff,0x3f};
    static const unsigned char scan[] = {1}, page[] = {0,4,0x12,0}, le[] = {0,0};
    const unsigned char *params[] = {mask, scan, page, le};
    const int lengths[] = {8, 1, 4, 2};
    for (unsigned i = 0; i < 4; i++) {
        sim_pad p = {0};
        sim_t *sim = sim_new(&p);
        hci_t io = sim_hci(sim);
        hci_ops ops = *io.ops;
        effect_base = io.ops; ops.cmd = setting_cmd; io.ops = &ops;
        setting_target = targets[i]; setting_mode = SET_LOST;
        setting_enabled = setting_attempts = setting_cleanup = closing_effects = 0;
        next_db();
        host_t *h = host_open(io, db, NULL);
        CHECK(h != NULL);
        if (!h) { io.ops->close(io.ctx); sim_free(sim); continue; }
        if (i >= 2) {
            if (i == 2) sim_set_page_scan(sim, 0x029c, 0x014e);
            CHECK(host_pair(h, 30));
            if (i == 2) host_poll(h, 5); else le_poll(h, sim_now());
        }
        {
            unsigned char reply[300];
            int n;
            CHECK(host_raw_cmd(h, 0x1009, NULL, 0, reply, &n));
        }
        setting_enabled = 1;
        CHECK(!hci_sync(h, targets[i], params[i], lengths[i]));
        closing_effects = 1;
        host_close(h);
        char name[80];
        snprintf(name, sizeof name, "failed-setting-rewrite-%04x", targets[i]);
        EFFECT(13, name, setting_cleanup == 0);
        CHECK(sim_resources(sim).closes == 1);
        sim_free(sim);
    }
}

static void test_refused_scan_does_not_flood_or_acquire_a_lease(void)
{
    sim_pad p = {0};
    sim_t *sim = sim_new(&p);
    hci_t io = sim_hci(sim);
    hci_ops ops = *io.ops;
    effect_base = io.ops; ops.cmd = setting_cmd; io.ops = &ops;
    setting_target = 0x200c; setting_mode = SET_REFUSED;
    setting_enabled = setting_attempts = setting_cleanup = closing_effects = 0;
    next_db();
    host_t *h = host_open(io, db, NULL);
    CHECK(h != NULL);
    if (!h) { io.ops->close(io.ctx); sim_free(sim); return; }
    CHECK(host_pair(h, 30)); setting_enabled = 1;
    for (int i = 0; i < 50; i++) le_poll(h, sim_now());
    CHECK(setting_attempts == 1);
    closing_effects = 1; host_close(h);
    EFFECT(13, "refused-scan-no-cleanup", setting_cleanup == 0);
    CHECK(sim_resources(sim).closes == 1);
    sim_free(sim);
}

static unsigned ack_target;
static int ack_reject, ack_cleanup, ack_refusals, ack_closing, ack_delay;
static int ack_cmd(void *ctx, unsigned op, const void *p, int n)
{
    if (op == ack_target) {
        CHECK(n == (op == 0x200c ? 2 : 8));
        if (ack_closing) {
            ack_cleanup++;
            if (op == 0x200c) CHECK(((const unsigned char *)p)[0] == 0);
        } else if (ack_reject) {
            unsigned char refused[] = {1, (unsigned char)op, (unsigned char)(op >> 8), 0x0c};
            ack_refusals++;
            CHECK(((const unsigned char *)p)[0] == (op == 0x200c ? 1 : 0xf7));
            sim_schedule_event(ctx, 0x0e, refused, sizeof refused, ack_delay);
            return 1;
        }
    }
    return effect_base->cmd(ctx, op, p, n);
}

/* G/H: preserve old wire ACKs; use the real scan caller instead of a private lease flag. */
static void test_old_setting_ack_never_authorizes_replacement(void)
{
    static const unsigned targets[] = {0x200c, OP_SET_EVENT_MASK};
    static const unsigned char off[] = {0, 0};
    static const unsigned char old_mask[] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3f};
    static const unsigned char new_mask[] = {0xf7,0xff,0xff,0xff,0xff,0xff,0xff,0x3f};
    for (unsigned family = 0; family < 2; family++)
        for (int appendix = 0; appendix < 2; appendix++)
            for (int interleaved = 0; interleaved < 2; interleaved++) {
                sim_pad p = {0};
                sim_t *sim = sim_new(&p);
                hci_t io = sim_hci(sim);
                hci_ops ops = *io.ops;
                effect_base = io.ops; ops.cmd = ack_cmd; io.ops = &ops;
                ack_target = targets[family];
                ack_reject = ack_cleanup = ack_refusals = ack_closing = 0;
                ack_delay = appendix == 0 && interleaved ? 2000 : 0;
                next_db();
                host_t *h = host_open(io, db, NULL);
                CHECK(h != NULL);
                if (!h) { io.ops->close(io.ctx); sim_free(sim); continue; }
                if (appendix == 0 && interleaved) sim_delay_events(sim, 0x0e, 3, 6000);
                int old_result = hci_sync(h, ack_target, family ? old_mask : off, family ? 8 : 2);
                CHECK(old_result == !(appendix == 0 && interleaved));
                if (appendix == 1 && interleaved) {
                    unsigned char duplicate[] = {1, (unsigned char)ack_target, (unsigned char)(ack_target >> 8), 0};
                    sim_inject_event(sim, 0x0e, duplicate, sizeof duplicate);
                }
                ack_reject = 1;
                if (family) (void)hci_sync(h, ack_target, new_mask, sizeof new_mask);
                else { CHECK(host_pair(h, 30)); le_poll(h, sim_now()); }
                io.ops->pump(io.ctx, 2000);
                unsigned char reply[300];
                int n;
                (void)host_raw_cmd(h, 0x1009, NULL, 0, reply, &n);
                CHECK(ack_refusals == 1);
                ack_closing = 1;
                host_close(h);
                char name[80];
                snprintf(name, sizeof name, "appendix-%c-%04x-%s",
                         appendix ? 'H' : 'G', ack_target,
                         interleaved ? (appendix ? "duplicate" : "delayed") : "control");
                EFFECT(13, name, ack_cleanup == 0);
                CHECK(sim_resources(sim).closes == 1);
                sim_free(sim);
            }
}

static const hci_ops *quarantine_base;
static unsigned quarantine_handles[2];
static int quarantine_connections, quarantine_terminals;

static int quarantine_event(void *ctx, unsigned char *out, int max)
{
    int n = quarantine_base->next_event(ctx, out, max);
    if (n == 13 && out[0] == 0x03 && out[2] == 0) {
        if (quarantine_connections < 2)
            quarantine_handles[quarantine_connections] = le16(out + 3);
        quarantine_connections++;
    }
    if (n == 6 && out[0] == 0x05 && out[2] == 0) quarantine_terminals++;
    return n;
}

/* Review r3 B5: a lost terminal cannot grant authority over a reused wire handle. */
static void test_reused_handle_without_terminal_never_disconnects(void)
{
    for (int lost = 0; lost < 2; lost++) {
        sim_pad p = pad_fixture();
        sim_t *sim = sim_new(&p);
        hci_t io = sim_hci(sim);
        hci_ops ops = *io.ops;
        seen observed = {0};
        host_events ev = {connected, pairing_state, disconnected, &observed, NULL};
        quarantine_base = io.ops;
        ops.next_event = quarantine_event;
        io.ops = &ops;
        quarantine_connections = quarantine_terminals = 0;
        memset(quarantine_handles, 0, sizeof quarantine_handles);
        next_db();
        host_t *h = host_open(io, db, &ev);
        CHECK(h != NULL);
        if (h) {
            CHECK(host_pair(h, 30));
            long end = sim_now() + 8000;
            while ((!observed.connects || !observed.states) && sim_now() < end)
                host_poll(h, 5);
            CHECK(observed.connects == 1 && observed.states > 0);
            CHECK(quarantine_connections == 1 && quarantine_handles[0] != 0);
            int inputs = observed.states;
            if (lost) {
                sim_vanish(sim);
                CHECK(observed.disconnects == 0 && quarantine_terminals == 0);
                sim_power_on(sim);
                end = sim_now() + 8000;
                while ((observed.connects < 2 || observed.states == inputs) && sim_now() < end)
                    host_poll(h, 5);
                CHECK(observed.connects == 2 && observed.states > inputs);
                CHECK(quarantine_connections == 2 && quarantine_handles[0] == quarantine_handles[1]);
            }
            CHECK(quarantine_terminals == 0 && sim_disconnect_requests(sim) == 0);
            host_close(h);
            int wire_disconnects = sim_disconnect_requests(sim);
            printf("HANDLE_REUSE lost_terminal=%d connections=%d handles=%04x/%04x"
                   " wire_disconnects=%d callbacks=%d/%d inputs=%d->%d\n",
                   lost, quarantine_connections, quarantine_handles[0], quarantine_handles[1],
                   wire_disconnects, observed.connects, observed.disconnects, inputs, observed.states);
            if (lost)
                EFFECT(14, "reused-handle-without-terminal-never-disconnects", wire_disconnects == 0);
            else CHECK(wire_disconnects == 1 && observed.disconnects == 1 && quarantine_terminals == 1);
        } else io.ops->close(io.ctx);
        CHECK(sim_resources(sim).closes == 1 && sim_resources(sim).uninits == 1);
        sim_free(sim);
    }
}

static void test_retained_completion_reply(void)
{
    sim_pad p = pad_fixture();
    sim_t *sim = sim_new(&p);
    hci_t io = sim_hci(sim);
    next_db();
    host_t *h = host_open(io, db, NULL);
    CHECK(h != NULL);
    if (h) {
        static const unsigned char unrelated[] = {1, 0x03, 0x10, 0};
        static const unsigned char wanted[] = {0, 0, 8, 0x12, 0};
        unsigned char reply[300];
        int n;
        sim_schedule_event(sim, 0x0e, unrelated, sizeof unrelated, 1);
        int result = host_raw_cmd(h, 0x0c1b, NULL, 0, reply, &n);
        EFFECT(15, "retained-completion-reply", result && n == sizeof wanted && !memcmp(reply, wanted, sizeof wanted));
        host_close(h);
    } else io.ops->close(io.ctx);
    CHECK(sim_resources(sim).closes == 1);
    sim_free(sim);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int strict = argc == 2 && strcmp(argv[1], "--expected-failures") == 0;
    if (argc > 2 || (argc == 2 && !strict)) return 2;
    strict_mode = strict;
    if (!mkdtemp(directory)) { perror("mkdtemp"); return 1; }
    test_consume_once_and_distinct_peers();
    test_classic_terminal_order();
    test_expected_current_native_request_does_not_release(0);
    test_expected_current_native_request_does_not_release(1);
    test_expected_current_native_request_does_not_release(2);
    test_expected_current_partial_startup_leaks_transport();
    test_classic_close_callbacks(0);
    test_classic_close_callbacks(1);
    test_le_terminal_seams();
    test_le_status_retains_pending_ownership(0);
    test_le_status_retains_pending_ownership(1);
    test_rejected_dhkey_cannot_claim_foreign_result();
    test_late_dhkey_cannot_claim_retired_result();
    test_foreign_create_status_never_authorizes_cancel();
    test_foreign_inquiry_untouched();
    test_raw_cancels_never_reach_controller();
    test_late_link_during_close();
    test_unacknowledged_connections_never_authorize_disconnect();
    test_only_acknowledged_settings_authorize_restoration();
    test_failed_setting_rewrite_retires_previous_authority();
    test_refused_scan_does_not_flood_or_acquire_a_lease();
    test_old_setting_ack_never_authorizes_replacement();
    test_reused_handle_without_terminal_never_disconnects();
    test_retained_completion_reply();
    clear_db();
    CHECK(rmdir(directory) == 0);
    ph_clock_hook = NULL;
    printf("%d session seam checks, %d failed; %d expected-failing cases%s\n",
           checks, failures, pending, strict ? " (strict)" : "");
    return failures != 0 || (strict && pending != 0);
}
