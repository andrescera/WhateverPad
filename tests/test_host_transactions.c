/* GPL-3.0-or-later.
 * Standalone acceptance tests: link $(CORE), tests/sim.c, tests/sim_le.c.
 * Compile every translation unit with the same WHATEVERPAD_ATTRIB_POLICY.
 * Default: NO_RESTORE. Optional: -DWHATEVERPAD_ATTRIB_POLICY=OWN_POLICY_SINGLE_PRODUCER.
 * Expected wire effects come from model observations, never the ownership ledger.
 */
#include "sim.h"
#include "sim_le.h"
#include "../src/host_int.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
static const char *test_name;
#define CHECK(c) do { \
    checks++; \
    if (!(c)) { \
        fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, test_name, #c); \
        failures++; \
    } \
} while (0)

enum {
    INQUIRY_CANCEL = 0x0402, CREATE_CANCEL = 0x0408,
    DISCONNECT = 0x0406, NAME_CANCEL = 0x041a,
    EVENT_MASK = 0x0c01, RESET = 0x0c03, SCAN_ENABLE = 0x0c1a,
    SSP_WRITE = 0x0c56, SUPPORTED = 0x1002, READ_ADDRESS = 0x1009,
    LE_CANCEL = 0x200e, RAND = 0x2018,
    MAX_COMMANDS = 512
};

typedef struct {
    unsigned op, invocation, completions;
    int len;
    unsigned char params[8];
} wire_command;

typedef struct {
    sim_t *classic;
    sim_le_t *le;
    host_t *host;
    hci_t transport;
    char dir[80], db[100];
    wire_command commands[MAX_COMMANDS];
    unsigned ncommands, capability, barrier_due, rand_pending;
    unsigned rand_started, rand_completed;
    unsigned char randoms[256][8];
    unsigned fault_op, fault_invocation, fault_left;
    int fault_armed, fault_delay, closing;
    unsigned handle;
    int has_handle;
    int connects, states, disconnects;
    link_ref current, first, disconnected;
    pad_info info;
    pad_state state;
    int forget_on_connect, forget_on_query, reenter_disconnect, forget_result, reenter_result;
    int inspect_retirement;
} fixture;

static int single_producer(void)
{
    return WHATEVERPAD_ATTRIB_POLICY == OWN_POLICY_SINGLE_PRODUCER;
}

static int same_link(link_ref a, link_ref b)
{
    return a.attempt == b.attempt && a.generation == b.generation && a.slot == b.slot;
}

static int setting(unsigned op)
{
    switch (op) {
    case EVENT_MASK: case SCAN_ENABLE: case 0x0c1c:
    case 0x2001: case 0x200b: case 0x200c:
        return 1;
    default:
        return 0;
    }
}

static int needs_barrier(unsigned op)
{
    return setting(op) || op == 0x0405 || op == 0x0409 || op == 0x200d;
}

static int forbidden(unsigned op)
{
    return op == RESET || op == INQUIRY_CANCEL || op == CREATE_CANCEL ||
           op == NAME_CANCEL || op == LE_CANCEL || op == SSP_WRITE;
}

static unsigned count(const fixture *f, unsigned op)
{
    unsigned n = 0;
    for (unsigned i = 0; i < f->ncommands; i++) n += f->commands[i].op == op;
    return n;
}

static wire_command *invocation(fixture *f, unsigned id)
{
    for (unsigned i = 0; i < f->ncommands; i++)
        if (f->commands[i].invocation == id) return &f->commands[i];
    return NULL;
}

static void observe(void *ud, enum sim_observation kind, sim_origin origin,
                    unsigned code, const unsigned char *p, int n)
{
    fixture *f = ud;
    if (kind == SIM_COMMAND) {
        CHECK(!origin.foreign && !origin.copy && origin.invocation != 0);
        CHECK(!forbidden(code));
        if (setting(code) || code == RAND) CHECK(f->capability);
        if (!f->closing) {
            CHECK(f->rand_pending == 0 ||
                  (code == RAND && f->fault_op == RAND && f->fault_left &&
                   count(f, RAND) < 3));
            if (f->barrier_due) CHECK(code == RAND);
            if (code == RAND) {
                CHECK(f->barrier_due || (f->rand_pending && f->fault_op == RAND));
                f->barrier_due = 0;
                f->rand_pending = origin.invocation;
                f->rand_started++;
            }
        }
        CHECK(f->ncommands < MAX_COMMANDS);
        if (f->ncommands == MAX_COMMANDS) return;
        wire_command *c = &f->commands[f->ncommands++];
        c->op = code;
        c->invocation = origin.invocation;
        c->len = n;
        if (n > 0) {
            size_t len = (size_t)n < sizeof c->params ? (size_t)n : sizeof c->params;
            memcpy(c->params, p, len);
        }
        /* Subscribe before model_command generates this invocation's response. */
        if (f->fault_armed && code == f->fault_op) {
            if (f->fault_left) f->fault_left--;
            f->fault_armed = f->fault_left != 0;
            f->fault_invocation = origin.invocation;
            if (f->le) sim_le_delay_events(f->le, 0x0e, -1, 1, f->fault_delay);
            else sim_delay_events(f->classic, 0x0e, 1, f->fault_delay);
        }
        if (f->forget_on_query && code == READ_ADDRESS) {
            f->forget_on_query = 0;
            f->forget_result = host_forget(f->host, f->info.addr);
        }
        return;
    }
    if (kind != SIM_DELIVERED) return;
    if (code == 3 && n == 11 && !p[0]) {
        f->has_handle = 1;
        f->handle = le16(p + 1);
    }
    if (code == 0x3e && n >= 4 && (p[0] == 1 || p[0] == 0x0a) && !p[1]) {
        f->has_handle = 1;
        f->handle = le16(p + 2);
    }
    if (code != 0x0e && code != 0x0f) return;
    CHECK(n >= 4);
    if (n < 4) return;
    unsigned op = le16(p + (code == 0x0e ? 1 : 2));
    unsigned status = p[code == 0x0e ? 3 : 0];
    wire_command *c = invocation(f, origin.invocation);
    CHECK(c != NULL && !origin.foreign && !origin.copy);
    if (!c) return;
    CHECK(c->op == op);
    c->completions++;
    if (op == SUPPORTED && code == 0x0e && !status) {
        /* The actual model answer, not a readback or a ledger capability bit. */
        CHECK(n == 68 && (p[4 + 27] & 0x80));
        if (n == 68 && (p[4 + 27] & 0x80)) f->capability = 1;
    }
    if (f->closing) return;
    if (!status && needs_barrier(op)) {
        CHECK(!f->barrier_due && !f->rand_pending);
        f->barrier_due = origin.invocation;
    }
    if (op == RAND) {
        CHECK(code == 0x0e && n == 12 && !status);
        if (f->fault_op == RAND && count(f, RAND) == 3)
            CHECK(!host_admits(f->host)); /* Every delayed fence is already quarantined. */
        else CHECK(f->rand_pending == origin.invocation);
        if (n == 12 && !status) {
            CHECK(f->rand_completed < 256);
            if (f->rand_completed < 256) {
                for (unsigned i = 0; i < f->rand_completed; i++)
                    CHECK(memcmp(f->randoms[i], p + 4, 8) != 0);
                memcpy(f->randoms[f->rand_completed++], p + 4, 8);
            }
        }
        f->rand_pending = 0;
    }
}

static void connected(void *ud, link_ref link, const pad_info *info)
{
    fixture *f = ud;
    CHECK(link.attempt && link.generation && link.slot < HOST_MAX_PADS);
    CHECK(f->connects == f->disconnects);
    if (!f->connects) f->first = link;
    f->connects++;
    f->current = link;
    f->info = *info;
    if (f->forget_on_connect) {
        f->forget_on_connect = 0;
        f->forget_result = host_forget(f->host, info->addr);
    }
}

static void input(void *ud, link_ref link, const pad_state *state)
{
    fixture *f = ud;
    CHECK(f->connects == f->disconnects + 1);
    CHECK(same_link(link, f->current));
    f->states++;
    f->state = *state;
}

static void disconnected(void *ud, link_ref link)
{
    fixture *f = ud;
    CHECK(f->disconnects < f->connects);
    CHECK(same_link(link, f->current));
    f->disconnects++;
    f->disconnected = link;
    if (f->inspect_retirement) {
        pad_info info;
        pad_state state;
        host_paired_info paired;
        CHECK(!host_connected(f->host, link.slot));
        CHECK(!host_pad_get(f->host, link.slot, &info, &state));
        CHECK(host_paired_get(f->host, 0, &paired) && !paired.connected);
    }
    if (f->reenter_disconnect) {
        f->reenter_disconnect = 0;
        f->reenter_result = host_forget(f->host, f->info.addr);
    }
}

static const unsigned char descriptor[] = {
    5,1,9,5,0xa1,1, 0x15,0,0x26,0xff,0,0x75,8,0x95,4,
    9,0x30,9,0x31,9,0x32,9,0x35,0x81,2,
    0x15,0,0x25,7,0x75,4,0x95,1,9,0x39,0x81,0x42,
    0x75,4,0x95,1,0x81,3, 5,9,0x19,1,0x29,0x10,
    0x15,0,0x25,1,0x75,1,0x95,0x10,0x81,2,0xc0
};

/* kind: 0 DS4, 1 8BitDo generic HID, 2 Xbox Series LE Secure Connections. */
static void begin_with_pairing_mode(fixture *f, int kind, int simple_pairing)
{
    memset(f, 0, sizeof *f);
    printf("CASE %s kind=%d\n", test_name, kind);
    snprintf(f->dir, sizeof f->dir, "/tmp/whateverpad-transactions-XXXXXX");
    if (!mkdtemp(f->dir)) { perror("mkdtemp"); exit(1); }
    snprintf(f->db, sizeof f->db, "%s/bonds", f->dir);
    if (kind == 2) {
        sim_le_pad p = {
            .addr = {0x51,0x52,0x53,0x54,0x55,0xd6},
            .sc = 1, .vid = 0x045e, .pid = 0x0b13,
            .rid_in = 1, .rid_out = 3, .mtu = 185, .seed = 19,
            .map = {5}, .map_len = 1,
            .report = {0xff,0xff,0,0x80,0,0x80,0,0x80,0,0,0,0,0,1,0,1},
            .report_len = 16
        };
        f->le = sim_le_new(&p);
        if (!f->le) exit(1);
        f->transport = sim_le_hci(f->le);
    } else {
        sim_pad p = {
            .mode = SIM_WAIT_PAIRING, .addr = {1,2,3,4,5,6},
            .cod = 0x002508, .vid = 0x054c, .pid = 0x05c4,
            .report_len = 78, .needs_output = 1, .seed = 17
        };
        p.report[0] = 0x11; p.report[1] = 0xc0;
        p.report[3] = 0x20; p.report[4] = 0xe0;
        p.report[5] = p.report[6] = 0x80; p.report[7] = 0x20;
        p.report[10] = 0x80; p.report[32] = 8;
        if (kind == 1) {
            static const unsigned char report[] = {0xff,0x80,0x80,0x80,4,1,0};
            p.vid = 0x2dc8; p.pid = 0x6001; p.needs_output = 0;
            memcpy(p.desc, descriptor, sizeof descriptor);
            p.desc_len = sizeof descriptor;
            memcpy(p.report, report, sizeof report);
            p.report_len = sizeof report;
        }
        f->classic = sim_new(&p);
        if (!f->classic) exit(1);
        f->transport = sim_hci(f->classic);
    }
    /* Configure the system's preexisting SSP state before binding the host;
     * the bridge must never change an OFF baseline irreversibly. */
    unsigned char enabled = (unsigned char)simple_pairing, event[HCI_PKT_MAX];
    CHECK(f->transport.ops->cmd(f->transport.ctx, SSP_WRITE, &enabled, 1));
    int n = f->transport.ops->next_event(f->transport.ctx, event, sizeof event);
    CHECK(n == 6 && event[0] == 0x0e && le16(event + 3) == SSP_WRITE && !event[5]);
    CHECK(f->transport.ops->next_event(f->transport.ctx, event, sizeof event) == 0);
    attrib_reset();
    if (f->le) sim_le_observe(f->le, observe, f);
    else sim_observe(f->classic, observe, f);
}

static void begin(fixture *f, int kind) { begin_with_pairing_mode(f, kind, 1); }

static int prepare(fixture *f)
{
    host_events events = {
        .on_connect = connected, .on_state = input,
        .on_disconnect = disconnected, .ud = f
    };
    f->host = host_prepare(f->transport, f->db, &events);
    CHECK(f->host != NULL);
    CHECK(f->ncommands == 0);          /* Binding precedes the first protocol effect. */
    if (!f->host) return 0;
    if (f->le) sim_le_bind_ownership(f->le, host_ownership(f->host));
    else sim_bind_ownership(f->classic, host_ownership(f->host));
    return 1;
}

static int start(fixture *f)
{
    if (!prepare(f)) return 0;
    int ok = host_start(f->host);
    CHECK(ok);
    if (ok) {
        /* A synchronous setting can return after its CC while the final
         * Rand is still queued. Await the observed wire fence, not a lease. */
        for (unsigned step = 0; step < 400 && (f->barrier_due || f->rand_pending); step++)
            host_poll(f->host, 5);
        CHECK(f->ncommands > 0 && f->commands[0].op == SUPPORTED);
        CHECK(f->capability);
        CHECK(count(f, EVENT_MASK) == 1);
        CHECK(count(f, SCAN_ENABLE) == 1);
        CHECK(f->rand_started > 0 && f->rand_started == f->rand_completed);
        CHECK(!f->barrier_due && !f->rand_pending);
    }
    return ok;
}

static long clock_now(const fixture *f)
{
    return f->le ? sim_le_now() : sim_now();
}

/* Virtual time only. The iteration bound also catches a nonadvancing clock. */
static void pump_until(fixture *f, long end, int wanted_states, int wanted_disconnects)
{
    for (unsigned step = 0; step < 20000 && clock_now(f) < end; step++) {
        if ((wanted_states >= 0 && f->states >= wanted_states) ||
            (wanted_disconnects >= 0 && f->disconnects >= wanted_disconnects)) return;
        host_poll(f->host, 5);
    }
    CHECK(clock_now(f) >= end ||
          (wanted_states >= 0 && f->states >= wanted_states) ||
          (wanted_disconnects >= 0 && f->disconnects >= wanted_disconnects));
}

static int pair(fixture *f)
{
    CHECK(host_pair(f->host, 30));
    pump_until(f, clock_now(f) + 15000, 1, -1);
    CHECK(f->connects == 1 && f->states > 0);
    CHECK(host_paired_count(f->host) == 1);
    CHECK(f->has_handle);
    for (unsigned step = 0; step < 400 && (f->barrier_due || f->rand_pending); step++)
        host_poll(f->host, 5);
    CHECK(f->rand_started == f->rand_completed && !f->barrier_due && !f->rand_pending);
    return f->connects == 1 && f->states > 0;
}

/* owned_close: 1 fresh link with drain, 2 fresh link without clock progress,
 * 0 none, -1 an owned link with stale input. */
static void finish(fixture *f, int owned_close)
{
    unsigned before = f->ncommands, disconnects = count(f, DISCONNECT);
    unsigned restores = 0;
    f->closing = 1;
    if (f->host) {
        host_close(f->host);
        f->host = NULL;
    } else {
        f->transport.ops->close(f->transport.ctx);
    }
    if (!single_producer()) {
        CHECK(f->ncommands == before);
        CHECK(count(f, DISCONNECT) == disconnects);
    } else {
        /* Only scan-enable has a known original in these fixtures. Masks and
         * scan parameters have no readback and must never be guessed on close. */
        for (unsigned i = before; i < f->ncommands; i++) {
            const wire_command *c = &f->commands[i];
            CHECK(c->op == DISCONNECT || c->op == SCAN_ENABLE);
            if (c->op == SCAN_ENABLE) {
                CHECK(c->len == 1 && c->params[0] == 0);
                restores++;
            }
            if (c->op == DISCONNECT)
                CHECK(f->has_handle && c->len == 3 && le16(c->params) == f->handle);
        }
        CHECK(restores <= 1);
        CHECK(count(f, DISCONNECT) == disconnects + (owned_close > 0));
        if (owned_close > 0) CHECK(restores == (unsigned)(owned_close == 1));
    }
    sim_lifetime life = f->le ? sim_le_resources(f->le) : sim_resources(f->classic);
    CHECK(life.closes == 1 && life.uninits == 1);
    CHECK(life.event_reads == 0 && life.acl_reads == 0);
    if (f->le) sim_le_free(f->le);
    else sim_free(f->classic);
    ph_clock_hook = NULL;
    static const char *suffixes[] = {"", ".new", ".irk", ".irk.new"};
    for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char path[120];
        snprintf(path, sizeof path, "%s%s", f->db, suffixes[i]);
        CHECK(unlink(path) == 0 || errno == ENOENT);
    }
    CHECK(rmdir(f->dir) == 0);
}

static void positive(int kind)
{
    fixture f;
    test_name = kind == 2 ? "LE pairing/input/close" :
                kind == 1 ? "8BitDo pairing/input/close" : "DS4 pairing/input/close";
    begin(&f, kind);
    if (!start(&f) || !pair(&f)) { finish(&f, 0); return; }
    CHECK(f.info.vid == (kind == 2 ? 0x045e : kind == 1 ? 0x2dc8 : 0x054c));
    CHECK(f.info.pid == (kind == 2 ? 0x0b13 : kind == 1 ? 0x6001 : 0x05c4));
    CHECK(f.state.buttons & PAD_CROSS);
    if (kind == 0) {
        CHECK(f.info.profile && !strcmp(f.info.profile, "DualShock 4"));
        CHECK(f.state.lx == 0x20 && f.state.ly == 0xe0);
        CHECK((f.state.buttons & (PAD_UP | PAD_L2)) == (PAD_UP | PAD_L2));
        CHECK(f.state.l2 == 0x80);
        CHECK(sim_outputs(f.classic) > 0);
    } else if (kind == 1) {
        CHECK(f.info.profile && !strcmp(f.info.profile, "Generic HID"));
        CHECK(f.state.lx == 0xff && f.state.ly == 0x80);
        CHECK(f.state.buttons & PAD_DOWN);
        CHECK(sim_sdp_continuations(f.classic) > 0);
    } else {
        CHECK(f.info.profile && !strcmp(f.info.profile, "Xbox"));
        CHECK(f.state.lx == 0xff && f.state.ly == 0x80);
        CHECK(f.state.buttons & PAD_CREATE);
        CHECK(sim_le_paired(f.le) && sim_le_encrypted(f.le));
        CHECK(sim_le_pairings(f.le) == 1 && sim_le_error(f.le) == NULL);
    }
    int previous_states = f.states;
    if (kind == 2) {
        unsigned char report[16] = {0,0x80,0,0x80,0,0x80,0,0x80};
        sim_le_set_report(f.le, report, sizeof report);
    } else if (kind == 1) {
        unsigned char report[7] = {0x80,0x80,0x80,0x80,8,0,0};
        sim_set_report(f.classic, report, sizeof report);
    } else {
        unsigned char report[78] = {0x11,0xc0};
        report[3] = report[4] = report[5] = report[6] = 0x80;
        report[7] = 8;
        sim_set_report(f.classic, report, sizeof report);
    }
    pump_until(&f, clock_now(&f) + 1000, previous_states + 1, -1);
    CHECK(f.states > previous_states);
    CHECK(!(f.state.buttons & PAD_CROSS) && f.state.lx == 0x80);
    CHECK(host_connected(f.host, f.current.slot));
    f.inspect_retirement = 1;
    finish(&f, 1);
    CHECK(f.disconnects == 1 && same_link(f.disconnected, f.current));
}

static void capability_refused(int le)
{
    fixture f;
    test_name = le ? "LE unsupported capabilities" : "Classic unsupported capabilities";
    begin(&f, le ? 2 : 0);
    if (!prepare(&f)) { finish(&f, 0); return; }
    if (le) sim_le_refuse_command(f.le, SUPPORTED, 1);
    else sim_refuse_command(f.classic, SUPPORTED, 1);
    CHECK(!host_start(f.host));
    CHECK(f.ncommands == 1 && count(&f, SUPPORTED) == 1);
    CHECK(!f.capability && count(&f, RAND) == 0);
    finish(&f, 0);
}

static void pairing_mode_disabled(int le)
{
    fixture f;
    test_name = "irreversible Simple Pairing transition refused";
    begin_with_pairing_mode(&f, le ? 2 : 0, 0);
    if (prepare(&f)) {
        CHECK(!host_start(f.host));
        CHECK(count(&f, SSP_WRITE) == 0);
    }
    finish(&f, 0);
}

/* Missing and 6-second-late responses exhaust the complete setup retry budget.
 * Test capability acquisition, its first Rand barrier, and a running write. */
static void ambiguous(int le, unsigned op, int delay)
{
    fixture f;
    test_name = delay < 0 ? "missing response stops admission" : "late response stops admission";
    begin(&f, le ? 2 : 0);
    printf("FAULT opcode=%04x response-delay=%d\n", op, delay);
    if (op == EVENT_MASK) {
        if (!start(&f)) { finish(&f, 0); return; }
    } else if (!prepare(&f)) {
        finish(&f, 0);
        return;
    }
    unsigned before = count(&f, op);
    f.fault_op = op;
    f.fault_delay = delay;
    f.fault_left = op == EVENT_MASK ? 1 : 3;
    f.fault_armed = 1;
    if (op == EVENT_MASK) {
        unsigned char mask[8] = {0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0x3f};
        unsigned char reply[HCI_PKT_MAX];
        int len = 99;
        CHECK(!host_raw_cmd(f.host, op, mask, sizeof mask, reply, &len));
        CHECK(len == 0);
    } else {
        CHECK(!host_start(f.host));
    }
    CHECK(!f.fault_armed && f.fault_invocation != 0);
    unsigned physical = op == EVENT_MASK ? 1 : 3;
    CHECK(count(&f, op) == before + physical); /* Count real invocations, not tokens. */
    CHECK(f.ncommands && f.commands[f.ncommands - 1].invocation == f.fault_invocation);
    unsigned stopped = f.ncommands;
    pump_until(&f, clock_now(&f) + 6000, -1, -1);
    CHECK(f.ncommands == stopped && count(&f, op) == before + physical);
    wire_command *c = invocation(&f, f.fault_invocation);
    CHECK(c != NULL && c->completions == (unsigned)(delay >= 0));
    unsigned char reply[HCI_PKT_MAX];
    int len = 99;
    CHECK(!host_raw_cmd(f.host, READ_ADDRESS, NULL, 0, reply, &len));
    CHECK(!host_pair(f.host, 30));
    CHECK(len == 0 && f.ncommands == stopped);
    finish(&f, 0);
    CHECK(f.ncommands == stopped);           /* Close cannot turn ambiguity into undo. */
}

static void raw_refused(int le)
{
    fixture f;
    test_name = le ? "LE raw command default deny" : "Classic raw command default deny";
    begin(&f, le ? 2 : 0);
    if (!start(&f)) { finish(&f, 0); return; }
    static const unsigned ops[] = {
        0xfcff, RESET, INQUIRY_CANCEL, CREATE_CANCEL, NAME_CANCEL, LE_CANCEL, RAND
    };
    unsigned before = f.ncommands;
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        unsigned char reply[HCI_PKT_MAX];
        int len = 99;
        CHECK(!host_raw_cmd(f.host, ops[i], NULL, 0, reply, &len));
        CHECK(len == 0 && f.ncommands == before);
    }
    /* Refusing an unknown opcode must not poison a healthy session. */
    unsigned char reply[HCI_PKT_MAX];
    int len = 0;
    CHECK(host_raw_cmd(f.host, READ_ADDRESS, NULL, 0, reply, &len));
    CHECK(len == 7 && reply[0] == 0);
    CHECK(f.ncommands == before + 1 && count(&f, READ_ADDRESS) > 0);
    finish(&f, 0);
}

static void disconnect_reentrant(void)
{
    fixture f;
    test_name = "generation-bearing callbacks and reentrant disconnect";
    begin(&f, 0);
    if (!start(&f) || !pair(&f)) { finish(&f, 0); return; }
    CHECK(host_pair(f.host, 0));
    unsigned before = count(&f, DISCONNECT);
    unsigned char gone[4] = {0,0,0,0x13}, inquiry_done = 0;
    put16(gone + 1, f.handle);
    f.reenter_disconnect = 1;
    sim_disconnect(f.classic);
    sim_schedule_event(f.classic, 5, gone, sizeof gone, 5);
    sim_schedule_event(f.classic, 1, &inquiry_done, 1, 10);
    pump_until(&f, clock_now(&f) + 50, -1, -1);
    CHECK(f.disconnects == 1 && f.reenter_result == 1);
    CHECK(same_link(f.disconnected, f.first));
    CHECK(host_paired_count(f.host) == 0);
    CHECK(count(&f, DISCONNECT) == before);   /* A vanished link cannot be undone again. */

    /* Classic handle reuse is deliberately unsafe inside one attempt.
     * This case tests callback identity, not reconnect. */
    finish(&f, 0);
    CHECK(f.disconnects == 1);
}

static void forget_in_connect(void)
{
    fixture f;
    test_name = "connect callback forget suppresses input and disconnects once";
    begin(&f, 0);
    if (!start(&f)) { finish(&f, 0); return; }
    f.forget_on_connect = f.reenter_disconnect = 1;
    CHECK(host_pair(f.host, 30));
    pump_until(&f, clock_now(&f) + 15000, -1, 1);
    CHECK(f.connects == 1 && f.disconnects == 1 && f.states == 0);
    CHECK(f.forget_result == 1 && f.reenter_result == 0);
    CHECK(same_link(f.disconnected, f.first));
    CHECK(host_paired_count(f.host) == 0);
    CHECK(count(&f, DISCONNECT) == (unsigned)single_producer());
    CHECK(host_pair(f.host, 0));
    finish(&f, 0);
    CHECK(f.disconnects == 1);
}

static void stale_input_close(void)
{
    fixture f;
    test_name = "single-producer stale input vetoes disconnect";
    begin(&f, 0);
    if (!start(&f) || !pair(&f)) { finish(&f, 0); return; }
    unsigned char unused = 0;
    sim_set_report(f.classic, &unused, 0);
    pump_until(&f, clock_now(&f) + 1200, -1, -1);
    finish(&f, -1);
    CHECK(count(&f, DISCONNECT) == 0);
}

static void forget_during_query(fixture *f)
{
    f->fault_op = READ_ADDRESS;
    f->fault_armed = f->forget_on_query = 1;
    f->fault_delay = 300;
    int len = 16;
    unsigned char reply[16];
    CHECK(host_raw_cmd(f->host, READ_ADDRESS, NULL, 0, reply, &len));
}

static void forget_while_command_busy(void)
{
    fixture f;
    test_name = "busy command does not strand or resurrect local retirement";
    begin(&f, 0);
    if (!start(&f) || !pair(&f)) { finish(&f, 0); return; }
    pad_info info;
    pad_state state;
    host_paired_info paired;
    CHECK(host_pad_get(f.host, f.current.slot, &info, &state));
    CHECK(host_paired_get(f.host, 0, &paired) && paired.connected);
    int states = f.states;
    forget_during_query(&f);
    pump_until(&f, clock_now(&f) + 100, -1, -1);
    CHECK(f.forget_result == 1 && f.connects == 1 && f.disconnects == 1);
    CHECK(f.states == states && !host_connected(f.host, f.current.slot));
    CHECK(!host_pad_get(f.host, f.current.slot, &info, &state));
    CHECK(host_paired_count(f.host) == 0 && !host_paired_get(f.host, 0, &paired));
    CHECK(own_link_retained(host_ownership(f.host), f.current));
    pad_output output = {0};
    CHECK(!host_set_output(f.host, f.current.slot, &output));
    CHECK(count(&f, DISCONNECT) == 0);
    finish(&f, 1);
    CHECK(f.disconnects == 1);
}

typedef struct {
    fixture *f;
    const hci_ops *base;
    hci_ops ops;
    uint64_t deadline;
    unsigned pumps, blocked, closes;
    int armed, no_progress, pump_result;
} close_probe;

static close_probe *closing;

static int cutoff_pump(void *ctx, int timeout_ms)
{
    close_probe *p = closing;
    if (!p->armed) return p->base->pump(ctx, timeout_ms);
    p->pumps++;
    own_ledger *ledger = host_ownership(p->f->host);
    own_summary s;
    own_snapshot(ledger, &s);
    CHECK(s.runtime.deadline == p->deadline);
    CHECK(own_link_retained(ledger, p->f->current));
    if (p->no_progress) {
        if (p->pumps > 1) {
            CHECK(p->pumps == 1);
            printf("host transactions: %u checks, %u failures (nonprogressing pump)\n",
                   checks, failures);
            exit(1);
        }
        return p->pump_result;
    }
    if (!own_drain_allowed(ledger, (uint64_t)sim_now(), 0)) {
        /* Model the USB cutoff return without advancing time; fail finitely
         * if host_close repeats it instead of reaching finalization. */
        if (++p->blocked == 2) {
            CHECK(p->blocked == 0);
            printf("host transactions: %u checks, %u failures (cutoff spin)\n", checks, failures);
            exit(1);
        }
        return 0;
    }
    CHECK(timeout_ms > 0 &&
          (uint64_t)timeout_ms <= p->deadline - SESSION_RESERVE_MS - (uint64_t)sim_now());
    return p->base->pump(ctx, timeout_ms);
}

static void cutoff_close(void *ctx)
{
    close_probe *p = closing;
    if (p->armed) {
        CHECK(++p->closes == 1);
        own_ledger *ledger = host_ownership(p->f->host);
        own_summary s;
        own_snapshot(ledger, &s);
        CHECK(!s.transport_closed && s.runtime.deadline == p->deadline);
        CHECK(p->f->disconnects == 1);
        CHECK(own_link_retained(ledger, p->f->current));
        CHECK(own_detach_link(ledger, p->f->current) == OWN_BUSY);
    }
    p->base->close(ctx);
    if (p->armed) {
        own_summary s;
        own_snapshot(host_ownership(p->f->host), &s);
        CHECK(s.transport_closed && s.runtime.deadline == p->deadline);
        CHECK(own_link_retained(host_ownership(p->f->host), p->f->current));
    }
}

static void close_at_cutoff(int crossing)
{
    fixture f;
    test_name = crossing ? "close caps wait at existing work cutoff" :
                           "close skips nonadvancing pump after existing cutoff";
    begin(&f, 0);
    close_probe p = {.f = &f, .base = f.transport.ops, .ops = *f.transport.ops};
    p.ops.pump = cutoff_pump;
    p.ops.close = cutoff_close;
    closing = &p;
    f.transport.ops = &p.ops;
    if (!start(&f) || !pair(&f)) {
        finish(&f, 0);
        closing = NULL;
        return;
    }
    forget_during_query(&f);
    CHECK(f.forget_result == 1 && f.disconnects == 1);
    own_ledger *ledger = host_ownership(f.host);
    CHECK(own_link_retained(ledger, f.current));
    CHECK(own_release(ledger, SESSION_STOP, (uint64_t)sim_now()));
    own_summary s;
    own_snapshot(ledger, &s);
    p.deadline = s.runtime.deadline;
    long cutoff = (long)(p.deadline - SESSION_RESERVE_MS);
    p.base->pump(f.transport.ctx, (int)(cutoff - sim_now() + (crossing ? -7 : 1)));
    CHECK(own_drain_allowed(ledger, (uint64_t)sim_now(), 0) == crossing);
    p.armed = 1;
    finish(&f, 0);
    CHECK(p.closes == 1 && p.blocked == 0);
    CHECK(p.pumps == (unsigned)crossing);
    CHECK(sim_now() == cutoff + (crossing ? 0 : 1));
    CHECK(f.disconnects == 1 && same_link(f.disconnected, f.first));
    closing = NULL;
}

static void close_without_progress(int pump_result, int known_dead)
{
    fixture f;
    test_name = known_dead ? "close skips a known-dead transport before its deadline" :
                pump_result < 0 ? "close stops when the pump dies without advancing time" :
                                 "close stops when an empty pump cannot advance time";
    begin(&f, 0);
    close_probe p = {.f = &f, .base = f.transport.ops, .ops = *f.transport.ops,
                     .no_progress = 1, .pump_result = pump_result};
    p.ops.pump = cutoff_pump;
    p.ops.close = cutoff_close;
    closing = &p;
    f.transport.ops = &p.ops;
    if (!start(&f) || !pair(&f)) {
        finish(&f, 0);
        closing = NULL;
        return;
    }
    forget_during_query(&f);
    CHECK(f.forget_result == 1 && f.disconnects == 1);
    own_ledger *ledger = host_ownership(f.host);
    CHECK(own_link_retained(ledger, f.current));
    long before = sim_now();
    CHECK(own_release(ledger, SESSION_STOP, (uint64_t)before));
    own_summary s;
    own_snapshot(ledger, &s);
    p.deadline = s.runtime.deadline;
    CHECK(s.runtime.state == SESSION_QUIESCING && s.runtime.reason == SESSION_STOP &&
          s.runtime.cleanup_pending);
    CHECK(p.deadline - SESSION_RESERVE_MS > (uint64_t)before + 2000);
    p.armed = 1;
    if (known_dead) {
        host_poll(f.host, 0);
        CHECK(host_transport_lost(f.host));
        p.pumps = 0;
    }
    finish(&f, pump_result < 0 ? 0 : 2);
    CHECK(p.closes == 1 && p.blocked == 0);
    CHECK(p.pumps == (unsigned)!known_dead);
    CHECK(sim_now() == before);
    CHECK(f.disconnects == 1 && same_link(f.disconnected, f.first));
    closing = NULL;
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (!strcmp(argv[1], "--close-cutoff-entry")) close_at_cutoff(0);
        else if (!strcmp(argv[1], "--close-cutoff-crossing")) close_at_cutoff(1);
        else if (!strcmp(argv[1], "--busy-forget")) forget_while_command_busy();
        else if (!strcmp(argv[1], "--retiring-paired")) positive(0);
        else if (!strcmp(argv[1], "--close-known-dead")) close_without_progress(-1, 1);
        else if (!strcmp(argv[1], "--close-dead-pump")) close_without_progress(-1, 0);
        else if (!strcmp(argv[1], "--close-stalled-pump")) close_without_progress(0, 0);
        else return 2;
        goto report;
    }
    if (argc != 1) return 2;
    for (int kind = 0; kind < 3; kind++) positive(kind);
    for (int le = 0; le < 2; le++) {
        pairing_mode_disabled(le);
        capability_refused(le);
        raw_refused(le);
        static const unsigned targets[] = {SUPPORTED, RAND, EVENT_MASK};
        for (unsigned i = 0; i < sizeof targets / sizeof targets[0]; i++) {
            ambiguous(le, targets[i], -1);
            ambiguous(le, targets[i], 6000);
        }
    }
    disconnect_reentrant();
    forget_in_connect();
    forget_while_command_busy();
    close_at_cutoff(0);
    close_at_cutoff(1);
    close_without_progress(-1, 1);
    close_without_progress(-1, 0);
    close_without_progress(0, 0);
    if (single_producer()) stale_input_close();
report:
    printf("host transactions: %u checks, %u failures (%s)\n", checks, failures,
           single_producer() ? "SINGLE_PRODUCER" : "NO_RESTORE");
    return failures ? 1 : 0;
}
