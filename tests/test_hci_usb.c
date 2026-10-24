/* Real hci_usb operations; only the OS boundary and clock are stood in for. */
#include "../src/hci_usb.h"
#include "../src/attrib.h"
#include "../src/util.h"
#include "usb_standin/dev/usb/usb_ioctl.h"
#include "usb_standin/receipt.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { EVENTS = 40, ACL = 20, OUT = 60, CMD = 62, XFERS = 63, FD = 73 };
static int failures, checks, pending, strict_mode;
/* A failed effect invalidates later fixture arrangements; do not cascade into a fixture crash. */
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; exit(1); } } while (0)

static struct {
    struct usb_fs_endpoint *ep;
    int fd_open, initialized, opens, inits, uninits, closes;
    int opened[XFERS], busy[XFERS], queued[XFERS], stops[XFERS], starts[XFERS];
    struct { int index, len; unsigned char data[HCI_PKT_MAX]; } done[128];
    int head, count, missing_completion;
    unsigned long fail_request;
    int fail_index, stop_failures, virtual_waits;
    struct { const char *name; int index; } trace[2048];
    int ntrace;
    int empty_stream, empty_completions;
    int churn_left, churn_completed;
    int completion_cost;
    int reject_node, node_attempts, alternate_out;
    int commands, hold_command, command_status;
    int outputs, output_status, last_acl_len;
    unsigned char last_acl[HCI_PKT_MAX];
    unsigned last_opcode;
    unsigned char last_params[255];
    int last_plen;
    void (*start_hook)(void *);
    void *start_ud;
} usb;
static long clock_ms;
static long clock_now(void) { return clock_ms; }

static void record(const char *name, int index)
{
    assert(usb.ntrace < 2048);
    usb.trace[usb.ntrace].name = name;
    usb.trace[usb.ntrace++].index = index;
}

static void transcript(const char *scenario)
{
    int i;
    printf("USB %s:", scenario);
    for (i = 0; i < usb.ntrace; i++) {
        printf(" %s", usb.trace[i].name);
        if (usb.trace[i].index >= 0) printf(":%d", usb.trace[i].index);
    }
    printf(" | open=%d init=%d uninit=%d close=%d waits=%d\n",
           usb.opens, usb.inits, usb.uninits, usb.closes, usb.virtual_waits);
}

static void reset(void)
{
    assert(!usb.fd_open);
    memset(&usb, 0, sizeof usb);
    memset(&test_usb_receipts, 0, sizeof test_usb_receipts);
    usb.fail_index = -1;
    clock_ms = 1000;
    ph_clock_hook = clock_now;
}

int test_usb_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!usb.fd_open);
    usb.node_attempts++;
    if (usb.reject_node) { errno = ENXIO; return -1; }
    assert(strcmp(path, "/dev/ugen0.2") == 0);
    record("OPEN", -1);
    usb.opens++;
    usb.fd_open = 1;
    return FD;
}

int test_usb_close(int fd)
{
    assert(fd == FD && usb.fd_open);
    CHECK(!usb.initialized);
    record("CLOSE", -1);
    usb.closes++;
    usb.fd_open = 0;
    return 0;
}

/* Every call is an advance of virtual time, never a wall-clock sleep. */
int test_usb_usleep(useconds_t us)
{
    assert(++usb.virtual_waits <= 2000);
    clock_ms += us / 1000 ? us / 1000 : 1;
    return 0;
}

static void completion(int index, const unsigned char *data, int len)
{
    int slot = (usb.head + usb.count) % 128;
    assert(index >= 0 && index < XFERS && !usb.queued[index] && usb.count < 128);
    assert(len <= HCI_PKT_MAX);
    usb.done[slot].index = index;
    usb.done[slot].len = len;
    if (len) memcpy(usb.done[slot].data, data, (size_t)len);
    usb.queued[index] = 1;
    usb.count++;
}

int test_usb_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    int index = -1;
    assert(fd == FD && usb.fd_open);
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (request == USB_FS_OPEN || request == USB_FS_START || request == USB_FS_STOP)
        index = *(uint8_t *)arg;
    if (request == USB_FS_STOP) {
        assert(index < XFERS);
        record("STOP", index);
        usb.stops[index]++;
    }
    if (request == usb.fail_request && (usb.fail_index < 0 || index == usb.fail_index)) {
        record("IOCTL_FAIL", index);
        usb.fail_request = 0;
        if (request == USB_FS_STOP) usb.stop_failures++;
        errno = EIO;
        return -1;
    }
    switch (request) {
    case USB_GET_DEVICE_DESC: {
        struct usb_device_descriptor *d = arg;
        memset(d, 0, sizeof *d);
        USETW(d->idVendor, 0x0e8d); USETW(d->idProduct, 0x3603);
        return 0;
    }
    case USB_GET_FULL_DESC: {
        /* One HCI function, no alternate OUT: the endpoint picker is real. */
        static const uint8_t cfg[] = {
            9, 2, 39, 0, 1, 1, 0, 0x80, 0,
            9, 4, 0, 0, 3, 0xe0, 1, 1, 0,
            7, 5, 0x81, 3, 16, 0, 1,
            7, 5, 0x82, 2, 0, 2, 0,
            7, 5, 0x02, 2, 0, 2, 0
        };
        struct usb_gen_descriptor *d = arg;
        assert(d->ugd_maxlen >= sizeof cfg);
        memcpy(d->ugd_data, cfg, sizeof cfg);
        d->ugd_actlen = sizeof cfg;
        if (usb.alternate_out) {
            static const uint8_t spare[] = {7, 5, 0x02, 2, 0, 2, 0};
            uint8_t *out = d->ugd_data;
            assert(d->ugd_maxlen >= sizeof cfg + sizeof spare);
            out[2] += sizeof spare; out[13] = 4; out[34] = 0x01;
            memcpy(out + sizeof cfg, spare, sizeof spare);
            d->ugd_actlen += sizeof spare;
        }
        return 0;
    }
    case USB_FS_INIT: {
        struct usb_fs_init *init = arg;
        assert(!usb.initialized && init->ep_index_max == XFERS);
        usb.ep = init->pEndpoints;
        usb.initialized = 1; usb.inits++;
        record("INIT", -1);
        return 0;
    }
    case USB_FS_OPEN:
        assert(usb.initialized && index < XFERS && !usb.opened[index]);
        usb.opened[index] = 1;
        record("XOPEN", index);
        return 0;
    case USB_FS_START:
        assert(usb.initialized && usb.opened[index] && !usb.busy[index] && !usb.queued[index]);
        usb.busy[index] = 1; usb.starts[index]++;
        record("START", index);
        if (index == CMD) {
            const struct usb_device_request *r = usb.ep[index].ppBuffer[0];
            const unsigned char *p = usb.ep[index].ppBuffer[1];
            CHECK(usb.ep[index].nFrames == 2);
            CHECK(usb.ep[index].pLength[0] == sizeof *r);
            CHECK(r->bmRequestType == UT_WRITE_CLASS_DEVICE && !r->bRequest &&
                  !UGETW(r->wValue) && !UGETW(r->wIndex));
            CHECK(UGETW(r->wLength) == usb.ep[index].pLength[1]);
            CHECK(usb.ep[index].pLength[1] == (unsigned)(3 + p[2]));
            usb.commands++;
            usb.last_opcode = le16(p);
            usb.last_plen = p[2];
            if (p[2]) memcpy(usb.last_params, p + 3, p[2]);
            if (!usb.hold_command) completion(index, NULL, 0);
        }
        if (index == OUT || index == OUT + 1) {
            unsigned n = usb.ep[index].pLength[0];
            CHECK(n >= 4 && n <= HCI_PKT_MAX);
            CHECK(n == 4 + le16((unsigned char *)usb.ep[index].ppBuffer[0] + 2));
            usb.outputs++;
            usb.last_acl_len = (int)n;
            memcpy(usb.last_acl, usb.ep[index].ppBuffer[0], n);
        }
        if (usb.start_hook) usb.start_hook(usb.start_ud);
        if (index == 0 && usb.empty_stream) completion(0, NULL, 0);
        return 0;
    case USB_FS_STOP:
        assert(usb.initialized && usb.opened[index]);
        usb.busy[index] = 0;
        if (!usb.missing_completion && !usb.queued[index]) completion(index, NULL, 0);
        return 0;
    case USB_FS_COMPLETE: {
        struct usb_fs_complete *d = arg;
        int n;
        if (usb.churn_left) {
            usb.churn_left--;
            usb.churn_completed++;
            d->ep_index = 255;
            return 0;
        }
        if (!usb.count) { errno = EAGAIN; return -1; }
        clock_ms += usb.completion_cost;
        index = usb.done[usb.head].index;
        n = usb.done[usb.head].len;
        d->ep_index = (uint8_t)index;
        usb.ep[index].pLength[0] = (uint32_t)n;
        usb.ep[index].aFrames = n > 0;
        usb.ep[index].status = index == CMD ? (unsigned)usb.command_status :
                              index == OUT || index == OUT + 1 ? (unsigned)usb.output_status :
                              n ? 0 : 1;
        if (n) memcpy(usb.ep[index].ppBuffer[0], usb.done[usb.head].data, (size_t)n);
        usb.busy[index] = usb.queued[index] = 0;
        usb.head = (usb.head + 1) % 128; usb.count--;
        record("COMPLETE", index);
        if (index == 0 && usb.empty_stream) {
            clock_ms += 100;
            if (++usb.empty_completions == 120) usb.empty_stream = 0;
        }
        return 0;
    }
    case USB_FS_UNINIT:
        record("UNINIT", -1);
        usb.uninits++;
        usb.initialized = 0;
        memset(usb.busy, 0, sizeof usb.busy);
        memset(usb.opened, 0, sizeof usb.opened);
        memset(usb.queued, 0, sizeof usb.queued);
        usb.count = usb.head = 0;
        return 0;
    case USB_DO_REQUEST:
        record("COMMAND", -1);
        return 0;
    default:
        assert(!"unexpected ioctl");
        return -1;
    }
}

static int busy(int first, int end)
{
    int i, n = 0;
    for (i = first; i < end; i++) n += usb.busy[i];
    return n;
}

static hci_t start(void)
{
    hci_t h = {0};
    CHECK(hci_usb_open(&h));
    assert(h.ops);
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(busy(0, EVENTS) == EVENTS && busy(EVENTS, OUT) == ACL);
    return h;
}

static void closed_once(hci_t h)
{
    int n;
    h.ops->close(h.ctx);
    n = usb.ntrace;
    h.ops->close(h.ctx);
    CHECK(usb.ntrace == n);
    CHECK(usb.opens == 1 && usb.uninits == 1 && usb.closes == 1);
    CHECK(!usb.fd_open && !usb.initialized && busy(0, XFERS) == 0);
    CHECK(n >= 2 && strcmp(usb.trace[n - 2].name, "UNINIT") == 0 &&
          strcmp(usb.trace[n - 1].name, "CLOSE") == 0);
}

static void test_close_busy_reads(void)
{
    int i;
    hci_t h;
    reset();
    h = start();
    usb.missing_completion = 1; /* no event/ACL read will finish on its own */
    closed_once(h);
    for (i = 0; i < OUT; i++) CHECK(usb.stops[i] == 1);
    CHECK(usb.stops[OUT] == 0 && usb.virtual_waits == 0);
    transcript("close-busy-reads-without-completions");
}

static void test_expected_current_yield_is_not_close(void)
{
    unsigned char packet[32];
    static const unsigned char acl[] = { 0x41, 0x20, 1, 0, 0xaa };
    hci_t h;
    int i;
    reset(); h = start();
    h.ops->yield(h.ctx, 30000);
    CHECK(busy(0, EVENTS) == 0 && busy(EVENTS, OUT) == ACL);
    CHECK(usb.uninits == 0 && usb.closes == 0);
    for (i = 0; i < OUT; i++) CHECK(usb.opened[i]); /* allocation was NOT released */
    completion(EVENTS, acl, sizeof acl);
    CHECK(h.ops->pump(h.ctx, 0) == 1);
    CHECK(h.ops->next_acl(h.ctx, packet, sizeof packet) == sizeof acl);
    CHECK(memcmp(packet, acl, sizeof acl) == 0);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == 0);
    clock_ms += 30000;
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(busy(0, EVENTS) == EVENTS); /* resumes, unlike terminal release */
    closed_once(h);
    transcript("expected-current-30s-yield-retains-ACL-and-all-allocations");
}

static void test_late_response_and_missing_stop_completion(void)
{
    static const unsigned char ev[] = { 0x0f, 4, 0, 1, 5, 4 };
    unsigned char out[16];
    hci_t h;
    reset(); h = start();
    completion(0, ev, sizeof ev); /* already completed when STOP races it */
    h.ops->yield(h.ctx, 30000);
    CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof ev);
    CHECK(memcmp(out, ev, sizeof ev) == 0); /* yield cannot retract an arrival */
    closed_once(h);
    transcript("late-response-already-completed-before-yield");

    reset(); h = start();
    usb.missing_completion = 1;
    h.ops->yield(h.ctx, 30000);
    clock_ms += 30000;
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(busy(0, EVENTS) == 0); /* current software busy bits never clear */
    for (int i = 0; i < EVENTS; i++) CHECK(usb.starts[i] == 1);
    closed_once(h);
    transcript("expected-current-missing-STOP-completion-prevents-rearm");
}

static void test_ioctl_failures(void)
{
    hci_t h;
    int i;
    reset(); h = start();
    usb.fail_request = USB_FS_STOP; usb.fail_index = 0;
    closed_once(h);
    CHECK(usb.stop_failures == 1);
    for (i = 0; i < OUT; i++) CHECK(usb.stops[i] == 1);
    transcript("STOP-failure-still-uninitializes-and-closes");

    reset(); h = start();
    usb.fail_request = USB_FS_START; usb.fail_index = CMD;
    CHECK(!h.ops->cmd(h.ctx, 0x1009, NULL, 0));
    CHECK(h.ops->pump(h.ctx, 0) == -1);
    closed_once(h);
    transcript("command-EIO-marks-transport-dead");

    reset();
    usb.fail_request = USB_FS_INIT;
    CHECK(!hci_usb_open(&h));
    CHECK(usb.uninits == 0 && usb.closes == 1 && !usb.fd_open);
    transcript("init-failure-does-not-authorize-UNINIT");

    reset();
    usb.fail_request = USB_FS_OPEN; usb.fail_index = 0;
    CHECK(!hci_usb_open(&h));
    CHECK(usb.inits == 1 && usb.uninits == 1 && usb.closes == 1);
    CHECK(usb.opened[OUT] == 0);
    transcript("partial-open-no-event-reader-unwinds");
}

static void test_busy_acl_out(void)
{
    static const unsigned char acl[] = { 0x41, 0x20, 1, 0, 0xaa };
    hci_t h;
    reset(); h = start();
    CHECK(h.ops->acl_send(h.ctx, acl, sizeof acl));
    CHECK(!h.ops->acl_send(h.ctx, acl, sizeof acl)); /* no completion: bounded 500 ms */
    CHECK(clock_ms == 1500 && usb.virtual_waits == 500);
    closed_once(h);
    CHECK(usb.stops[OUT] == 1);
    transcript("busy-ACL-write-timeout-then-full-close");
}

static void test_event_read_operations(void)
{
    hci_t h;
    int i;
    reset(); h = start();
    h.ops->event_reads(h.ctx, 0);
    CHECK(busy(0, EVENTS) == 0 && busy(EVENTS, OUT) == ACL);
    h.ops->event_reads(h.ctx, 2);
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(busy(0, EVENTS) == 2 && busy(EVENTS, OUT) == ACL);
    h.ops->event_len(h.ctx, 8);
    CHECK(busy(0, EVENTS) == 0 && busy(EVENTS, OUT) == ACL);
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(busy(0, EVENTS) == 2 && usb.ep[0].pLength[0] == 8 && usb.ep[1].pLength[0] == 8);
    closed_once(h);
    for (i = 0; i < EVENTS; i++) CHECK(usb.stops[i] == (i < 2 ? 3 : 1));
    transcript("event-read-pause-and-resize-keep-ACL-until-close");

    reset();
    usb.fail_request = USB_FS_OPEN; usb.fail_index = 10;
    CHECK(hci_usb_open(&h));
    CHECK(h.ops->missing_reads(h.ctx) == 30);
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(busy(0, EVENTS) == 10 && busy(EVENTS, OUT) == ACL);
    clock_ms += 4000; /* deterministic top-up boundary, no real waiting */
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(h.ops->missing_reads(h.ctx) == 0 && busy(0, EVENTS) == EVENTS);
    closed_once(h);
    for (i = 0; i < OUT; i++) CHECK(usb.stops[i] == 1);
    transcript("partial-event-open-topup-then-close-all");
}

/* Appendix B / T:433-451, using pump's existing per-call timeout on M. */
static void test_empty_completion_stream_observes_deadline(void)
{
    hci_t h;
    reset(); h = start();
    long deadline = clock_ms + 10000;
    completion(0, NULL, 0);
    usb.empty_stream = 1;
    CHECK(h.ops->pump(h.ctx, 10000) == 0);
    CHECK(clock_ms <= deadline && usb.empty_stream);
    usb.empty_stream = 0;
    closed_once(h);
}

static void test_refused_usb_initialization_has_no_uninit_authority(void)
{
    hci_t h;
    reset(); usb.fail_request = USB_FS_INIT;
    CHECK(!hci_usb_open(&h));
    CHECK(usb.inits == 0 && usb.closes == 1);
    CHECK(usb.uninits == 0);
}

static void test_completion_churn_has_a_finite_batch(void)
{
    hci_t h;
    reset(); h = start();
    usb.churn_left = 1024;
    CHECK(h.ops->pump(h.ctx, 0) == 0);
    CHECK(usb.churn_completed <= 64 && usb.churn_left > 0);
    usb.churn_left = 0;
    closed_once(h);
}

static void test_refused_transfer_opens_never_authorize_stops(void)
{
    static const int indices[] = {CMD, OUT, 0, EVENTS};
    for (unsigned i = 0; i < sizeof indices / sizeof indices[0]; i++) {
        hci_t h;
        reset(); usb.fail_request = USB_FS_OPEN; usb.fail_index = indices[i];
        CHECK(!hci_usb_open(&h));
        CHECK(usb.inits == 1 && usb.uninits == 1 && usb.closes == 1);
        for (int j = 0; j < XFERS; j++) CHECK(usb.stops[j] == 0);
    }
}

static void test_node_and_alternate_output_ownership(void)
{
    static const unsigned char acl[] = {0x41, 0x20, 1, 0, 0xaa};
    hci_t h;
    reset(); usb.reject_node = 1;
    CHECK(!hci_usb_open(&h));
    CHECK(usb.node_attempts == 5 && usb.opens == 0 && usb.uninits == 0 && usb.closes == 0);
    for (int refused = 0; refused < 2; refused++) {
        reset(); usb.alternate_out = 1;
        if (refused) { usb.fail_request = USB_FS_OPEN; usb.fail_index = OUT + 1; }
        h = start();
        CHECK(h.ops->acl_send(h.ctx, acl, sizeof acl));
        closed_once(h);
        CHECK(usb.stops[OUT + 1] == !refused && usb.stops[OUT] == refused);
    }
}

static own_ledger *bound(hci_t *h, own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    CHECK(l != NULL && own_activate(l));
    CHECK(hci_usb_bind_ownership(h, l, NULL, NULL));
    return l;
}

static own_ref prepare(own_ledger *l, unsigned opcode, const unsigned char *p, int n,
                       own_token *token)
{
    own_command c = {.opcode = (uint16_t)opcode, .plen = (uint8_t)n,
                     .ack_by = (uint64_t)clock_ms + 2000,
                     .terminal_by = (uint64_t)clock_ms + 4000};
    own_ref ref = {0};
    if (n) memcpy(c.params, p, (size_t)n);
    if (opcode == 0x0c1a) {
        c.original_known = c.original_len = 1;
        c.original[0] = 2;
    }
    CHECK(own_prepare(l, &c, &ref) == OWN_OK);
    CHECK(own_next_hci(l, (uint64_t)clock_ms, token) == OWN_OK);
    return ref;
}

static void receive_event(hci_t h, const unsigned char *event, int len)
{
    unsigned char out[HCI_PKT_MAX];
    /* Re-arm the consumed reader before arranging its next completion. */
    h.ops->pump(h.ctx, 0);
    completion(0, event, len);
    CHECK(h.ops->pump(h.ctx, 0) == 1);
    CHECK(h.ops->next_event(h.ctx, out, sizeof out) == len);
    CHECK(!memcmp(out, event, (size_t)len));
}

static void acknowledge(hci_t h, unsigned opcode, int status_event, int status)
{
    unsigned char e[] = {0x0e, 4, 1, 0, 0, 0};
    if (status_event) { e[0] = 0x0f; e[2] = (unsigned char)status; e[3] = 1; }
    else e[5] = (unsigned char)status;
    put16(e + (status_event ? 4 : 3), opcode);
    receive_event(h, e, sizeof e);
}

static void fence(hci_t h, own_ledger *l, unsigned value)
{
    unsigned char e[] = {0x0e, 12, 1, 0x18, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    own_token token;
    CHECK(own_next_hci(l, (uint64_t)clock_ms, &token) == OWN_OK);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    CHECK(usb.last_opcode == 0x2018 && usb.last_plen == 0);
    e[6] = (unsigned char)value;
    receive_event(h, e, sizeof e);
}

static void test_tokens_are_exclusive_single_use_and_immutable(void)
{
    static const unsigned char acl[] = {0x41, 0x20, 1, 0, 0xaa};
    unsigned char scan = 1;
    own_token token, other;
    hci_t h;
    own_ledger *l, *foreign;
    reset(); attrib_reset(); h = start(); l = bound(&h, OWN_POLICY_NO_RESTORE);
    foreign = own_new(OWN_POLICY_NO_RESTORE, 1);
    CHECK(foreign != NULL && own_activate(foreign));
    prepare(foreign, 0x1009, NULL, 0, &other);
    CHECK(hci_usb_submit(&h, &other) == OWN_NOT_SENT && usb.commands == 0);
    own_free(foreign);
    prepare(l, 0x0c1a, &scan, 1, &token);
    scan = 0; /* The caller's buffer is not the prepared effect. */
    CHECK(!h.ops->cmd(h.ctx, 0x1009, NULL, 0));
    CHECK(!h.ops->acl_send(h.ctx, acl, sizeof acl));
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    CHECK(usb.commands == 1 && usb.last_opcode == 0x0c1a &&
          usb.last_plen == 1 && usb.last_params[0] == 1);
    CHECK(hci_usb_submit(&h, &token) == OWN_NOT_SENT && usb.commands == 1);
    acknowledge(h, 0x0c1a, 0, 0);
    CHECK(attrib_get()->submitted == 1 && attrib_get()->classes[ATTRIB_SOLICITED] == 1);
    closed_once(h); own_free(l);
}

typedef struct { own_ledger *ledger; int calls, at; } release_point;
static void release_during_preparation(void *ud)
{
    release_point *p = ud;
    if (++p->calls == p->at) own_release(p->ledger, SESSION_STOP, (uint64_t)clock_ms);
}

static void test_release_inside_read_reap_refuses_raw_submit(void)
{
    for (int at = 1; at <= 2; at++) {
        hci_t h;
        own_token token;
        own_ledger *l;
        release_point p;
        reset(); h = start();
        l = own_new(OWN_POLICY_NO_RESTORE, 1);
        CHECK(l != NULL && own_activate(l));
        p = (release_point){l, 0, at};
        CHECK(hci_usb_bind_ownership(&h, l, release_during_preparation, &p));
        prepare(l, 0x1009, NULL, 0, &token);
        completion(0, NULL, 0);
        CHECK(hci_usb_submit(&h, &token) == OWN_NOT_SENT);
        CHECK(p.calls >= at && usb.commands == 0);
        CHECK(!h.ops->cmd(h.ctx, 0x1009, NULL, 0));
        closed_once(h); own_free(l);
    }
}

static void release_inside_start(void *ud)
{
    own_release(ud, SESSION_STOP, (uint64_t)clock_ms);
}

static void test_real_submit_result_survives_release_and_refusal(void)
{
    for (int refused = 0; refused < 2; refused++) {
        hci_t h;
        own_token token;
        own_ledger *l;
        own_ref ref;
        own_state state;
        reset(); h = start(); l = bound(&h, OWN_POLICY_NO_RESTORE);
        ref = prepare(l, 0x1009, NULL, 0, &token);
        if (refused) { usb.fail_request = USB_FS_START; usb.fail_index = CMD; }
        else { usb.start_hook = release_inside_start; usb.start_ud = l; }
        CHECK(hci_usb_submit(&h, &token) == (refused ? OWN_NOT_SENT : OWN_SENT));
        CHECK(usb.commands == !refused);
        usb.start_hook = NULL;
        if (refused) CHECK(own_status(l, ref, &state) == OWN_REFUSED);
        else {
            acknowledge(h, 0x1009, 0, 0);
            CHECK(own_status(l, ref, &state) == OWN_OK && state == OWN_CLOSED);
        }
        CHECK(hci_usb_submit(&h, &token) == OWN_NOT_SENT && usb.commands == !refused);
        closed_once(h); own_free(l);
    }
}

static void test_backend_acknowledgement_conditions(void)
{
    static const unsigned char nop[] = {0x0e, 3, 1, 0, 0};
    static const unsigned char malformed[] = {0x0e, 2, 1, 9};
    for (int mode = 0; mode < 6; mode++) {
        hci_t h;
        own_token token;
        own_ledger *l;
        own_ref ref;
        own_summary s;
        reset(); attrib_reset(); h = start(); l = bound(&h, OWN_POLICY_NO_RESTORE);
        ref = prepare(l, 0x1009, NULL, 0, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        if (mode == 0) {
            receive_event(h, nop, sizeof nop);
            CHECK(attrib_get()->classes[ATTRIB_NOP] == 1);
            acknowledge(h, 0x1009, 0, 0);
        } else if (mode == 1) acknowledge(h, 0x1009, 1, 0); /* wrong event type */
        else if (mode == 2) acknowledge(h, 0x1005, 0, 0); /* foreign opcode */
        else if (mode == 3) {
            clock_ms += 2000; /* Equal to ack_by, not before it. */
            acknowledge(h, 0x1009, 0, 0);
        } else if (mode == 4) receive_event(h, malformed, sizeof malformed);
        else {
            acknowledge(h, 0x1009, 0, 0);
            acknowledge(h, 0x1009, 0, 0); /* duplicate */
        }
        own_snapshot(l, &s);
        CHECK(mode == 0 ? !s.ambiguity_count : s.ambiguity_count > 0);
        CHECK(own_next_hci(l, (uint64_t)clock_ms, &token) != OWN_OK);
        CHECK(usb.commands == 1);
        CHECK(own_reclaim(l, ref) == (mode == 0 || mode == 5 ? OWN_OK : OWN_BUSY));
        closed_once(h); own_free(l);
    }
}

static void test_one_command_lane_and_async_failure(void)
{
    hci_t h;
    own_token token, second;
    own_ledger *l;
    own_summary s;
    reset(); h = start(); l = bound(&h, OWN_POLICY_NO_RESTORE);
    prepare(l, 0x1009, NULL, 0, &token);
    usb.hold_command = 1;
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    CHECK(own_next_hci(l, (uint64_t)clock_ms, &second) == OWN_BUSY);
    acknowledge(h, 0x1009, 0, 0); /* ACK can arrive before the USB completion. */
    prepare(l, 0x1005, NULL, 0, &second);
    CHECK(hci_usb_submit(&h, &second) == OWN_NOT_SENT && usb.commands == 1);
    completion(CMD, NULL, 0);
    usb.command_status = 1;
    CHECK(h.ops->pump(h.ctx, 0) == -1);
    own_snapshot(l, &s);
    CHECK(s.ambiguity_count > 0);
    CHECK(hci_usb_submit(&h, &second) == OWN_NOT_SENT && usb.commands == 1);
    closed_once(h); own_free(l);
}

static void test_owned_restore_is_derived_and_policy_checked(void)
{
    for (int policy = OWN_POLICY_NO_RESTORE; policy <= OWN_POLICY_SINGLE_PRODUCER; policy++) {
        unsigned char scan = 1;
        own_ledger *l;
        own_token token;
        hci_t h;
        reset(); h = start(); l = bound(&h, (own_policy)policy);
        prepare(l, 0x0c1a, &scan, 1, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        acknowledge(h, 0x0c1a, 0, 0);
        fence(h, l, 1);
        CHECK(own_release(l, SESSION_STOP, (uint64_t)clock_ms));
        if (policy == OWN_POLICY_SINGLE_PRODUCER) {
            CHECK(own_next_hci(l, (uint64_t)clock_ms, &token) == OWN_OK);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
            CHECK(usb.commands == 3 && usb.last_opcode == 0x0c1a &&
                  usb.last_plen == 1 && usb.last_params[0] == 2);
            acknowledge(h, 0x0c1a, 0, 0);
        } else CHECK(usb.commands == 2);
        CHECK(own_next_hci(l, (uint64_t)clock_ms, &token) != OWN_OK);
        closed_once(h); own_free(l);
    }
}

static void test_ring_overflow_revokes_a_prepared_restore(void)
{
    unsigned char scan = 1, events[30], out[HCI_PKT_MAX];
    own_ledger *l;
    own_ref ref;
    own_token token;
    own_summary s;
    hci_t h;
    int n = 0;
    reset(); attrib_reset(); h = start(); l = bound(&h, OWN_POLICY_SINGLE_PRODUCER);
    ref = prepare(l, 0x0c1a, &scan, 1, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    acknowledge(h, 0x0c1a, 0, 0);
    fence(h, l, 1);
    CHECK(own_request_undo(l, ref, (uint64_t)clock_ms, &token) == OWN_OK);
    for (int i = 0; i < 10; i++) {
        events[3*i] = 1; events[3*i+1] = 1; events[3*i+2] = 0;
    }
    h.ops->pump(h.ctx, 0);
    for (int i = 0; i < 7; i++) {
        completion(i, events, sizeof events);
        CHECK(h.ops->pump(h.ctx, 0) == 1);
    }
    own_snapshot(l, &s);
    CHECK(attrib_get()->overflow == 6 && s.ambiguity_count > 0);
    CHECK(hci_usb_submit(&h, &token) == OWN_NOT_SENT && usb.commands == 2);
    while (h.ops->next_event(h.ctx, out, sizeof out)) n++;
    CHECK(n == 64);
    closed_once(h); own_free(l);
}

static void test_duplicate_replacement_ack_never_reaches_a_restore(void)
{
    static const unsigned char answers[] = {
        0x0e, 4, 1, 0x1a, 0x0c, 0, /* copied old success */
        0x0e, 4, 1, 0x1a, 0x0c, 0x0c /* replacement's real refusal */
    };
    for (int policy = 0; policy <= 1; policy++) {
        unsigned char scan = 0;
        own_token token;
        own_ledger *l;
        hci_t h;
        reset(); h = start(); l = bound(&h, (own_policy)policy);
        prepare(l, 0x0c1a, &scan, 1, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        acknowledge(h, 0x0c1a, 0, 0); fence(h, l, 1);
        scan = 1;
        prepare(l, 0x0c1a, &scan, 1, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        h.ops->pump(h.ctx, 0);
        completion(0, answers, sizeof answers);
        CHECK(h.ops->pump(h.ctx, 0) == 1);
        CHECK(own_next_hci(l, (uint64_t)clock_ms, &token) != OWN_OK);
        CHECK(usb.commands == 3); /* no fence/undo after contradictory answers */
        closed_once(h); own_free(l);
    }
}

static void test_absolute_cleanup_cutoff_stops_each_completion(void)
{
    own_ledger *l;
    own_summary s;
    hci_t h;
    reset(); h = start(); l = bound(&h, OWN_POLICY_NO_RESTORE);
    CHECK(own_release(l, SESSION_STOP, (uint64_t)clock_ms));
    own_snapshot(l, &s);
    clock_ms = (long)s.runtime.deadline - SESSION_RESERVE_MS - 250;
    for (int i = 0; i < 20; i++) completion(i, NULL, 0);
    usb.completion_cost = 50;
    CHECK(h.ops->pump(h.ctx, 10000) == 0);
    CHECK(usb.count == 15); /* reaper stops at the cutoff, not after its batch */
    CHECK(clock_ms == (long)s.runtime.deadline - SESSION_RESERVE_MS);
    CHECK(h.ops->pump(h.ctx, 10000) == 0 && usb.count == 15);
    for (int i = 0; i < OUT; i++) CHECK(usb.starts[i] == 1);
    closed_once(h); own_free(l);
}

static link_ref acquire_link(hci_t h, own_ledger *l, own_ref *ref)
{
    own_peer peer = {.wire = {1, 2, 3, 4, 5, 6}, .identity = {1, 2, 3, 4, 5, 6}};
    link_ref link;
    own_token token;
    own_command c = {.opcode = 0x0405, .plen = 13, .ack_by = 3000, .terminal_by = 5000};
    own_terminal_event terminal = {.kind = OWN_TERM_CONNECTION, .handle = 0x41};
    CHECK(own_reserve_link(l, &peer, &link) == OWN_OK);
    c.peer = peer; c.link = link;
    memcpy(c.params, peer.wire, 6);
    CHECK(own_prepare(l, &c, ref) == OWN_OK);
    CHECK(own_next_hci(l, (uint64_t)clock_ms, &token) == OWN_OK);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    acknowledge(h, 0x0405, 1, 0);
    terminal.peer = peer;
    own_observe_connection(l, peer.le, peer.address_type, terminal.peer.wire,
                           terminal.handle, terminal.status, (uint64_t)clock_ms);
    fence(h, l, 1);
    return link;
}

static void test_overflow_revokes_all_link_leases(void)
{
    own_ledger *l;
    own_ref ref;
    own_token token;
    unsigned char events[30];
    hci_t h;
    reset(); h = start(); l = bound(&h, OWN_POLICY_SINGLE_PRODUCER);
    link_ref link = acquire_link(h, l, &ref);
    CHECK(own_input(l, link, 0x41, (uint64_t)clock_ms) == OWN_OK);
    CHECK(own_request_undo(l, ref, (uint64_t)clock_ms, &token) == OWN_OK);
    for (int i = 0; i < 10; i++) {
        events[3*i] = 1; events[3*i+1] = 1; events[3*i+2] = 0;
    }
    h.ops->pump(h.ctx, 0);
    for (int i = 0; i < 7; i++) {
        completion(i, events, sizeof events);
        CHECK(h.ops->pump(h.ctx, 0) == 1);
    }
    CHECK(own_input(l, link, 0x41, (uint64_t)clock_ms) != OWN_OK);
    CHECK(hci_usb_submit(&h, &token) == OWN_NOT_SENT && usb.commands == 2);
    CHECK(usb.last_opcode == 0x2018); /* never Disconnect an unleased handle */
    closed_once(h); own_free(l);
}

static own_submit_result acl_submit(hci_t h, own_acl_lane *lane, const own_acl_token *token)
{
    const hci_submission submission = {
        .kind = HCI_SUBMIT_ACL, .token.acl = {.lane = lane, .token = token}
    };
    return h.ops->op_submit(h.ctx, &submission);
}

static void test_acl_token_submission_and_real_results(void)
{
    for (int policy = 0; policy <= 1; policy++) {
        hci_t h;
        own_ref parent;
        own_acl_token token, next;
        unsigned char packet[] = {0x41, 0x20, 1, 0, 0xa5};
        reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        link_ref link = acquire_link(h, l, &parent); /* no accepted input needed for output */
        own_acl_lane *lane = own_acl_new(l);
        CHECK(lane != NULL);
        CHECK(h.ops->op_submit(h.ctx, NULL) == OWN_NOT_SENT);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) == OWN_OK);
        packet[0] = 0x42; packet[4] = 0x5a;
        CHECK(acl_submit(h, lane, &token) == OWN_SENT);
        CHECK(usb.outputs == 1 && usb.last_acl_len == 5 &&
              usb.last_acl[0] == 0x41 && usb.last_acl[4] == 0xa5);
        CHECK(acl_submit(h, lane, &token) == OWN_NOT_SENT && usb.outputs == 1);
        packet[0] = 0x41;
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &next) == OWN_OK);
        long wait_until = clock_ms + 500;
        CHECK(acl_submit(h, lane, &next) == OWN_NOT_SENT && clock_ms == wait_until);
        CHECK(usb.outputs == 1); /* busy hardware is not a second send */
        completion(OUT, NULL, 0);
        CHECK(acl_submit(h, lane, &next) == OWN_SENT && usb.outputs == 2);
        CHECK(usb.last_acl[4] == 0x5a);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) == OWN_OK);
        completion(OUT, NULL, 0);
        usb.fail_request = USB_FS_START; usb.fail_index = OUT;
        CHECK(acl_submit(h, lane, &token) == OWN_NOT_SENT && usb.outputs == 2);
        CHECK(h.ops->pump(h.ctx, 0) == -1);
        closed_once(h); own_acl_free(lane); own_free(l);
    }
}

static void test_acl_callback_latch_refuses_output(void)
{
    for (int busy_output = 0; busy_output < 2; busy_output++) {
        hci_t h;
        own_ref parent;
        own_acl_token token;
        const unsigned char packet[] = {0x41, 0x20, 1, 0, 0xa5};
        reset(); h = start();
        own_ledger *l = own_new(OWN_POLICY_NO_RESTORE, 1);
        release_point point = {l, 0, 0};
        CHECK(l && own_activate(l));
        CHECK(hci_usb_bind_ownership(&h, l, release_during_preparation, &point));
        link_ref link = acquire_link(h, l, &parent);
        own_acl_lane *lane = own_acl_new(l);
        CHECK(lane != NULL);
        if (busy_output) {
            CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) == OWN_OK);
            CHECK(acl_submit(h, lane, &token) == OWN_SENT);
        }
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) == OWN_OK);
        h.ops->pump(h.ctx, 0);
        completion(0, NULL, 0);
        if (busy_output) completion(OUT, NULL, 0);
        point.calls = 0; point.at = busy_output ? 2 : 1;
        CHECK(acl_submit(h, lane, &token) == OWN_NOT_SENT);
        CHECK(point.calls >= point.at && usb.outputs == busy_output);
        CHECK(!h.ops->acl_send(h.ctx, packet, sizeof packet));
        closed_once(h); own_acl_free(lane); own_free(l);
    }
}

static void test_wire_loss_revokes_acl_before_submission(void)
{
    for (int mode = 0; mode < 5; mode++) {
        hci_t h;
        own_ref parent;
        own_acl_token token;
        const unsigned char packet[] = {0x41, 0x20, 0, 0};
        unsigned char event[] = {5, 4, 0, 0x41, 0, 0x13};
        unsigned char overflow[70 * 3];
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, OWN_POLICY_SINGLE_PRODUCER);
        link_ref link = acquire_link(h, l, &parent);
        own_acl_lane *lane = own_acl_new(l);
        CHECK(lane != NULL);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) == OWN_OK);
        h.ops->pump(h.ctx, 0);
        if (mode == 1) event[3] = 0x42; /* another link's disconnect cannot steal this lease */
        if (mode == 2) { event[1] = 2; completion(0, event, 4); }
        else if (mode == 3) {
            for (int i = 0; i < 70; i++) {
                overflow[3*i] = 1; overflow[3*i+1] = 1; overflow[3*i+2] = 0;
            }
            completion(0, overflow, sizeof overflow);
        } else if (mode == 4) {
            static const unsigned char foreign[] = {0x0e, 4, 1, 1, 0x10, 0};
            completion(0, foreign, sizeof foreign);
        } else completion(0, event, sizeof event);
        CHECK(acl_submit(h, lane, &token) == (mode == 1 ? OWN_SENT : OWN_NOT_SENT));
        CHECK(usb.outputs == (mode == 1));
        if (mode == 3) CHECK(attrib_get()->overflow == 6);
        closed_once(h); own_acl_free(lane); own_free(l);
    }
}

static void test_acl_release_inside_start_and_async_failure(void)
{
    for (int asynchronous_failure = 0; asynchronous_failure < 2; asynchronous_failure++) {
        hci_t h;
        own_ref parent;
        own_acl_token token;
        const unsigned char packet[] = {0x41, 0x20, 0, 0};
        reset(); h = start();
        own_ledger *l = bound(&h, OWN_POLICY_NO_RESTORE);
        link_ref link = acquire_link(h, l, &parent);
        own_acl_lane *lane = own_acl_new(l);
        CHECK(lane != NULL);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) == OWN_OK);
        if (!asynchronous_failure) { usb.start_hook = release_inside_start; usb.start_ud = l; }
        CHECK(acl_submit(h, lane, &token) == OWN_SENT && usb.outputs == 1);
        usb.start_hook = NULL;
        if (asynchronous_failure) {
            completion(OUT, NULL, 0); usb.output_status = 1;
            CHECK(h.ops->pump(h.ctx, 0) == -1);
        }
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) != OWN_OK);
        closed_once(h);
        CHECK(usb.stops[OUT] == !asynchronous_failure);
        own_acl_free(lane); own_free(l);
    }
}

static void test_hci_phase_deadline_reaches_attribution(void)
{
    hci_t h;
    own_token token;
    own_summary summary;
    reset(); attrib_reset(); h = start();
    own_ledger *l = bound(&h, OWN_POLICY_NO_RESTORE);
    prepare(l, 0x1009, NULL, 0, &token); /* ack_by=3000, not attrib's default 2000 */
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    clock_ms = 2500;
    acknowledge(h, 0x1009, 0, 0);
    own_snapshot(l, &summary);
    CHECK(attrib_get()->timeout == 0 && attrib_get()->classes[ATTRIB_SOLICITED] == 1);
    CHECK(summary.ambiguity_count == 0);
    closed_once(h); own_free(l);
}

static void test_derived_disconnect_requires_a_live_fresh_lease(void)
{
    static const unsigned char disconnected[] = {5, 4, 0, 0x41, 0, 0x13};
    for (int policy = 0; policy <= 1; policy++) for (int mode = 0; mode < 3; mode++) {
        hci_t h;
        own_ref parent;
        own_token token;
        reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        link_ref link = acquire_link(h, l, &parent);
        CHECK(own_input(l, link, 0x41, (uint64_t)clock_ms) == OWN_OK);
        h.ops->pump(h.ctx, 0);
        if (mode == 2) clock_ms += OWN_FRESH_MS + 1;
        CHECK(own_release(l, SESSION_STOP, (uint64_t)clock_ms));
        if (policy == OWN_POLICY_SINGLE_PRODUCER && mode != 2) {
            CHECK(own_request_undo(l, parent, (uint64_t)clock_ms, &token) == OWN_OK);
            if (mode == 1) completion(0, disconnected, sizeof disconnected);
            CHECK(hci_usb_submit(&h, &token) == (mode == 1 ? OWN_NOT_SENT : OWN_SENT));
            if (!mode) {
                CHECK(usb.commands == 3 && usb.last_opcode == 0x0406 &&
                      usb.last_plen == 3 && usb.last_params[0] == 0x41 &&
                      usb.last_params[1] == 0 && usb.last_params[2] == 0x13);
                acknowledge(h, 0x0406, 1, 0);
            } else CHECK(usb.commands == 2);
        } else {
            CHECK(own_request_undo(l, parent, (uint64_t)clock_ms, &token) != OWN_OK);
            CHECK(usb.commands == 2);
        }
        closed_once(h); own_free(l);
    }
}

static void test_queued_connection_cannot_remint_a_disconnected_lease(void)
{
    unsigned violations = 0;
    static const unsigned char classic[] = {3,11,0,0x41,0,1,2,3,4,5,6,1,0};
    static const unsigned char le[] = {
        0x3e,19,1,0,0x41,0,0,0,1,2,3,4,5,6,24,0,0,0,72,0,0
    };
    static const unsigned char enhanced[] = {
        0x3e,31,0x0a,0,0x41,0,0,0,1,2,3,4,5,6,
        0,0,0,0,0,0,0,0,0,0,0,0,24,0,0,0,72,0,0
    };
    static const unsigned char disconnected[] = {5,4,0,0x41,0,0x13};
    static const unsigned char rand[] = {0x0e,12,1,0x18,0x20,0,1,0,0,0,0,0,0,0};
    for (unsigned policy = 0; policy < 2; policy++)
        for (unsigned transport = 0; transport < 3; transport++)
            for (unsigned lost = 0; lost < 2; lost++) {
                const unsigned char *connected = transport == 0 ? classic :
                                                  transport == 1 ? le : enhanced;
                size_t connected_len = transport == 0 ? sizeof classic :
                                       transport == 1 ? sizeof le : sizeof enhanced;
                hci_t h;
                own_ref parent;
                own_token command;
                link_ref link;
                unsigned char wire[64], popped[HCI_PKT_MAX];
                size_t n = 0;
                reset(); attrib_reset(); h = start();
                own_ledger *l = bound(&h, (own_policy)policy);
                own_peer peer = {.le = transport != 0, .wire = {1,2,3,4,5,6},
                                 .identity = {1,2,3,4,5,6}};
                own_command c = {.opcode = transport ? 0x200d : 0x0405,
                                 .plen = transport ? 25 : 13,
                                 .ack_by = 3000, .terminal_by = 5000, .peer = peer};
                CHECK(own_reserve_link(l, &peer, &link) == OWN_OK);
                c.link = link;
                memcpy(c.params + (transport ? 6 : 0), peer.wire, 6);
                CHECK(own_prepare(l, &c, &parent) == OWN_OK);
                CHECK(own_next_hci(l, (uint64_t)clock_ms, &command) == OWN_OK);
                CHECK(hci_usb_submit(&h, &command) == OWN_SENT);
                acknowledge(h, c.opcode, 1, 0);
                CHECK(own_next_hci(l, (uint64_t)clock_ms, &command) == OWN_OK);
                CHECK(hci_usb_submit(&h, &command) == OWN_SENT && usb.last_opcode == 0x2018);
                memcpy(wire + n, connected, connected_len); n += connected_len;
                if (lost) {
                    memcpy(wire + n, disconnected, sizeof disconnected);
                    n += sizeof disconnected;
                }
                memcpy(wire + n, rand, sizeof rand); n += sizeof rand;
                h.ops->pump(h.ctx, 0);
                CHECK(usb.busy[0]);
                completion(0, wire, (int)n);
                CHECK(h.ops->pump(h.ctx, 0) == 1);
                CHECK(h.ops->next_event(h.ctx, popped, sizeof popped) == (int)connected_len);
                CHECK(!memcmp(popped, connected, connected_len));
                own_terminal_event terminal = {
                    .kind = OWN_TERM_CONNECTION, .peer = peer, .handle = 0x41
                };
                own_terminal(l, parent, &terminal, (uint64_t)clock_ms);
                own_acl_lane *lane = own_acl_new(l);
                own_acl_token token;
                const unsigned char packet[] = {0x41,0x20,0,0};
                CHECK(lane != NULL);
                own_result prepared = own_acl_prepare(lane, link, packet, sizeof packet,
                                                     (uint64_t)clock_ms, &token);
                own_submit_result sent = prepared == OWN_OK ? acl_submit(h, lane, &token) : OWN_NOT_SENT;
                printf("QUEUED_CONNECTION policy=%u transport=%u disconnected=%u prepare=%d sent=%d outputs=%d\n",
                       policy, transport, lost, prepared, sent, usb.outputs);
                if (!lost) CHECK(sent == OWN_SENT && usb.outputs == 1);
                else {
                    CHECK(h.ops->next_event(h.ctx, popped, sizeof popped) == sizeof disconnected);
                    CHECK(!memcmp(popped, disconnected, sizeof disconnected));
                    violations += sent != OWN_NOT_SENT || usb.outputs != 0;
                }
                closed_once(h); own_acl_free(lane); own_free(l);
            }
    CHECK(violations == 0);
}

static unsigned classified_answers(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ATTRIB_CLASS_COUNT; i++)
        n += (unsigned)attrib_get()->classes[i];
    return n;
}

static void test_actual_receipt_provenance_reaches_classifier(void)
{
    unsigned violations = 0;
    {
        static const unsigned char nop[] = {0x0e,3,1,0,0};
        hci_t h;
        own_token token;
        own_summary summary;
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, OWN_POLICY_NO_RESTORE);
        prepare(l, 0x1009, NULL, 0, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        receive_event(h, nop, sizeof nop);
        CHECK(test_usb_receipts.last == OWN_RECEIPT_NONE && test_usb_receipts.calls == 1);
        CHECK(attrib_get()->classes[ATTRIB_NOP] == 1 && attrib_get()->unanswered == 1);
        acknowledge(h, 0x1009, 0, 0);
        CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED && test_usb_receipts.calls == 2);
        CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 1 && attrib_get()->unanswered == 0);
        CHECK(classified_answers() == 2);
        own_snapshot(l, &summary);
        CHECK(summary.ambiguity_count == 0);
        closed_once(h); own_free(l);
    }
    for (unsigned mode = 0; mode < 2; mode++) {
        hci_t h;
        own_token token;
        own_summary summary;
        unsigned char noise[70 * 3], out[HCI_PKT_MAX];
        static const unsigned char answer[] = {0x0e,4,1,9,0x10,0};
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, OWN_POLICY_NO_RESTORE);
        prepare(l, 0x1009, NULL, 0, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        if (!mode) {
            for (unsigned i = 0; i < 70; i++) {
                noise[3*i] = 1; noise[3*i+1] = 1; noise[3*i+2] = 0;
            }
            h.ops->pump(h.ctx, 0);
            completion(0, noise, sizeof noise);
            CHECK(h.ops->pump(h.ctx, 0) == 1 && attrib_get()->overflow == 6);
            while (h.ops->next_event(h.ctx, out, sizeof out)) {}
        } else acknowledge(h, 0x1009, 1, 0);
        own_snapshot(l, &summary);
        CHECK(summary.ambiguity_count > 0);
        unsigned before = classified_answers(), receipts = test_usb_receipts.calls;
        CHECK(usb.busy[1]); /* The reply arrives on a genuinely retained read. */
        completion(1, answer, sizeof answer);
        CHECK(h.ops->pump(h.ctx, 0) == 1);
        CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof answer);
        CHECK(!memcmp(out, answer, sizeof answer));
        CHECK(test_usb_receipts.last == OWN_RECEIPT_NONE && test_usb_receipts.calls == receipts + 1);
        CHECK(classified_answers() == before + 1);
        printf("NONE_CLASSIFIER mode=%u provenance=%d solicited=%lu unanswered=%lu ambiguities=%u\n",
               mode, test_usb_receipts.last, attrib_get()->classes[ATTRIB_SOLICITED],
               attrib_get()->unanswered, summary.ambiguity_count);
        violations += attrib_get()->classes[ATTRIB_SOLICITED] != 0 ||
                      attrib_get()->unanswered != 1;
        closed_once(h); own_free(l);
    }
    CHECK(violations == 0);
}

static void test_credit_only_events_leave_command_lane_untouched(void)
{
    unsigned violations = 0;
    for (unsigned policy = 0; policy < 2; policy++) for (unsigned waiting = 0; waiting < 3; waiting++) {
        hci_t h;
        own_token token, next;
        own_ref parent = {0};
        own_summary summary;
        unsigned updates = 0;
        int failed = 0;
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        if (waiting) {
            static const unsigned char inquiry[] = {0x33,0x8b,0x9e,1,0};
            parent = prepare(l, waiting == 1 ? 0x1009 : 0x0401,
                             waiting == 1 ? NULL : inquiry, waiting == 1 ? 0 : 5, &token);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        }
        /* Exercise every legal one-octet credit value, including zero. */
        for (unsigned value = 0; value < 256 && !failed; value++) {
            unsigned char cc[] = {0x0e,3,(unsigned char)(value + 1),0,0};
            unsigned char cs[] = {0x0f,4,0,(unsigned char)(value + 1),0,0};
            for (unsigned form = 0; form < 2; form++) {
                receive_event(h, form ? cs : cc, form ? sizeof cs : sizeof cc);
                updates++;
                own_snapshot(l, &summary);
                CHECK(test_usb_receipts.last == OWN_RECEIPT_NONE);
                CHECK(attrib_get()->classes[ATTRIB_NOP] == updates);
                CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 0);
                CHECK(attrib_get()->unanswered == (waiting != 0) && usb.commands == (waiting != 0));
                own_result lane = own_next_hci(l, (uint64_t)clock_ms, &next);
                if (summary.credit_nops != updates || summary.ambiguity_count ||
                    summary.runtime.reason != SESSION_NONE ||
                    lane != (waiting ? OWN_BUSY : OWN_STOPPED)) {
                    failed = 1;
                    break;
                }
            }
        }
        printf("CREDIT_NOP policy=%u waiter=%u updates=%u credits=%u ambiguities=%u reason=%d\n",
               policy, waiting, updates, summary.credit_nops, summary.ambiguity_count,
               summary.runtime.reason);
        violations += failed;
        if (!failed && waiting) {
            acknowledge(h, waiting == 1 ? 0x1009 : 0x0401, waiting != 1, 0);
            CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED);
            CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 1 && attrib_get()->unanswered == 0);
            if (waiting == 2) {
                own_terminal_event terminal = {.kind = OWN_TERM_INQUIRY};
                own_terminal(l, parent, &terminal, (uint64_t)clock_ms);
            }
        }
        closed_once(h); own_free(l);
    }
    CHECK(violations == 0);
}

static void test_unassociated_events_do_not_answer_commands(void)
{
    static const unsigned char packets[][11] = {
        {0x13,5,1,0x41,0,3,0},                  /* completed ACL packets */
        {0x48,9,4,0,1,0x41,0,2,0,3,0},         /* completed data blocks */
        {0xff,6,0x0f,4,0,1,0,0},               /* opaque vendor payload */
        {0xfe,1,0},                             /* unsupported event */
        {0x3e,1,0xff},                          /* unsupported LE subevent */
        {5,4,1,0x41,0,0xff}                    /* failure: reason ignored */
    };
    for (unsigned policy = 0; policy < 2; policy++) {
        hci_t h;
        own_token token, next;
        own_summary summary;
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        prepare(l, 0x1009, NULL, 0, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        for (unsigned i = 0; i < sizeof packets / sizeof packets[0]; i++) {
            unsigned before = test_usb_receipts.calls;
            receive_event(h, packets[i], 2 + packets[i][1]);
            own_snapshot(l, &summary);
            CHECK(test_usb_receipts.calls == before && summary.ambiguity_count == 0);
            CHECK(own_next_hci(l, (uint64_t)clock_ms, &next) == OWN_BUSY);
            CHECK(attrib_get()->unanswered == 1 && attrib_get()->classes[ATTRIB_SOLICITED] == 0);
        }
        acknowledge(h, 0x1009, 0, 0);
        CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED && attrib_get()->unanswered == 0);
        closed_once(h); own_free(l);
    }
}

static void test_credit_reassembly_and_real_response(void)
{
    static const unsigned char cs[] = {0x0f,4,0,1,0,0};
    static const unsigned char cc[] = {0x0e,3,0,0,0};
    static const unsigned char extra[] = {0x0e,5,255,0,0,0xff,0x7e};
    static const unsigned char answer[] = {0x0e,4,1,9,0x10,0};
    for (unsigned policy = 0; policy < 2; policy++) {
        hci_t h;
        own_token token, next;
        own_summary summary;
        unsigned char out[HCI_PKT_MAX], batch[sizeof cc + sizeof extra + sizeof answer];
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        prepare(l, 0x1009, NULL, 0, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        h.ops->pump(h.ctx, 0); completion(0, cs, 3);
        CHECK(h.ops->pump(h.ctx, 0) == 0 && test_usb_receipts.calls == 0);
        h.ops->pump(h.ctx, 0); completion(0, cs + 3, 3);
        CHECK(h.ops->pump(h.ctx, 0) == 1);
        CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof cs && !memcmp(out, cs, sizeof cs));
        CHECK(test_usb_receipts.last == OWN_RECEIPT_NONE && test_usb_receipts.calls == 1);
        CHECK(own_next_hci(l, (uint64_t)clock_ms, &next) == OWN_BUSY);
        memcpy(batch, cc, sizeof cc);
        memcpy(batch + sizeof cc, extra, sizeof extra);
        memcpy(batch + sizeof cc + sizeof extra, answer, sizeof answer);
        h.ops->pump(h.ctx, 0); completion(0, batch, sizeof batch);
        CHECK(h.ops->pump(h.ctx, 0) == 1 && test_usb_receipts.calls == 4);
        CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof cc && !memcmp(out, cc, sizeof cc));
        CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof extra && !memcmp(out, extra, sizeof extra));
        CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof answer && !memcmp(out, answer, sizeof answer));
        own_snapshot(l, &summary);
        CHECK(summary.credit_nops == 3 && summary.ambiguity_count == 0);
        CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED);
        CHECK(attrib_get()->classes[ATTRIB_NOP] == 3 &&
              attrib_get()->classes[ATTRIB_SOLICITED] == 1 && attrib_get()->unanswered == 0);
        closed_once(h); own_free(l);
    }
}

static void test_credits_cannot_hide_real_ambiguity_or_timeout(void)
{
    static const unsigned char bad[][6] = {
        {0x0e,4,1,5,0x10,0},       /* unsolicited opcode */
        {0x0f,4,0,1,9,0x10},       /* wrong event type */
        {0x0f,4,0x0c,1,0,0},       /* opcode-zero Status must be success */
        {0x0e,2,1,0,0,0}           /* truncated opcode */
    };
    static const unsigned char cs[] = {0x0f,4,0,1,0,0};
    for (unsigned policy = 0; policy < 2; policy++) {
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            hci_t h;
            own_token token;
            own_summary summary;
            reset(); attrib_reset(); h = start();
            own_ledger *l = bound(&h, (own_policy)policy);
            prepare(l, 0x1009, NULL, 0, &token);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
            receive_event(h, bad[i], i == 3 ? 4 : 6);
            own_snapshot(l, &summary);
            CHECK(summary.runtime.reason == SESSION_AMBIGUOUS && summary.ambiguity_count > 0);
            CHECK(summary.credit_nops == 0 && attrib_get()->classes[ATTRIB_SOLICITED] == 0);
            CHECK(attrib_get()->classes[ATTRIB_NOP] == 0 && attrib_get()->unanswered == 1);
            if (!i) {
                unsigned before = summary.ambiguity_count;
                unsigned char out[16];
                CHECK(usb.busy[1]);
                completion(1, cs, sizeof cs);
                CHECK(h.ops->pump(h.ctx, 0) == 1);
                CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof cs);
                own_snapshot(l, &summary);
                CHECK(summary.runtime.reason == SESSION_AMBIGUOUS &&
                      summary.ambiguity_count == before && summary.credit_nops == 1);
            }
            closed_once(h); own_free(l);
        }
        hci_t h;
        own_token token;
        own_summary summary;
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        prepare(l, 0x1009, NULL, 0, &token);
        CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        clock_ms = 2999;
        receive_event(h, cs, sizeof cs);
        own_snapshot(l, &summary);
        CHECK(summary.runtime.reason == SESSION_NONE && summary.credit_nops == 1);
        clock_ms = 3000;
        h.ops->pump(h.ctx, 0);
        own_snapshot(l, &summary);
        CHECK(summary.runtime.reason == SESSION_AMBIGUOUS && summary.ambiguity_count > 0);
        CHECK(attrib_get()->unanswered == 1 && attrib_get()->timeout == 1 && usb.commands == 1);
        closed_once(h); own_free(l);
    }
}

static void test_failed_le_event_ignored_fields_cannot_revoke_a_link(void)
{
    unsigned violations = 0;
    static const unsigned char connected[] = {
        0x3e,19,1,0,0x41,0,0,0,1,2,3,4,5,6,24,0,0,0,72,0,0
    };
    static const unsigned char rand[] = {0x0e,12,1,0x18,0x20,0,1,0,0,0,0,0,0,0};
    for (unsigned policy = 0; policy < 2; policy++) for (unsigned enhanced = 0; enhanced < 2; enhanced++) {
        hci_t h;
        own_ref parent;
        link_ref link;
        own_token command;
        own_acl_token output;
        own_summary summary;
        unsigned char wire[sizeof connected + sizeof rand], popped[HCI_PKT_MAX], failure[33];
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        own_peer peer = {.le = 1, .wire = {1,2,3,4,5,6}, .identity = {1,2,3,4,5,6}};
        own_command c = {.opcode = 0x200d, .plen = 25, .ack_by = 3000,
                         .terminal_by = 5000, .peer = peer};
        CHECK(own_reserve_link(l, &peer, &link) == OWN_OK);
        c.link = link; memcpy(c.params + 6, peer.wire, 6);
        CHECK(own_prepare(l, &c, &parent) == OWN_OK);
        CHECK(own_next_hci(l, (uint64_t)clock_ms, &command) == OWN_OK);
        CHECK(hci_usb_submit(&h, &command) == OWN_SENT);
        acknowledge(h, 0x200d, 1, 0);
        CHECK(own_next_hci(l, (uint64_t)clock_ms, &command) == OWN_OK);
        CHECK(hci_usb_submit(&h, &command) == OWN_SENT);
        memcpy(wire, connected, sizeof connected);
        memcpy(wire + sizeof connected, rand, sizeof rand);
        h.ops->pump(h.ctx, 0); completion(0, wire, sizeof wire);
        CHECK(h.ops->pump(h.ctx, 0) == 1);
        CHECK(h.ops->next_event(h.ctx, popped, sizeof popped) == sizeof connected);
        CHECK(!memcmp(popped, connected, sizeof connected));
        CHECK(h.ops->next_event(h.ctx, popped, sizeof popped) == sizeof rand);
        own_acl_lane *lane = own_acl_new(l);
        static const unsigned char packet[] = {0x41,0x20,0,0};
        CHECK(lane != NULL);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &output) == OWN_OK);
        memset(failure, 0xff, sizeof failure);
        failure[0] = 0x3e; failure[1] = enhanced ? 31 : 19;
        failure[2] = enhanced ? 0x0a : 1; failure[3] = 0x0c;
        /* Only status/subevent are valid on failure; these bytes may be arbitrary. */
        failure[7] = 0; memcpy(failure + 8, peer.wire, 6);
        receive_event(h, failure, enhanced ? 33 : 21);
        own_snapshot(l, &summary);
        own_submit_result sent = acl_submit(h, lane, &output);
        printf("FAILED_LE policy=%u enhanced=%u ambiguities=%u sent=%d outputs=%d\n",
               policy, enhanced, summary.ambiguity_count, sent, usb.outputs);
        violations += summary.ambiguity_count != 0 || sent != OWN_SENT || usb.outputs != 1;
        closed_once(h); own_acl_free(lane); own_free(l);
    }
    CHECK(violations == 0);
}

static void test_failed_classic_attempt_does_not_end_an_existing_link(void)
{
    unsigned violations = 0;
    for (unsigned policy = 0; policy < 2; policy++) for (unsigned ignored = 0; ignored < 2; ignored++) {
        hci_t h;
        own_ref parent;
        own_acl_token token;
        own_summary summary;
        unsigned char failure[] = {3,11,0x0c,0xff,0xff,1,2,3,4,5,6,1,0xff};
        static const unsigned char packet[] = {0x41,0x20,0,0};
        reset(); attrib_reset(); h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        link_ref link = acquire_link(h, l, &parent);
        own_acl_lane *lane = own_acl_new(l);
        CHECK(lane != NULL);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &token) == OWN_OK);
        if (ignored) failure[11] = 0xff;
        receive_event(h, failure, sizeof failure);
        own_snapshot(l, &summary);
        own_submit_result sent = acl_submit(h, lane, &token);
        printf("FAILED_CLASSIC policy=%u ignored_type=%u ambiguities=%u sent=%d outputs=%d\n",
               policy, ignored, summary.ambiguity_count, sent, usb.outputs);
        violations += summary.ambiguity_count != 0 || sent != OWN_SENT || usb.outputs != 1;
        closed_once(h); own_acl_free(lane); own_free(l);
    }
    CHECK(violations == 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int acl_boundary = argc == 2 && !strcmp(argv[1], "--acl-boundary");
    int queued_connection = argc == 2 && !strcmp(argv[1], "--queued-connection");
    int receipt_classifier = argc == 2 && !strcmp(argv[1], "--receipt-classifier");
    int credit_nop = argc == 2 && !strcmp(argv[1], "--credit-nop");
    int event_sweep = argc == 2 && !strcmp(argv[1], "--event-sweep");
    strict_mode = argc == 2 && !strcmp(argv[1], "--expected-failures");
    if (argc > 2 || (argc == 2 && !strict_mode && !acl_boundary &&
                    !queued_connection && !receipt_classifier && !credit_nop && !event_sweep)) return 2;
    if (event_sweep) {
        test_unassociated_events_do_not_answer_commands();
        test_failed_le_event_ignored_fields_cannot_revoke_a_link();
        test_failed_classic_attempt_does_not_end_an_existing_link();
        goto done;
    }
    if (credit_nop) {
        test_credit_only_events_leave_command_lane_untouched();
        test_credit_reassembly_and_real_response();
        test_credits_cannot_hide_real_ambiguity_or_timeout();
        goto done;
    }
    if (queued_connection) {
        test_queued_connection_cannot_remint_a_disconnected_lease();
        goto done;
    }
    if (receipt_classifier) {
        test_actual_receipt_provenance_reaches_classifier();
        goto done;
    }
    if (acl_boundary) {
        test_acl_callback_latch_refuses_output();
        test_wire_loss_revokes_acl_before_submission();
        goto done;
    }
    test_close_busy_reads();
    test_expected_current_yield_is_not_close();
    test_late_response_and_missing_stop_completion();
    test_ioctl_failures();
    test_busy_acl_out();
    test_event_read_operations();
    test_empty_completion_stream_observes_deadline();
    test_refused_usb_initialization_has_no_uninit_authority();
    test_completion_churn_has_a_finite_batch();
    test_refused_transfer_opens_never_authorize_stops();
    test_node_and_alternate_output_ownership();
    test_tokens_are_exclusive_single_use_and_immutable();
    test_release_inside_read_reap_refuses_raw_submit();
    test_real_submit_result_survives_release_and_refusal();
    test_backend_acknowledgement_conditions();
    test_one_command_lane_and_async_failure();
    test_owned_restore_is_derived_and_policy_checked();
    test_ring_overflow_revokes_a_prepared_restore();
    test_duplicate_replacement_ack_never_reaches_a_restore();
    test_absolute_cleanup_cutoff_stops_each_completion();
    test_overflow_revokes_all_link_leases();
    test_acl_token_submission_and_real_results();
    test_acl_callback_latch_refuses_output();
    test_wire_loss_revokes_acl_before_submission();
    test_acl_release_inside_start_and_async_failure();
    test_hci_phase_deadline_reaches_attribution();
    test_derived_disconnect_requires_a_live_fresh_lease();
    test_queued_connection_cannot_remint_a_disconnected_lease();
    test_actual_receipt_provenance_reaches_classifier();
    test_credit_only_events_leave_command_lane_untouched();
    test_credit_reassembly_and_real_response();
    test_credits_cannot_hide_real_ambiguity_or_timeout();
    test_unassociated_events_do_not_answer_commands();
    test_failed_le_event_ignored_fields_cannot_revoke_a_link();
    test_failed_classic_attempt_does_not_end_an_existing_link();
done:
    ph_clock_hook = NULL;
    printf("%d USB lifetime checks, %d failed; %d expected-failing cases\n", checks, failures, pending);
    return failures != 0 || (strict_mode && pending != 0);
}
