/* Lifecycle seams for tasks 3-4. No replacement runtime policy lives here. */
#include "sim.h"
#include "sim_le.h"
#include "../src/host_int.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks, pending;
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
    printf("%s expected-failing-until-task-%d %s\n", desired ? "XPASS" : "XFAIL", task, name);
    if (!desired) pending++;
}

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

typedef struct { int connects, disconnects; } seen;
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

static void test_expected_current_le_status_loses_pending_ownership(void)
{
    sim_le_pad p = {0};
    sim_le_t *s;
    hci_t transport;
    host_t *h;
    seen callbacks = {0};
    host_events ev = observers(&callbacks);
    unsigned char packet[300];
    long deadline;
    int i, found = 0;
    next_db();
    memcpy(p.addr, third_party, 6);
    p.vid = 0x045e; p.pid = 0x0b13;
    s = sim_le_new(&p);
    transport = sim_le_hci(s);
    h = host_open(transport, db, &ev);
    CHECK(h != NULL);
    if (!h) { transport.ops->close(transport.ctx); sim_le_free(s); return; }
    sim_le_delay_events(s, 0x3e, 1, 1, 10000);
    CHECK(host_pair(h, 30));
    deadline = sim_le_now() + 1000;
    while (!sim_le_creates(s) && sim_le_now() < deadline) host_poll(h, 5);
    CHECK(sim_le_creates(s) == 1 && callbacks.connects == 0);
    for (i = 0; i < HOST_MAX_PADS; i++) {
        link_t *l = &h->links[i];
        if (!l->used || !l->le) continue;
        found++;
        CHECK(!l->connected && !l->le_s.create_out); /* current Command Status behavior */
        expected_failure(3, "LE-Create-ownership-through-terminal", l->le_s.create_out != 0);
    }
    CHECK(found == 1);
    host_close(h);
    CHECK(sim_le_cancels(s) == 0 && sim_le_resources(s).closes == 1);
    CHECK(callbacks.connects == 0 && callbacks.disconnects == 0);
    transport.ops->pump(transport.ctx, 10000);
    CHECK(sim_le_system_event(s, packet, sizeof packet) == 21);
    CHECK(packet[0] == 0x3e && packet[2] == 1 && memcmp(packet + 8, third_party, 6) == 0);
    CHECK(sim_le_system_event(s, packet, sizeof packet) == 0);
    printf("SESSION expected-current LE shutdown: Create -> Status(success, create_out=0)"
           " -> host_close(cancel=0 close=1) -> +10000ms late Connection Complete(system=1, callbacks=0)\n");
    sim_le_free(s);
}

int main(int argc, char **argv)
{
    int strict = argc == 2 && strcmp(argv[1], "--expected-failures") == 0;
    if (argc > 2 || (argc == 2 && !strict)) return 2;
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
    test_expected_current_le_status_loses_pending_ownership();
    clear_db();
    CHECK(rmdir(directory) == 0);
    ph_clock_hook = NULL;
    printf("%d session seam checks, %d failed; %d expected-failing-until-task-3/4 cases%s\n",
           checks, failures, pending, strict ? " (strict)" : "");
    return failures != 0 || (strict && pending != 0);
}
