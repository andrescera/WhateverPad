/* Raw bytes traverse the production USB, host/LE, vpad and HTTP implementations.
 * Only the controller and OS/vendor boundaries know causal provenance. */
#include "sim.h"
#include "sim_le.h"
#include "usb_standin/dev/usb/usb_ioctl.h"
#include "../src/hci_usb.h"
#include "../src/ps5_vpad.h"
#include "../src/web.h"
#include "../src/util.h"
#include "../src/log.h"
#include "../src/attrib.h"
#include "../src/bond_store.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { MAX_STEPS = 128, MAX_COMMANDS = 1024, CMD = 62, XFERS = 63, FD = 973 };
enum property {
    NO_CANCEL, NORMAL_ADMISSION, COMMAND_SERIALIZATION, NO_RESTORE_UNDO, CAUSAL_UNDO, FRESH_HANDLE,
    TERMINAL_REQUIRED, CALLBACK_ONCE, VPAD_ADMISSION, CLEANUP_BOUND,
    NATIVE_RELEASE, RETRY_DELIVERY, LEFT_CONNECTED,
    USB_INVENTORY, VENDOR_IDENTITY, HTTP_FRAME, POSITIVE_INPUT, PROPERTY_COUNT
};
static const struct { const char *name; int todo; } properties[] = {
    {"no-cancel-or-reset", 0}, {"no-normal-effect-after-release", 13},
    {"one-unanswered-command-no-resubmission", 13},
    {"no-restore-policy-no-undo", 13}, {"undo-needs-own-ack-barrier-and-version", 14},
    {"disconnect-needs-fresh-owned-input", 14}, {"status-is-not-link-completion", 14},
    {"callback-per-generation-once", 14}, {"no-vpad-work-after-release", 15},
    {"one-ten-second-cleanup-budget", 15}, {"native-request-releases-transport", 15},
    {"undelivered-retry-is-not-admitted", 0}, {"uncertain-link-reported-left-connected", 15},
    {"exact-usb-acquisitions-and-finalization", 0}, {"delete-only-acquired-identity-once", 0},
    {"complete-http-framing", 0}, {"pairing-reaches-usable-virtual-input", 0}
};
enum action {
    TICK, PAIR, OUTPUT, STATE, RETRY, BLOCKED_RETRY, STOP, NATIVE, LOSS,
    DELAY_ACK, LOSE_ACK, DUPLICATE_ACK, FOREIGN_ACK, REFUSE, UNCERTAIN,
    FOREIGN_CREATE, VANISH, REUSE, ID_ZERO, ID_TWO, ADD_REFUSED, DELETE_REFUSED,
    QUEUE_FOUR, REENTER, CHURN, CUTOFF, DEADLINE, ACTION_COUNT
};
static const char *const actions[] = {
    "tick", "pair", "output", "State", "Retry", "blocked-Retry", "Stop",
    "native-request", "transport-loss", "delay-ACK", "lose-ACK", "duplicate-old-ACK",
    "foreign-ACK", "refuse-before-submit", "uncertain-submit", "foreign-Create",
    "withheld-disconnect", "reuse-handle", "identity-zero", "identity-two",
    "Add-refused", "Delete-refused", "four-slots", "callback-reentry", "empty-error-completions",
    "cutoff-9750", "deadline-10000"
};
typedef struct { unsigned char action; unsigned value; } step;
typedef struct {
    const char *policy, *property;
    int todo;
    unsigned failures;
} expectation;

static unsigned validate_expectations(const expectation *entries, unsigned count)
{
    unsigned unexpected = 0;
    for (unsigned i = 0; i < count; i++) {
        if (entries[i].failures) continue;
        unexpected++;
        printf("XPASS policy=%s property=%s todo=%d\n",
            entries[i].policy, entries[i].property, entries[i].todo);
    }
    printf("STRICT expected=%u xfail=%u xpass=%u\n", count, count - unexpected, unexpected);
    return unexpected;
}

static int integration_result(unsigned hard, unsigned unexpected, int expected_failures)
{
    if (unexpected) return 2;
    return hard || expected_failures ? 1 : 0;
}

static int strict_fixture(int missing)
{
    expectation entries[] = {
        {"fixture", "fixture-still-failing", 0, 1},
        {"fixture", "fixture-disappears", 0, 1}
    };
    if (missing) entries[1].failures = 0;
    unsigned unexpected = validate_expectations(entries, 2);
    return integration_result(0, unexpected, 1);
}

typedef struct {
    unsigned op, n;
    unsigned char p[255];
    sim_origin origin;
    int accepted, ack, answered, terminal, barrier, undo;
    unsigned resource_version;
    long at;
} invocation;
typedef struct { int32_t id; int acquired, identified, deletes; } device;
typedef struct {
    sim_origin origin;
    unsigned char bytes[12];
    int length;
} answer;
typedef struct {
    unsigned seed, scenario;
    int le, positive, released, lost, foreign_create, fault, identity, add_fail, delete_fail;
    int reenter, reentry_case, serving, refuse_send, port, logpipe[2], foreign_stimulus, reassignment_stimulus;
    int reassignment_pending;
    unsigned reassignment_create, reassignment_version, duplicate_copies, last_real_answer;
    int opens, uninits, closes, commands, acl, inputs, connects, disconnects, generation;
    int start_count[XFERS], complete_count[XFERS], stop_count[XFERS];
    int open[XFERS], busy[XFERS], cancelled[XFERS];
    int initialized, fd_open, out_done, command_done, command_status, churn;
    unsigned inject_op, latest, barrier_parent;
    answer scan_answer, rand_answer;
    unsigned copy_op;
    int copy_stage, copy_foreign;
    unsigned setting_owner[6], setting_pending[6];
    unsigned char original[6][8];
    int original_known[6], current_length[6], ambiguous;
    unsigned char barrier_values[256][8];
    unsigned barrier_count;
    int acknowledgements[MAX_COMMANDS], foreign_handle, foreign_inputs;
    unsigned link_invocation, slot_invocation[4];
    unsigned handle_owner, handle_version;
    unsigned handle_value, reassignment_handle;
    int handle_live;
    sim_origin delivery_origin;
    unsigned delivery_code;
    unsigned char delivery_data[255];
    int delivery_length, delivery_pending;
    int connected_invocation[MAX_COMMANDS], retired_invocation[MAX_COMMANDS];
    unsigned policy_bad[2];
    unsigned assumptions; /* bit 0: L1 violated, bit 1: B1, bit 2: another producer */
    long start, release_at, last_input;
    volatile int stop, retry, bt;
    hci_t controller, transport;
    sim_t *classic;
    sim_le_t *low_energy;
    host_t *host;
    web_t *web;
    struct usb_fs_endpoint *ep;
    invocation history[MAX_COMMANDS];
    device devices[32];
    int ndevices;
    uint64_t authid;
    unsigned char caps[16];
} fixture;
static fixture f;
static const char *mutation;
static int unsafe_attribution_control;
static char directory[128], db[160];
static unsigned checks, runs, positives[3], deliveries, generated_events, submitted, empty_completions;
#define REQUIRE(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL integration seed=%u line=%d: %s\n", f.seed, __LINE__, #c); exit(1); \
} } while (0)

static int mutated(const char *name) { return mutation && strcmp(mutation, name) == 0; }
static void violation(enum property p)
{
    f.policy_bad[0] |= 1u << p;
    f.policy_bad[1] |= 1u << p;
}
static int setting(unsigned op)
{
    return op == 0x0c01 || op == 0x0c1a || op == 0x0c1c ||
           op == 0x2001 || op == 0x200b || op == 0x200c;
}
static int setting_index(unsigned op)
{
    static const unsigned ops[] = {0x0c01, 0x0c1a, 0x0c1c, 0x2001, 0x200b, 0x200c};
    for (int i = 0; i < 6; i++) if (ops[i] == op) return i;
    return -1;
}
static int create(unsigned op) { return op == 0x0405 || op == 0x0409 || op == 0x200d; }
static int status_ack(unsigned op)
{
    return create(op) || op == 0x0401 || op == 0x0406 || op == 0x040a ||
           op == 0x0411 || op == 0x0413 || op == 0x2013 || op == 0x2019 ||
           op == 0x2025 || op == 0x2026;
}
static void causal_event(sim_origin, unsigned, const unsigned char *, int, int);
static void delay_events(int, int, int);
static void release_now(void)
{
    if (!f.released) {
        if (f.scenario == 7 && f.foreign_handle && f.foreign_inputs) {
            for (unsigned i = 1; i < MAX_COMMANDS; i++)
                if (create(f.history[i].op) && !f.history[i].origin.foreign && !f.history[i].answered)
                    f.foreign_stimulus = 1;
        }
        f.released = 1; f.release_at = now_ms();
    }
}

/* This model reads controller causality, never a host/ownership/session field.
 * A serial identifies the controller invocation which made an event, not the
 * production waiter's current opcode. Duplicates retain their original serial. */
static void observe(void *ud, enum sim_observation kind, sim_origin origin,
                    unsigned code, const unsigned char *p, int n)
{
    (void)ud;
    REQUIRE(origin.invocation < MAX_COMMANDS);
    if (origin.foreign) f.assumptions |= 4;
    invocation *r = &f.history[origin.invocation];
    if (kind == SIM_COMMAND) {
        REQUIRE(origin.invocation && n >= 0 && n <= 255);
        REQUIRE(!r->op);
        r->op = code; r->n = (unsigned)n; r->origin = origin;
        r->at = now_ms();
        if (n) memcpy(r->p, p, (size_t)n);
        f.latest = origin.invocation;
        if (f.reassignment_pending && origin.foreign && create(code))
            f.reassignment_create = origin.invocation;
        if (f.barrier_parent && code != 0x2018) f.barrier_parent = 0;
        int index = setting_index(code);
        if (index >= 0) f.setting_pending[index] = origin.invocation;
        if (!origin.foreign) for (unsigned i = 1; i < origin.invocation; i++)
            if (f.history[i].op && !f.history[i].origin.foreign && !f.history[i].answered)
                violation(COMMAND_SERIALIZATION);
        if ((f.copy_stage == 1 && code == f.copy_op) ||
            (f.copy_stage == 2 && code == 0x2018)) {
            answer *a = f.copy_stage == 1 ? &f.scan_answer : &f.rand_answer;
            sim_origin copy = a->origin;
            if (f.copy_foreign) { copy.foreign = 1; copy.invocation = 0; }
            else copy.copy = 1;
            causal_event(copy, 0x0e, a->bytes, a->length, 0);
            /* Real answers retain submission order (B1); the two old copies
             * precede both the replacement's refusal and its fresh Rand. */
            delay_events(0x0e, 1, f.copy_stage == 1 ? 100 : 200);
            f.copy_stage = f.copy_stage == 1 && !f.copy_foreign ? 2 : 0;
        }
        return;
    }
    if (kind == SIM_GENERATED) generated_events++;
    else deliveries++;
    if ((code == 0x0e && n >= 4) || (code == 0x0f && n >= 4)) {
        unsigned op = le16(p + (code == 0x0e ? 1 : 2));
        int status = p[code == 0x0e ? 3 : 0];
        if (!op) return; /* credit-only NOP */
        if (kind == SIM_DELIVERED && origin.invocation) {
            if (origin.copy) {
                if (op == 0x0c1a || op == 0x200c) f.duplicate_copies |= 1;
                if (op == 0x2018) f.duplicate_copies |= 2;
            } else {
                if (origin.invocation < f.last_real_answer) f.assumptions |= 2;
                if (origin.invocation > f.last_real_answer) f.last_real_answer = origin.invocation;
            }
        }
        if (origin.invocation) REQUIRE(r->op == op);
        if (kind == SIM_DELIVERED && origin.invocation && !origin.copy &&
            !origin.foreign && !status && code == 0x0e) {
            if (op == (f.le ? 0x200cu : 0x0c1au)) {
                REQUIRE(n == 4);
                f.scan_answer = (answer){.origin = origin, .length = n};
                memcpy(f.scan_answer.bytes, p, (size_t)n);
                memset(&f.rand_answer, 0, sizeof f.rand_answer);
            } else if (op == 0x2018 && f.barrier_parent == f.scan_answer.origin.invocation) {
                REQUIRE(n == 12);
                f.rand_answer = (answer){.origin = origin, .length = n};
                memcpy(f.rand_answer.bytes, p, (size_t)n);
            }
        }
        if (kind == SIM_GENERATED) {
            if (!status && origin.invocation && !r->accepted) {
                r->accepted = 1;
                int index = setting_index(op);
                if (index >= 0 && r->n <= 8) {
                    f.current_length[index] = (int)r->n;
                    f.setting_owner[index] = origin.invocation;
                }
            }
        } else if (origin.invocation) {
            f.acknowledgements[origin.invocation]++;
            r->answered = 1;
            int index = setting_index(op);
            if (index >= 0 && f.setting_pending[index] == origin.invocation)
                f.setting_pending[index] = 0;
            if (!status && !origin.foreign) r->ack = 1;
            if (f.acknowledgements[origin.invocation] > 1 || origin.foreign ||
                (code == 0x0f) != status_ack(op) || now_ms() - r->at >= 2000) {
                f.ambiguous = 1;
                f.barrier_parent = 0;
            } else if (!status && (setting(op) || create(op))) f.barrier_parent = origin.invocation;
            if (op == 0x2018 && n == 12 && !status && f.barrier_parent) {
                int fresh = f.barrier_count < 256;
                for (unsigned i = 0; i < f.barrier_count; i++)
                    if (!memcmp(f.barrier_values[i], p + 4, 8)) fresh = 0;
                if (fresh && !f.ambiguous) {
                    memcpy(f.barrier_values[f.barrier_count++], p + 4, 8);
                    f.history[f.barrier_parent].barrier = 1;
                } else f.ambiguous = 1;
                f.barrier_parent = 0;
            }
            if (!status && code == 0x0e && (op == 0x0c19 || op == 0x0c1b)) {
                int index = op == 0x0c19 ? 1 : 2, length = op == 0x0c19 ? 1 : 4;
                if (!f.original_known[index] && n == 4 + length) {
                    memcpy(f.original[index], p + 4, (size_t)length);
                    f.original_known[index] = 1;
                }
            }
        } else {
            f.ambiguous = 1;
            f.barrier_parent = 0;
        }
    }
    if ((code == 3 && n >= 11 && !p[0]) ||
        (code == 0x3e && n >= 19 && p[0] == 1 && !p[1])) {
        if (kind == SIM_GENERATED && !r->resource_version) {
            f.handle_owner = origin.invocation;
            f.handle_value = le16(p + (code == 3 ? 1 : 2));
            r->resource_version = ++f.handle_version; f.handle_live = 1;
        }
        if (kind == SIM_DELIVERED) {
            const unsigned char *expected = r->p + (r->op == 0x200d ? 6 : 0);
            if (origin.invocation && create(r->op) && r->n >= 6 &&
                !memcmp(expected, p + (code == 3 ? 3 : 6), 6)) r->terminal = 1;
            f.generation++;
            f.link_invocation = origin.invocation;
            if (origin.foreign) f.foreign_handle = 1;
            if (f.reassignment_pending && origin.invocation == f.reassignment_create &&
                r->terminal && f.handle_owner == origin.invocation &&
                r->resource_version > f.reassignment_version &&
                le16(p + (code == 3 ? 1 : 2)) == f.reassignment_handle && !f.last_input)
                f.reassignment_stimulus = 1;
        }
    }
    if (kind == SIM_GENERATED && code == 5 && n >= 4 && !p[0]) f.handle_live = 0;
}

/* Retain the envelope until the last USB fragment completes. The host cannot
 * acquire a link from the first sixteen bytes of a longer LE terminal. */
static void controller_observation(void *ud, enum sim_observation kind, sim_origin origin,
                                   unsigned code, const unsigned char *p, int n)
{
    if (kind != SIM_DELIVERED) { observe(ud, kind, origin, code, p, n); return; }
    REQUIRE(!f.delivery_pending && n >= 0 && n <= 255);
    f.delivery_origin = origin; f.delivery_code = code; f.delivery_length = n;
    memcpy(f.delivery_data, p, (size_t)n); f.delivery_pending = 1;
}

static void causal_event(sim_origin origin, unsigned code, const unsigned char *p, int n, int delay)
{
    if (f.le) sim_le_causal_event(f.low_energy, origin, (unsigned char)code, p, n, delay);
    else sim_causal_event(f.classic, origin, (unsigned char)code, p, n, delay);
}
static void delay_events(int code, int count, int ms)
{
    if (f.le) sim_le_delay_events(f.low_energy, code, -1, count, ms);
    else sim_delay_events(f.classic, code, count, ms);
}
static void refuse_command(unsigned op)
{
    if (f.le) sim_le_refuse_command(f.low_energy, op, 0x0c);
    else sim_refuse_command(f.classic, op, 0x0c);
}

/* Real hci_usb.c ends here. Reads are split to the requested transfer size;
 * events are not smuggled directly into the host's parser or ledger. */
int test_usb_open(const char *path, int flags, ...)
{
    (void)flags;
    REQUIRE(strcmp(path, "/dev/ugen0.2") == 0 && !f.fd_open);
    f.fd_open = 1; f.opens++;
    return FD;
}
int test_usb_close(int fd)
{
    REQUIRE(fd == FD && f.fd_open && !f.initialized);
    f.fd_open = 0; f.closes++;
    return 0;
}
int test_usb_usleep(useconds_t us)
{
    f.controller.ops->pump(f.controller.ctx, (int)(us / 1000) ? (int)(us / 1000) : 1);
    REQUIRE(now_ms() - f.start < 200000);
    return 0;
}
static unsigned char event_bytes[HCI_PKT_MAX];
static int event_len, event_offset;
static void wire_command(const unsigned char *packet)
{
    unsigned op = le16(packet);
    int n = packet[2];
    const unsigned char *p = packet + 3;
    submitted++; f.commands++;
    if (op == 0x0402 || op == 0x0408 || op == 0x200e || op == 0x0c03) violation(NO_CANCEL);
    if (f.released) {
        if (now_ms() >= f.release_at + 9750) violation(CLEANUP_BOUND);
        if (!setting(op) && op != 0x0406) violation(NORMAL_ADMISSION);
        if (setting(op) || op == 0x0406) {
            f.policy_bad[0] |= 1u << NO_RESTORE_UNDO;
            int proven = 0;
            for (unsigned i = 1; i < MAX_COMMANDS; i++) {
                invocation *r = &f.history[i];
                if (f.ambiguous || r->origin.foreign || !r->accepted || !r->ack || !r->barrier || r->undo) continue;
                /* No fixture grants mask preimages, so equality/readback is
                 * insufficient. Link handles need their own terminal too. */
                if (op == 0x0406 && n == 3 && le16(p) == (f.le ? 0x50u : 0x41u) && p[2] == 0x13 &&
                    create(r->op) && r->terminal && f.handle_live && f.handle_owner == i &&
                    r->resource_version == f.handle_version && !f.foreign_handle) {
                    proven = 1; r->undo++;
                }
                int index = setting_index(op);
                if (index >= 0 && r->op == op && f.original_known[index] &&
                    !f.setting_pending[index] &&
                    f.setting_owner[index] == i && n == f.current_length[index] &&
                    memcmp(r->p, f.original[index], (size_t)n) != 0 &&
                    !memcmp(p, f.original[index], (size_t)n)) {
                    proven = 1; r->undo++;
                }
            }
            if (!proven) violation(CAUSAL_UNDO);
            if (op == 0x0406 && (f.foreign_handle || !f.last_input || now_ms() - f.last_input >= 1000))
                violation(FRESH_HANDLE);
        }
    }
    if (f.foreign_create && create(op) && !f.le) {
        f.foreign_create = 0;
        REQUIRE(sim_foreign_command(f.classic, op, p, n));
        delay_events(0x0f, 1, 15000);
        refuse_command(op);
    }
    f.controller.ops->cmd(f.controller.ctx, op, p, n);
}
int test_usb_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    REQUIRE(fd == FD && f.fd_open);
    unsigned i = (request == USB_FS_START || request == USB_FS_STOP || request == USB_FS_OPEN)
        ? *(unsigned char *)arg : 0;
    REQUIRE(i < XFERS);
    switch (request) {
    case USB_GET_DEVICE_DESC: {
        struct usb_device_descriptor *d = arg;
        memset(d, 0, sizeof *d);
        USETW(d->idVendor, 0x0e8d); USETW(d->idProduct, 0x3603);
        return 0;
    }
    case USB_GET_FULL_DESC: {
        static const unsigned char cfg[] = {
            9,2,39,0,1,1,0,0x80,0, 9,4,0,0,3,0xe0,1,1,0,
            7,5,0x81,3,16,0,1, 7,5,0x82,2,0,2,0, 7,5,2,2,0,2,0
        };
        struct usb_gen_descriptor *d = arg;
        REQUIRE(d->ugd_maxlen >= sizeof cfg);
        memcpy(d->ugd_data, cfg, sizeof cfg); d->ugd_actlen = sizeof cfg;
        return 0;
    }
    case USB_GET_DEVICEINFO: case USB_GET_IFACE_DRIVER: errno = ENOTTY; return -1;
    case USB_FS_INIT:
        REQUIRE(!f.initialized);
        REQUIRE(((struct usb_fs_init *)arg)->ep_index_max == XFERS);
        f.ep = ((struct usb_fs_init *)arg)->pEndpoints; f.initialized = 1;
        return 0;
    case USB_FS_OPEN: REQUIRE(!f.open[i]); f.open[i] = 1; return 0;
    case USB_FS_START:
        REQUIRE(f.initialized && f.open[i] && !f.busy[i] && !f.cancelled[i]);
        if (i == CMD) {
            const struct usb_device_request *r = f.ep[i].ppBuffer[0];
            const unsigned char *p = f.ep[i].ppBuffer[1];
            REQUIRE(f.ep[i].nFrames == 2 && f.ep[i].pLength[0] == sizeof *r);
            REQUIRE(r->bmRequestType == UT_WRITE_CLASS_DEVICE && !r->bRequest &&
                    !UGETW(r->wValue) && !UGETW(r->wIndex));
            REQUIRE(UGETW(r->wLength) == f.ep[i].pLength[1] &&
                    f.ep[i].pLength[1] == (unsigned)(3 + p[2]));
            if (mutated("refuse-everything") || f.lost ||
                (f.fault == REFUSE && le16(p) == f.inject_op)) {
                f.fault = 0; errno = EIO; return -1;
            }
            wire_command(p);
            f.command_status = f.fault == UNCERTAIN && le16(p) == f.inject_op;
            if (f.command_status) { f.fault = 0; f.ambiguous = 1; }
            f.command_done = 1;
        }
        f.start_count[i]++;
        f.busy[i] = 1;
        if (i >= 60 && i < CMD) {
            f.acl++;
            if (f.released) violation(NORMAL_ADMISSION);
            REQUIRE(f.controller.ops->acl_send(f.controller.ctx, f.ep[i].ppBuffer[0], (int)f.ep[i].pLength[0]));
            f.out_done = (int)i + 1;
        }
        return 0;
    case USB_FS_STOP:
        REQUIRE(f.initialized && f.open[i]);
        REQUIRE(f.busy[i] && !f.cancelled[i]);
        f.stop_count[i]++; f.busy[i] = 0; f.cancelled[i] = 1;
        return 0;
    case USB_FS_COMPLETE: {
        int n = 0, found = -1, complete_event = 0, cancelled_completion = 0;
        unsigned char bytes[HCI_PKT_MAX];
        for (i = 0; i < XFERS; i++) if (f.cancelled[i]) {
            found = (int)i; f.cancelled[i] = 0; cancelled_completion = 1; break;
        }
        if (found < 0 && f.out_done) { found = f.out_done - 1; f.out_done = 0; }
        if (found < 0 && f.command_done) found = CMD;
        int failed_completion = 0;
        if (found < 0 && f.churn && f.busy[0]) {
            found = 0; f.churn--; empty_completions++; failed_completion = 1;
            f.controller.ops->pump(f.controller.ctx, 100);
        }
        if (found < 0) for (i = 0; i < 60; i++) {
            if (!f.busy[i]) continue;
            if (i < 40) {
                if (event_offset == event_len) {
                    event_len = f.controller.ops->next_event(f.controller.ctx, event_bytes, sizeof event_bytes);
                    event_offset = 0;
                }
                n = event_len - event_offset;
                if (n > (int)f.ep[i].pLength[0]) n = (int)f.ep[i].pLength[0];
                if (n) {
                    memcpy(bytes, event_bytes + event_offset, (size_t)n); event_offset += n;
                    complete_event = event_offset == event_len;
                }
            } else n = f.controller.ops->next_acl(f.controller.ctx, bytes, sizeof bytes);
            if (n) { found = (int)i; break; }
        }
        if (found < 0) { errno = EAGAIN; return -1; }
        struct usb_fs_endpoint *ep = &f.ep[found];
        ((struct usb_fs_complete *)arg)->ep_index = (unsigned char)found;
        ep->pLength[0] = (uint32_t)n; ep->aFrames = n > 0; ep->status = (unsigned)failed_completion;
        if (found == CMD) {
            f.command_done = 0;
            ep->status = (unsigned)f.command_status;
        }
        if (n) memcpy(ep->ppBuffer[0], bytes, (size_t)n);
        if (!cancelled_completion) {
            REQUIRE(f.busy[found]);
            f.complete_count[found]++;
        }
        f.busy[found] = 0;
        if (complete_event) {
            REQUIRE(f.delivery_pending);
            f.delivery_pending = 0;
            observe(NULL, SIM_DELIVERED, f.delivery_origin, f.delivery_code, f.delivery_data, f.delivery_length);
        }
        return 0;
    }
    case USB_FS_UNINIT:
        if (mutated("leak-usb")) return 0;
        REQUIRE(f.initialized);
        for (i = 0; i < XFERS; i++)
            if (f.busy[i] || f.start_count[i] != f.complete_count[i] + f.stop_count[i])
                violation(USB_INVENTORY);
        f.initialized = 0; f.uninits++;
        return 0;
    default: REQUIRE(0); return -1;
    }
}

/* Vendor identity is an independent inventory of successful Add calls.
 * Log candidates do not create an acquired device in this inventory. */
int test_vpad_socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    return dup(f.logpipe[0]);
}
int test_vpad_connect(int fd, const struct sockaddr *p, socklen_t n) { (void)fd; (void)p; (void)n; return 0; }
int32_t sceUserServiceInitialize(void *p) { (void)p; return 0; }
int32_t sceUserServiceGetInitialUser(int32_t *p) { *p = 1; return 0; }
int32_t sceUserServiceGetForegroundUser(int32_t *p) { *p = 1; return 0; }
int32_t sceUserServiceGetLoginUserIdList(int32_t *p) { p[0] = 1; p[1] = p[2] = p[3] = -1; return 0; }
int32_t scePadInit(void) { return 0; }
int32_t scePadSetProcessPrivilege(int32_t p) { (void)p; return 0; }
int32_t scePadGetHandle(int32_t u, int32_t t, int32_t i) { (void)u; (void)t; (void)i; return -1; }
int32_t scePadOpen(int32_t u, int32_t t, int32_t i, const void *p) { (void)u; (void)t; (void)i; (void)p; return -1; }
int32_t scePadClose(int32_t h) { (void)h; return 0; }
int32_t scePadReadState(int32_t h, void *p) { (void)h; (void)p; return -1; }
uint64_t kernel_get_ucred_authid(pid_t p) { (void)p; return f.authid; }
int32_t kernel_set_ucred_authid(pid_t p, uint64_t a) { (void)p; f.authid = a; return 0; }
int32_t kernel_get_ucred_caps(pid_t p, uint8_t *c) { (void)p; memcpy(c, f.caps, 16); return 0; }
int32_t kernel_set_ucred_caps(pid_t p, const uint8_t *c) { (void)p; memcpy(f.caps, c, 16); return 0; }
int sceKernelSendNotificationRequest(int a, void *p, size_t n, int b) { (void)a; (void)p; (void)n; (void)b; return 0; }
static device *device_by_id(int32_t id)
{
    for (int i = 0; i < f.ndevices; i++) if (f.devices[i].id == id) return &f.devices[i];
    return NULL;
}
int32_t scePadVirtualDeviceAddDevice(void *p, int32_t type)
{
    (void)p; (void)type;
    if (f.released) violation(VPAD_ADMISSION);
    if (f.add_fail) return -1;
    REQUIRE(f.ndevices < 32);
    device *d = &f.devices[f.ndevices++];
    d->id = 0x10000 + (int32_t)f.seed * 32 + f.ndevices; d->acquired = 1;
    if (f.identity) {
        char line[256];
        int n = snprintf(line, sizeof line,
            "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x%x][type:1][subType:22][capabilityBattery:0]\n", (unsigned)d->id);
        REQUIRE(write(f.logpipe[1], line, (size_t)n) == n);
        if (f.identity == 2) {
            n = snprintf(line, sizeof line,
                "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x%x][type:1][subType:22][capabilityBattery:0]\n", (unsigned)d->id + 0x1000000);
            REQUIRE(write(f.logpipe[1], line, (size_t)n) == n);
        }
        if (f.identity == 1) d->identified = 1;
    }
    return 0;
}
static int32_t bind_device(uint64_t id, int32_t user)
{
    (void)user;
    device *d = device_by_id((int32_t)id);
    REQUIRE(d && d->acquired && d->identified && !d->deletes);
    if (f.released) violation(VPAD_ADMISSION);
    return 0;
}
int32_t scePadVirtualDeviceInsertData(int32_t id, const void *p)
{
    device *d = device_by_id(id);
    REQUIRE(d && d->acquired && d->identified && !d->deletes);
    if (f.positive) {
        uint32_t buttons;
        const unsigned char *bytes = p;
        memcpy(&buttons, p, sizeof buttons);
        REQUIRE(buttons & PAD_CROSS);
        REQUIRE(bytes[4] == (f.scenario == 0 ? 0x20 : 0xff));
        REQUIRE(bytes[5] == (f.scenario == 0 ? 0xe0 : 0x80));
    }
    if (f.released) violation(VPAD_ADMISSION);
    if (!mutated("drop-input")) f.inputs++;
    return 0;
}
int32_t scePadVirtualDeviceDeleteDevice(int32_t id)
{
    if (mutated("drop-delete")) return 0;
    if (mutated("foreign-delete")) id ^= 0x40000000;
    device *d = device_by_id(id);
    REQUIRE(d && d->acquired && d->identified);
    d->deletes += mutated("duplicate-delete") ? 2 : 1;
    if (f.reentry_case && !mutated("duplicate-delete")) {
        if (d->deletes != 1) violation(CALLBACK_ONCE);
    } else REQUIRE(d->deletes == 1);
    if (f.reenter) { f.reenter = 0; vpad_remove(0); }
    return f.delete_fail ? -1 : 0;
}
void *test_vpad_dlopen(const char *p, int flags) { (void)p; (void)flags; return (void *)1; }
void *test_vpad_dlsym(void *p, const char *name) { (void)p; (void)name; return (void *)bind_device; }

int test_vpad_web_bind(int fd, const struct sockaddr *p, socklen_t n)
{
    int rc = bind(fd, p, n);
    if (!rc) {
        struct sockaddr_in a;
        socklen_t len = sizeof a;
        REQUIRE(getsockname(fd, (struct sockaddr *)&a, &len) == 0);
        f.port = ntohs(a.sin_port);
    }
    return rc;
}
/* Only production server sends are faulted, never the request or sanitizer. */
ssize_t send(int fd, const void *p, size_t n, int flags)
{
    if (f.serving && f.refuse_send) { errno = EPIPE; return -1; }
    if (f.serving && mutated("truncate-http")) return (ssize_t)n;
    return sendto(fd, p, n, flags, NULL, 0);
}
static unsigned request(const char *method, const char *path, int blocked)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(f.port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    char raw[256], reply[16384];
    int n = snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nX-WhateverPad: 1\r\n\r\n", method, path);
    REQUIRE(send(fd, raw, (size_t)n, 0) == n && shutdown(fd, SHUT_WR) == 0);
    f.serving = 1; f.refuse_send = blocked;
    web_poll(f.web);
    web_action action;
    while (web_take_pending(f.web, &action)) web_apply_pending(f.web, action);
    web_poll(f.web);
    while (web_take_pending(f.web, &action)) web_apply_pending(f.web, action);
    web_poll(f.web);
    f.serving = f.refuse_send = 0;
    int bytes = 0;
    for (;;) {
        REQUIRE(poll(&ready, 1, 1000) > 0);
        int k = (int)recv(fd, reply + bytes, sizeof reply - 1 - (size_t)bytes, 0);
        REQUIRE(k >= 0);
        if (!k) break;
        bytes += k;
        REQUIRE(bytes < (int)sizeof reply - 1);
    }
    reply[bytes] = 0;
    unsigned radio_left = 0;
    if (!blocked) {
        unsigned length = 0;
        char *body = strstr(reply, "\r\n\r\n"), *header = strstr(reply, "\r\nContent-Length: ");
        REQUIRE(bytes >= 12 && !strncmp(reply, "HTTP/1.1 200", 12));
        REQUIRE(body && header && sscanf(header, "\r\nContent-Length: %u", &length) == 1);
        REQUIRE((size_t)bytes == (size_t)(body + 4 - reply) + length);
        const char *field = strstr(body + 4, "\"radio_left\"");
        if (field) {
            REQUIRE(sscanf(field, "\"radio_left\" : %u", &radio_left) == 1);
            REQUIRE(radio_left <= 4);
        }
    } else if (f.retry) violation(RETRY_DELIVERY);
    REQUIRE(close(fd) == 0);
    if (f.stop) release_now();
    return radio_left;
}
static void connected(void *ud, link_ref ref, const pad_info *info)
{
    (void)ud; (void)info;
    int slot = ref.slot;
    REQUIRE(slot >= 0 && slot < 4);
    f.connects++;
    invocation *r = &f.history[f.link_invocation];
    if (!f.link_invocation || r->origin.foreign || !r->ack || !r->terminal)
        violation(TERMINAL_REQUIRED);
    f.slot_invocation[slot] = f.link_invocation;
    if (++f.connected_invocation[f.link_invocation] > 1) violation(CALLBACK_ONCE);
    REQUIRE(vpad_add(slot));
}
static void input(void *ud, link_ref ref, const pad_state *state)
{
    (void)ud;
    if (f.foreign_handle || (f.handle_live && f.history[f.handle_owner].origin.foreign))
        f.foreign_inputs++;
    else if (f.handle_live) f.last_input = now_ms();
    vpad_update(ref.slot, state);
}
static void disconnected(void *ud, link_ref ref)
{
    (void)ud;
    int slot = ref.slot;
    f.disconnects++;
    if (f.disconnects > f.connects) violation(CALLBACK_ONCE);
    unsigned invocation = f.slot_invocation[slot];
    if (++f.retired_invocation[invocation] > f.connected_invocation[invocation])
        violation(CALLBACK_ONCE);
    vpad_remove(slot);
}

static void raw(unsigned op, const unsigned char *p, int n)
{
    unsigned char reply[256];
    int length;
    host_raw_cmd(f.host, op, p, n, reply, &length);
}
static void tick(unsigned ms)
{
    f.controller.ops->pump(f.controller.ctx, (int)ms);
    if (f.host) host_poll(f.host, 0);
    vpad_poll(now_ms());
}
static void execute(step s)
{
    unsigned char p[16] = {0};
    unsigned scan = f.le ? 0x200c : 0x0c1a;
    switch (s.action) {
    case TICK: tick(s.value); break;
    case PAIR: host_pair(f.host, 30); break;
    case OUTPUT: {
        pad_output out = {200, 100, 1, 2, 3, 1};
        host_set_output(f.host, 0, &out); break;
    }
    case STATE: request("GET", "/api/state", 0); break;
    case RETRY:
        f.bt = WEB_BT_FAILED; request("POST", "/api/retry", 0);
        REQUIRE(f.retry == 1); f.retry = 0; break;
    case BLOCKED_RETRY: f.bt = WEB_BT_FAILED; request("POST", "/api/retry", 1); f.retry = 0; break;
    case STOP: request("POST", "/api/stop", 0); break;
    case NATIVE: {
        static const unsigned char addr[6] = {0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48};
        if (!f.le) sim_peer_request(f.classic, addr, 0x002508);
        release_now(); tick(50);
        if (!f.closes) violation(NATIVE_RELEASE);
        break;
    }
    case LOSS: f.lost = 1; release_now(); tick(50); break;
    case DELAY_ACK: delay_events(0x0e, 1, 100); raw(scan, p, f.le ? 2 : 1); break;
    case LOSE_ACK:
        f.assumptions |= 1;
        delay_events(0x0e, 1, -1);
        raw(scan, p, f.le ? 2 : 1); break;
    case DUPLICATE_ACK: case FOREIGN_ACK: {
        raw(scan, p, f.le ? 2 : 1);
        host_poll(f.host, 0); /* deliver the automatic barrier left by the synchronous ACK */
        /* An unchanged write can be suppressed. Use the actual acknowledged
         * setting and its automatic barrier, not the last synchronous call. */
        REQUIRE(f.scan_answer.length == 4 && f.rand_answer.length == 12);
        REQUIRE(f.scan_answer.origin.invocation != f.rand_answer.origin.invocation);
        f.copy_op = scan; f.copy_stage = 1;
        f.copy_foreign = s.action == FOREIGN_ACK;
        refuse_command(scan);
        p[0] = !f.history[f.scan_answer.origin.invocation].p[0];
        raw(scan, p, f.le ? 2 : 1);
        break;
    }
    case REFUSE: case UNCERTAIN:
        f.inject_op = scan; f.fault = s.action;
        raw(scan, p, f.le ? 2 : 1); break;
    case FOREIGN_CREATE: f.foreign_create = 1; break;
    case VANISH:
        if (f.last_input && f.link_invocation && !f.history[f.link_invocation].origin.foreign)
            f.reassignment_pending = 1;
        f.reassignment_version = f.handle_version;
        f.reassignment_handle = f.handle_value;
        if (!f.le) sim_vanish(f.classic);
        f.handle_live = 0;
        f.assumptions |= 1; f.last_input = 0;
        break;
    case REUSE:
        if (!f.le && f.scenario == 8) {
            const unsigned char peer[13] = {1,2,3,4,5,6};
            REQUIRE(sim_foreign_command(f.classic, 0x0405, peer, sizeof peer));
        } else if (!f.le) sim_power_on(f.classic);
        tick(50); break;
    case ID_ZERO: f.identity = 0; break;
    case ID_TWO: f.identity = 2; break;
    case ADD_REFUSED: f.add_fail = 1; break;
    case DELETE_REFUSED: f.delete_fail = 1; break;
    case QUEUE_FOUR: for (int i = 0; i < 4; i++) vpad_add(i); break;
    case REENTER: f.reenter = f.reentry_case = 1; break;
    case CHURN: {
        if (f.le) break;
        sim_go_deaf(f.classic);
        f.churn = 120; f.assumptions |= 1;
        long before = now_ms();
        f.transport.ops->pump(f.transport.ctx, 100);
        if (now_ms() - before > 100) violation(CLEANUP_BOUND);
        /* A corrected bounded pump may leave work, but it cannot consume
         * unlimited completions in one call. Clear only the injected source. */
        f.churn = 0;
        break;
    }
    case CUTOFF: case DEADLINE:
        if (f.released) f.controller.ops->pump(f.controller.ctx,
            (int)(f.release_at + (s.action == CUTOFF ? 9750 : 10000) - now_ms()));
        break;
    default: REQUIRE(0);
    }
}

static const unsigned char generic_descriptor[] = {
    5,1,9,5,0xa1,1, 0x15,0,0x26,0xff,0,0x75,8,0x95,4,
    9,0x30,9,0x31,9,0x32,9,0x35,0x81,2,
    0x15,0,0x25,7,0x75,4,0x95,1,9,0x39,0x81,0x42,
    0x75,4,0x95,1,0x81,3, 5,9,0x19,1,0x29,0x10,
    0x15,0,0x25,1,0x75,1,0x95,0x10,0x81,2,0xc0
};
static void begin(unsigned seed)
{
    memset(&f, 0, sizeof f);
    attrib_reset();
    event_len = event_offset = 0;
    f.seed = seed; f.scenario = seed % 16; f.le = f.scenario == 2 || f.scenario == 5;
    f.positive = f.scenario < 3; f.identity = 1; f.authid = 0x4800000000000001ull;
    if (f.le) {
        sim_le_pad p = {0};
        const unsigned char addr[] = {0x51,0x52,0x53,0x54,0x55,0xd6};
        memcpy(p.addr, addr, 6);
        p.vid = 0x045e; p.pid = 0x0b13; p.sc = 1; p.seed = seed;
        p.report_len = 16; p.report[0] = p.report[1] = 0xff;
        p.report[3] = p.report[5] = p.report[7] = 0x80; p.report[13] = 1;
        p.rid_in = 1; p.rid_out = 3; p.mtu = 185; p.map[0] = 5; p.map_len = 1;
        f.low_energy = sim_le_new(&p); f.controller = sim_le_hci(f.low_energy);
        sim_le_observe(f.low_energy, controller_observation, NULL);
    } else {
        sim_pad p = {0};
        const unsigned char addr[] = {1,2,3,4,5,6};
        memcpy(p.addr, addr, 6);
        p.cod = 0x002508; p.vid = 0x054c; p.pid = 0x05c4; p.seed = seed;
        p.report_len = 78; p.report[0] = 0x11; p.report[1] = 0xc0;
        p.report[3] = 0x20; p.report[4] = 0xe0; p.report[5] = p.report[6] = 0x80; p.report[7] = 0x20;
        if (f.scenario == 1) {
            p.vid = 0x2dc8; p.pid = 0x6001; p.report_len = 7;
            p.report[0] = 0xff; p.report[1] = p.report[2] = p.report[3] = 0x80;
            p.report[4] = 4; p.report[5] = 1; p.report[6] = 0;
            memcpy(p.desc, generic_descriptor, sizeof generic_descriptor);
            p.desc_len = sizeof generic_descriptor;
        }
        f.classic = sim_new(&p); f.controller = sim_hci(f.classic);
        sim_observe(f.classic, controller_observation, NULL);
    }
    f.start = now_ms();
    REQUIRE(pipe(f.logpipe) == 0);
    REQUIRE(fcntl(f.logpipe[0], F_SETFL, O_NONBLOCK) == 0);
    REQUIRE(vpad_init());
    REQUIRE(hci_usb_open(&f.transport));
    if (f.scenario == 15) {
        bond_store store = {.adapter = {.addr = {0x66,0x55,0x44,0x33,0x22,0x11}, .vid = 0x0e8d, .pid = 0x3603}};
        for (unsigned i = 0; i < BOND_MAX; i++) {
            db_rec r = {.addr = {0x80,2,3,4,5,6}, .vid = 0x054c, .pid = 0x05c4, .complete = 1};
            r.addr[0] = (unsigned char)(0x80 + i); memset(r.key, (int)i + 1, sizeof r.key);
            REQUIRE(bond_store_put(&store, &r) == BOND_COMMITTED);
        }
        REQUIRE(bond_store_save(db, &store) == BOND_COMMITTED);
    }
    host_events ev = {connected, input, disconnected, NULL, NULL};
    f.host = host_prepare(f.transport, db, &ev);
    REQUIRE(f.host != NULL);
    REQUIRE(hci_usb_bind_ownership(&f.transport, host_ownership(f.host), NULL, NULL));
    if (!host_start(f.host)) {
        host_close(f.host);
        f.host = NULL;
    }
    if (f.positive) REQUIRE(f.host != NULL);
    if (f.scenario == 15) REQUIRE(f.host && host_paired_count(f.host) == 16);
    f.bt = f.host ? WEB_BT_OK : WEB_BT_FAILED;
    web_cfg cfg = {.host = &f.host, .stop = &f.stop, .retry = &f.retry, .bt_state = &f.bt,
        .bt_reason = "integration fixture", .version = "integration-generator"};
    f.web = web_start(&cfg, 0);
    REQUIRE(f.web && f.port);
}
static void finish(void)
{
    if (getenv("INTEGRATION_LOG"))
        fprintf(stderr, "OBSERVED seed=%u connects=%d disconnects=%d input=%d Add=%d generation=%d commands=%d ACL=%d\n",
            f.seed, f.connects, f.disconnects, f.inputs, f.ndevices, f.generation, f.commands, f.acl);
    if (f.positive && !f.inputs) violation(POSITIVE_INPUT);
    release_now();
    if (unsafe_attribution_control) {
        /* Feed the real wire oracle the restore an opcode-only owner would
         * emit after the copied ACKs. The causal guard must reject it. */
        const unsigned char restore[] = {0x1a, 0x0c, 1, 0};
        wire_command(restore);
        REQUIRE(!(f.policy_bad[1] & (1u << CAUSAL_UNDO)));
    }
    if (f.host) { host_close(f.host); f.host = NULL; }
    if (f.disconnects != f.connects) violation(CALLBACK_ONCE);
    for (unsigned i = 0; i < MAX_COMMANDS; i++)
        if (f.connected_invocation[i] != f.retired_invocation[i]) violation(CALLBACK_ONCE);
    for (int i = 0; i < 4; i++) vpad_remove(i);
    vpad_poll(now_ms());
    f.controller.ops->pump(f.controller.ctx, 301);
    vpad_poll(now_ms());
    f.controller.ops->pump(f.controller.ctx, 3001);
    vpad_poll(now_ms());
    for (int i = 0; i < 4; i++) vpad_remove(i);
    for (int i = 0; i < f.ndevices; i++) {
        device *d = &f.devices[i];
        if (d->identified && d->deletes != 1 && !f.reentry_case) violation(VENDOR_IDENTITY);
        if (!d->identified && d->deletes) violation(VENDOR_IDENTITY);
    }
    if (now_ms() > f.release_at + 10000) violation(CLEANUP_BOUND);
    if (f.foreign_handle) {
        /* Observe the public summary's machine value, not translated copy.
         * The expected count comes from the foreign controller inventory. */
        if (request("GET", "/api/state", 0) != 1) violation(LEFT_CONNECTED);
    }
    /* Measure before the fixture safety net, so a missing close is observable. */
    if (f.opens != 1 || f.uninits != 1 || f.closes != 1) violation(USB_INVENTORY);
    if (f.fd_open && !mutated("leak-usb")) f.transport.ops->close(f.transport.ctx);
    REQUIRE(vpad_creds_restore());
    REQUIRE(f.authid == 0x4800000000000001ull);
    static const unsigned char original_caps[16] = {0};
    REQUIRE(!memcmp(f.caps, original_caps, sizeof original_caps));
    web_stop(f.web);
    REQUIRE(close(f.logpipe[0]) == 0 && close(f.logpipe[1]) == 0);
    f.controller.ops->close(f.controller.ctx);
    if (f.le) sim_le_free(f.low_energy); else sim_free(f.classic);
    const char *suffix[] = {"", ".new", ".irk"};
    for (unsigned i = 0; i < sizeof suffix / sizeof suffix[0]; i++) {
        char path[180]; snprintf(path, sizeof path, "%s%s", db, suffix[i]);
        REQUIRE(unlink(path) == 0 || errno == ENOENT);
    }
}
static unsigned random_next(unsigned *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}
static void schedule(unsigned seed, step *out, unsigned n)
{
    unsigned rng = seed + 0x9e3779b9u;
    for (unsigned i = 0; i < n; i++) {
        unsigned r = random_next(&rng);
        out[i] = (step){r % 12 == 0 ? STATE : r % 12 == 1 ? OUTPUT : TICK, 100 + r % 101};
    }
    out[0] = (step){PAIR, 0};
    switch (seed % 16) {
    case 3:
        out[1].action = DELAY_ACK; out[24].action = CHURN; out[25].action = REFUSE; break;
    case 4: out[1].action = DUPLICATE_ACK; break;
    case 5: out[1].action = LOSE_ACK; break;
    case 6: out[1].action = FOREIGN_ACK; break;
    case 7:
        for (unsigned i = 2; i < 48; i++) out[i].action = TICK;
        out[0].action = FOREIGN_CREATE; out[1].action = PAIR; out[48].action = STOP; break;
    case 8:
        for (unsigned i = 1; i < 48; i++) out[i].action = TICK;
        out[48].action = VANISH; out[49].action = REUSE; out[50].action = STOP; break;
    case 9: out[0].action = ID_ZERO; out[1].action = QUEUE_FOUR; out[2].action = STOP; break;
    case 10:
        out[24].action = NATIVE; out[25].action = CUTOFF; out[26].action = DEADLINE; break;
    case 11: out[24].action = LOSS; break;
    case 12: out[0].action = ID_TWO; out[1].action = QUEUE_FOUR; break;
    case 13: out[0].action = ADD_REFUSED; out[1].action = QUEUE_FOUR; break;
    case 14: out[24].action = BLOCKED_RETRY; out[25].action = RETRY; break;
    case 15:
        out[0].action = QUEUE_FOUR; out[24].action = UNCERTAIN;
        out[30].action = REENTER; out[31].action = DELETE_REFUSED; break;
    default: break;
    }
    out[n - 2].action = STOP;
    out[n - 1] = (step){TICK, 1};
}
static unsigned replay(unsigned seed, const step *steps, unsigned n)
{
    begin(seed);
    for (unsigned i = 0; i < n && f.host; i++) execute(steps[i]);
    finish();
    runs++;
    return f.policy_bad[0] | f.policy_bad[1];
}
static void shrink(unsigned seed, step *steps, unsigned n, enum property property)
{
    unsigned bit = 1u << property;
    /* Rebuild every fixture. Deleting a command also deletes its generated
     * receipts; no copied host pointers or causally orphaned answers survive. */
    for (unsigned chunk = n / 2; chunk; chunk /= 2) {
        for (unsigned at = 0; at + chunk <= n;) {
            step candidate[MAX_STEPS];
            memcpy(candidate, steps, at * sizeof *steps);
            memcpy(candidate + at, steps + at + chunk, (n - at - chunk) * sizeof *steps);
            if (replay(seed, candidate, n - chunk) & bit) {
                memcpy(steps, candidate, (n - chunk) * sizeof *steps); n -= chunk; at = 0;
            } else at++;
        }
    }
    printf("SHRUNK deletion-minimal seed=%u property=%s steps=%u: open", seed, properties[property].name, n);
    for (unsigned i = 0; i < n; i++) printf(" %s(%u)", actions[steps[i].action], steps[i].value);
    puts(" finalize");
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--strict-fixture")) {
        if (!strcmp(argv[2], "complete")) return strict_fixture(0);
        if (!strcmp(argv[2], "missing")) return strict_fixture(1);
        return 2;
    }
    unsigned seeds = 500, steps = 64, first = 0;
    int strict = 0, single = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--full")) { seeds = 10000; steps = 128; }
        else if (!strcmp(argv[i], "--expected-failures")) strict = 1;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            char *end;
            errno = 0;
            unsigned long value = strtoul(argv[++i], &end, 0);
            if (errno || end == argv[i] || *end || value > 1000000) return 2;
            first = (unsigned)value; seeds = 1; single = 1;
        }
        else if (!strcmp(argv[i], "--mutate") && i + 1 < argc) { mutation = argv[++i]; seeds = 1; }
        else if (!strcmp(argv[i], "--unsafe-attribution-control")) unsafe_attribution_control = 1;
        else { fprintf(stderr, "invalid integration argument: %s\n", argv[i]); return 2; }
    }
    if (first > 1000000) return 2;
    if (unsafe_attribution_control && (!single || first != 4 || strict || mutation)) return 2;
    if (mutation && !mutated("refuse-everything") && !mutated("drop-input") &&
        !mutated("foreign-delete") && !mutated("duplicate-delete") && !mutated("truncate-http") &&
        !mutated("leak-usb") && !mutated("drop-delete")) return 2;
    if (strict && (single || mutation)) {
        fprintf(stderr, "STRICT_ERROR scope=partial-profile requires=fast-or-full\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const char *log_path = getenv("INTEGRATION_LOG");
    REQUIRE(log_open(log_path ? log_path : "/dev/null"));
    /* Persistence has its own tests; these disposable bonds must not turn
     * 10,000 integration schedules into disk-journal throughput tests. */
    snprintf(directory, sizeof directory, "/dev/shm/whateverpad-integration-XXXXXX");
    REQUIRE(mkdtemp(directory) != NULL);
    snprintf(db, sizeof db, "%s/bonds", directory);
    unsigned counts[2][PROPERTY_COUNT] = {{0}}, residual[2][PROPERTY_COUNT] = {{0}};
    unsigned sample[PROPERTY_COUNT], assumptions[8] = {0};
    for (unsigned p = 0; p < PROPERTY_COUNT; p++) sample[p] = ~0u;
    unsigned hard = 0, foreign_stimuli = 0, reassignment_stimuli = 0, failing_properties = 0;
    unsigned duplicate_stimuli = 0, unexpected_passes = 0, expected_cases = 0;
    for (unsigned seed = first; seed < first + seeds; seed++) {
        step trace[MAX_STEPS];
        schedule(seed, trace, steps);
        unsigned bad = replay(seed, trace, steps);
        if (f.scenario == 7) { REQUIRE(f.foreign_stimulus); foreign_stimuli++; }
        if (f.scenario == 8) { REQUIRE(f.reassignment_stimulus); reassignment_stimuli++; }
        if (f.scenario == 4) { REQUIRE(f.duplicate_copies == 3); duplicate_stimuli++; }
        assumptions[f.assumptions]++;
        if (f.positive && !(bad & (1u << POSITIVE_INPUT))) positives[f.scenario]++;
        for (unsigned p = 0; p < PROPERTY_COUNT; p++) {
            for (unsigned policy = 0; policy < 2; policy++)
                if (f.policy_bad[policy] & (1u << p)) {
                    counts[policy][p]++;
                    if (f.assumptions) residual[policy][p]++;
                }
            if (bad & (1u << p)) {
                if (sample[p] == ~0u) sample[p] = seed;
                if (!properties[p].todo) hard++;
            }
        }
    }
    if (strict) {
        expectation entries[2 * PROPERTY_COUNT];
        for (unsigned policy = 0; policy < 2; policy++) for (unsigned p = 0; p < PROPERTY_COUNT; p++) {
            if (!properties[p].todo || (policy && p == NO_RESTORE_UNDO)) continue;
            entries[expected_cases++] = (expectation){
                policy ? "SINGLE_PRODUCER-projection" : "NO_RESTORE-projection",
                properties[p].name, properties[p].todo, counts[policy][p]
            };
        }
        unexpected_passes = validate_expectations(entries, expected_cases);
    }
    for (unsigned policy = 0; policy < 2; policy++) for (unsigned p = 0; p < PROPERTY_COUNT; p++) {
        printf("PROPERTY policy=%s %s violations=%u/%u eligible-violations=%u residual-violations=%u %s",
            policy ? "SINGLE_PRODUCER-projection" : "NO_RESTORE-projection",
            properties[p].name, counts[policy][p], seeds,
            counts[policy][p] - (policy ? residual[policy][p] : 0), policy ? residual[policy][p] : 0,
            counts[policy][p] ? properties[p].todo ? "XFAIL" : "FAIL" : "PASS");
        if (counts[policy][p]) printf(" todo=%d seed=%u", properties[p].todo, sample[p]);
        putchar('\n');
    }
    for (unsigned i = 0; i < 8; i++) if (assumptions[i])
        printf("ASSUMPTIONS L1=%s B1=%s single-producer=%s schedules=%u (violating schedules retained)\n",
            i & 1 ? "violated" : "respected", i & 2 ? "violated" : "respected", i & 4 ? "violated" : "respected", assumptions[i]);
    for (unsigned p = 0; p < PROPERTY_COUNT; p++) if (sample[p] != ~0u && (single || strict)) {
        step trace[MAX_STEPS]; schedule(sample[p], trace, steps);
        shrink(sample[p], trace, steps, (enum property)p);
    }
    for (unsigned p = 0; p < PROPERTY_COUNT; p++) failing_properties += sample[p] != ~0u;
    REQUIRE(rmdir(directory) == 0);
    log_close();
    printf("INTEGRATION seeds=%u first=%u steps=%u scheduled=%u checks=%u commands=%u generated=%u delivered=%u empty-error-completions=%u"
           " positives=DS4:%u,8BitDo:%u,LE:%u foreign-Create-with-fresh-input-before-refusal=%u"
           " unobserved-reassignment=%u two-duplicate-traces=%u hard=%u xpass=%u replays=%u\n",
        seeds, first, steps, seeds * steps, checks, submitted, generated_events, deliveries, empty_completions,
        positives[0], positives[1], positives[2], foreign_stimuli, reassignment_stimuli,
        duplicate_stimuli, hard, unexpected_passes, runs - seeds);
    return integration_result(hard, unexpected_passes, strict && failing_properties);
}
