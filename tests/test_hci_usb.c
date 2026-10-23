/* Real hci_usb operations; only the OS boundary and clock are stood in for. */
#include "../src/hci_usb.h"
#include "../src/util.h"
#include "usb_standin/dev/usb/usb_ioctl.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { EVENTS = 40, ACL = 20, OUT = 60, XFERS = 62, FD = 73 };
static int failures, checks, pending, strict_mode;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void expected_failure(int todo, const char *name, int desired, const char *effect)
{
    checks++;
    printf("%s expected-failing-until-todo-%d %s\n", desired ? "XPASS" : "XFAIL", todo, name);
    if (!desired) {
        pending++;
        if (strict_mode) printf("FAIL %s: %s (todo-%d)\n", name, effect, todo);
    }
}
#define EFFECT(todo, name, c) expected_failure(todo, name, (c), #c)

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
    int reject_node, node_attempts, alternate_out;
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
        if (!usb.count) { errno = EAGAIN; return -1; }
        index = usb.done[usb.head].index;
        n = usb.done[usb.head].len;
        d->ep_index = (uint8_t)index;
        usb.ep[index].pLength[0] = (uint32_t)n;
        usb.ep[index].aFrames = n > 0;
        usb.ep[index].status = n ? 0 : 1;
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
    usb.fail_request = USB_DO_REQUEST;
    CHECK(!h.ops->cmd(h.ctx, 0x1009, NULL, 0));
    CHECK(h.ops->pump(h.ctx, 0) == -1);
    closed_once(h);
    transcript("command-EIO-marks-transport-dead");

    reset();
    usb.fail_request = USB_FS_INIT;
    CHECK(!hci_usb_open(&h));
    CHECK(usb.uninits == 1 && usb.closes == 1 && !usb.fd_open);
    transcript("expected-current-init-failure-attempts-UNINIT");

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
#if WHATEVERPAD_PROBE
    CHECK(clock_ms <= deadline && usb.empty_stream);
#else
    EFFECT(9, "empty-completion-stream-deadline", clock_ms <= deadline && usb.empty_stream);
#endif
    usb.empty_stream = 0;
    closed_once(h);
}

static void test_refused_usb_initialization_has_no_uninit_authority(void)
{
    hci_t h;
    reset(); usb.fail_request = USB_FS_INIT;
    CHECK(!hci_usb_open(&h));
    CHECK(usb.inits == 0 && usb.closes == 1);
    EFFECT(9, "refused-USB-init-no-uninit", usb.uninits == 0);
}

static void test_refused_transfer_opens_never_authorize_stops(void)
{
    static const int indices[] = {OUT, 0, EVENTS};
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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    strict_mode = argc == 2 && !strcmp(argv[1], "--expected-failures");
    if (argc > 2 || (argc == 2 && !strict_mode)) return 2;
    test_close_busy_reads();
    test_expected_current_yield_is_not_close();
    test_late_response_and_missing_stop_completion();
    test_ioctl_failures();
    test_busy_acl_out();
    test_event_read_operations();
    test_empty_completion_stream_observes_deadline();
    test_refused_usb_initialization_has_no_uninit_authority();
    test_refused_transfer_opens_never_authorize_stops();
    test_node_and_alternate_output_ownership();
    ph_clock_hook = NULL;
    printf("%d USB lifetime checks, %d failed; %d expected-failing cases\n", checks, failures, pending);
    return failures != 0 || (strict_mode && pending != 0);
}
