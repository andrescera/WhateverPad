/* Reuse the real USB syscall stand-in without editing its behavior of record. */
#define main usb_lifetime_main
#define test_usb_ioctl usb_fixture_ioctl
#define test_usb_usleep usb_fixture_usleep
#include "test_hci_usb.c"
#undef test_usb_usleep
#undef test_usb_ioctl
#undef main
#include "../src/attrib.h"
#include "../src/probe.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include "sim.h"
#include <stdlib.h>

static char json[ATTRIB_JSON_MAX], logs[130000];
static const unsigned char secret[] = "ZQ_SECRET_73";
static int freeze_usb_clock, frozen_waits;

int test_usb_usleep(useconds_t us)
{
    /* Watchdog permits a failing implementation to finish in virtual time. */
    if (freeze_usb_clock && ++frozen_waits <= 128) return 0;
    return usb_fixture_usleep(us);
}

int test_usb_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    /* The inherited stand-in deliberately has no diagnostic-only ioctls. */
    if (request == USB_GET_DEVICEINFO || request == USB_GET_IFACE_DRIVER) {
        errno = ENOTTY;
        return -1;
    }
    return usb_fixture_ioctl(fd, request, arg);
}

static enum attrib_class answer(unsigned op, int status_event, long at)
{
    unsigned char ev[32] = {0};
    ev[0] = status_event ? 0x0f : 0x0e;
    ev[1] = 4 + sizeof secret;
    ev[status_event ? 2 : 5] = 0x0c;
    ev[status_event ? 3 : 2] = 7;
    put16(ev + (status_event ? 4 : 3), op);
    memcpy(ev + 6, secret, sizeof secret);
    return attrib_observe(ev, 6 + sizeof secret, at);
}

static void metadata_only(void)
{
    CHECK(attrib_json(json, sizeof json) > 0);
    CHECK(strstr(json, (const char *)secret) == NULL);
    CHECK(strstr(json, "5a 51 5f 53 45 43 52 45 54 5f 37 33") == NULL);
    if (getenv("ATTRIB_DUMP_JSON")) printf("ATTRIB_JSON %s\n", json);
}

static void test_classes(void)
{
    unsigned char short_cc[] = {0x0e, 4, 7, 5, 0x10};
    unsigned char short_cs[] = {0x0f, 4, 0, 7, 1};
    unsigned long from = log_seq();
    char version[40];
    char summary_path[] = "build/attrib-summary-XXXXXX";
    int summary_fd;
    FILE *summary;
    attrib_reset();
    attrib_submit(0x1005, 1, 100);
    CHECK(answer(0x1005, 0, 101) == ATTRIB_SOLICITED);
    CHECK(answer(0x1005, 0, 102) == ATTRIB_DUPLICATE);
    attrib_submit(0x0405, 1, 103);
    CHECK(answer(0x0405, 0, 104) == ATTRIB_UNMATCHED); /* wrong phase */
    CHECK(answer(0x0405, 1, 105) == ATTRIB_SOLICITED);
    CHECK(answer(0x0405, 1, 106) == ATTRIB_DUPLICATE);
    CHECK(answer(0, 0, 107) == ATTRIB_NOP);
    CHECK(answer(0, 1, 108) == ATTRIB_NOP);
    CHECK(answer(0x1234, 0, 109) == ATTRIB_UNMATCHED);
    CHECK(attrib_observe(short_cc, sizeof short_cc, 110) == ATTRIB_MALFORMED);
    CHECK(attrib_observe(short_cs, sizeof short_cs, 110) == ATTRIB_MALFORMED);
    CHECK(attrib_observe(short_cc, 1, 110) == ATTRIB_MALFORMED);
    attrib_submit(0x1009, 1, 111);
    attrib_deadline(0x1009, 120);
    attrib_poll(120);
    CHECK(attrib_get()->timeout == 1 && attrib_get()->unanswered == 1);
    attrib_poll(121);
    CHECK(attrib_get()->timeout == 1);
    CHECK(answer(0x1009, 0, 121) == ATTRIB_LATE);
    CHECK(attrib_get()->unanswered == 0);
    CHECK(answer(0x1009, 0, 122) == ATTRIB_DUPLICATE);
    attrib_submit(0x1009, 0, 123); /* failed transport is not a submission */
    CHECK(attrib_get()->submitted == 3);
    CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 2);
    CHECK(attrib_get()->classes[ATTRIB_DUPLICATE] == 3);
    CHECK(attrib_get()->classes[ATTRIB_MALFORMED] == 3);
    CHECK(attrib_get()->classes[ATTRIB_NOP] == 2);
    metadata_only();
    CHECK(strstr(json, "\"unanswered\":0") != NULL);
    CHECK(strstr(json, "\"timeout\":1") != NULL);
    snprintf(version, sizeof version, "\"rules_version\":%d", ATTRIB_RULES_VERSION);
    CHECK(strstr(json, version) != NULL);
    CHECK(strstr(json, "\"status\":12") != NULL);
    CHECK(strstr(json, "\"credits\":7") != NULL);
    summary_fd = mkstemp(summary_path);
    CHECK(summary_fd >= 0);
    if (summary_fd >= 0) close(summary_fd);
    CHECK(log_open(summary_path));
    attrib_log_summary();
    log_close();
    summary = fopen(summary_path, "rb");
    CHECK(summary != NULL);
    if (summary) {
        size_t n = fread(logs, 1, sizeof logs - 1, summary);
        logs[n] = 0;
        CHECK(strstr(logs, "unanswered=0 timeout=1 requests=0 requests_untracked=0") != NULL);
        CHECK(strstr(logs, (const char *)secret) == NULL);
        CHECK(fclose(summary) == 0);
    }
    CHECK(unlink(summary_path) == 0);
    log_since(from, logs, sizeof logs);
    CHECK(strstr(logs, (const char *)secret) == NULL);
    CHECK(strstr(logs, "duplicate=3") != NULL);
    puts("ATTRIB classes: solicited, nop, duplicate, malformed, late, unmatched; metadata only");
}

static void test_multiset_and_capacity(void)
{
    int i;
    attrib_reset();
    for (i = 0; i < 16; i++) attrib_submit(0x1001, 1, 100);
    attrib_submit(0x1002, 1, 100);
    CHECK(attrib_get()->submitted == 17 && attrib_get()->unanswered == 17);
    CHECK(attrib_get()->untracked == 1);
    CHECK(answer(0x1002, 0, 101) == ATTRIB_UNTRACKED);
    for (i = 0; i < 16; i++) CHECK(answer(0x1001, 0, 102) == ATTRIB_SOLICITED);
    CHECK(answer(0x1001, 0, 103) == ATTRIB_DUPLICATE);
    CHECK(attrib_get()->unanswered == 0);
    metadata_only();
    CHECK(strstr(json, "\"untracked\":1") != NULL);
    attrib_reset();
    for (i = 0; i < 70; i++) CHECK(answer(0x5000 + i, 0, i) == ATTRIB_UNMATCHED);
    metadata_only();
    CHECK(strstr(json, "\"samples_omitted\":6") != NULL);
    CHECK(strstr(json, "\"opcodes_omitted\":6") != NULL);
    CHECK(strstr(json, "\"opcode\":20543") != NULL); /* first 64, never last 64 */
    CHECK(strstr(json, "\"opcode\":20544") == NULL);
    {
        char tiny[8] = "guard";
        CHECK(attrib_json(tiny, sizeof tiny) == 0);
        CHECK(tiny[0] == 0); /* never publish a truncated success */
    }
    puts("ATTRIB capacity: 16-entry multiset, untracked submission, first 64 samples");
}

static void test_requests(void)
{
    unsigned char ev[] = {4, 10, 1, 2, 0xaa, 0xbb, 0xcc, 0xdd, 8, 0x25, 0, 1};
    attrib_reset();
    CHECK(attrib_observe(ev, sizeof ev, 100) == ATTRIB_IGNORED);
    CHECK(attrib_observe(ev, sizeof ev, 101) == ATTRIB_IGNORED);
    ev[11] = 2;
    CHECK(attrib_observe(ev, sizeof ev, 102) == ATTRIB_IGNORED);
    CHECK(attrib_get()->requests == 3);
    CHECK(attrib_observe(ev, 11, 103) == ATTRIB_MALFORMED);
    metadata_only();
    CHECK(strstr(json, "\"cod\":9480,\"link_type\":1,\"count\":2,\"addr_tail\":\"02:01\"") != NULL);
    CHECK(strstr(json, "DD:CC:BB:AA") == NULL);
    puts("ATTRIB Connection Request: grouped CoD/link type, two address bytes only");
}

static void test_usb_overflow(void)
{
    unsigned char events[10 * 6], packet[32];
    hci_t h;
    int i, j, n = 0;
    reset();
    attrib_reset();
    h = start();
    for (i = 0; i < 10; i++) {
        unsigned char *e = events + 6 * i;
        e[0] = 0x0e; e[1] = 4; e[2] = 1;
        put16(e + 3, 0x1005); e[5] = 0;
    }
    /* Seven completions carry 70 distinct events into the real 64-event ring. */
    for (j = 0; j < 7; j++) {
        completion(j, events, sizeof events);
        CHECK(h.ops->pump(h.ctx, 0) == 1);
    }
    CHECK(attrib_get()->overflow == 6);
    while (h.ops->next_event(h.ctx, packet, sizeof packet)) n++;
    CHECK(n == 64);
    CHECK(attrib_get()->overflow == 6);
    metadata_only();
    CHECK(strstr(json, "\"overflow\":6") != NULL);
    closed_once(h);
    puts("ATTRIB USB overflow: 70 arrivals, 64 retained, exactly 6 dropped events");
}

static unsigned commands[65536], ncommands;
static int recording_cmd(void *ctx, unsigned op, const void *p, int n)
{
    commands[op]++;
    ncommands++;
    return sim_model_command(ctx, op, p, n);
}

#if !WHATEVERPAD_PROBE
static const unsigned probe_startup[] = {
    0x1002, 0x1005, 0x0c01, 0x2018, 0x0c55, 0x0c19, 0x0c1a, 0x2018,
    0x1009, 0x2001, 0x2018, 0x2002, 0x200b, 0x2018
};
typedef struct {
    struct { unsigned op, generated, answered, parent, barrier; } invocation[32];
    unsigned count, pending, required_parent, barriers;
    unsigned char random[4][8];
    int startup, missing;
} probe_trace;

static int probe_setting(unsigned op)
{
    return op == 0x0c01 || op == 0x0c1a || op == 0x2001 || op == 0x200b;
}

static void observe_probe(void *ud, enum sim_observation kind, sim_origin origin,
                          unsigned code, const unsigned char *p, int n)
{
    probe_trace *t = ud;
    if (!origin.invocation) return; /* deliberately injected raw events have no command origin */
    CHECK(origin.invocation < sizeof t->invocation / sizeof t->invocation[0]);
    CHECK(!origin.foreign && !origin.copy);
    unsigned id = origin.invocation;
    if (kind == SIM_COMMAND) {
        if (t->missing) {
            CHECK(code == 0x1002 && t->count < 3 && !t->invocation[id].op);
            CHECK(attrib_get()->unanswered == (unsigned)(t->count != 0));
            t->invocation[id].op = code;
            t->pending = id;
            t->count++;
            return;
        }
        CHECK(t->startup && t->count < sizeof probe_startup / sizeof probe_startup[0]);
        /* The exact effect sequence catches retransmission even if the model
         * assigns the repeated submission a new private invocation number. */
        CHECK(code == probe_startup[t->count]);
        CHECK(!t->invocation[id].op && !t->pending && attrib_get()->unanswered == 0);
        if (code == 0x2018) {
            CHECK(n == 0 && t->required_parent && id != t->required_parent);
            CHECK(probe_setting(t->invocation[t->required_parent].op));
            CHECK(!t->invocation[t->required_parent].barrier);
            t->invocation[id].parent = t->required_parent;
        } else CHECK(!t->required_parent);
        t->invocation[id].op = code;
        t->pending = id;
        t->count++;
        return;
    }
    CHECK(code == 0x0e && n >= 4 && !p[3]);
    CHECK(t->invocation[id].op == le16(p + 1));
    if (kind == SIM_GENERATED) {
        CHECK(++t->invocation[id].generated == 1);
        return; /* generation is not delivery of the required answer */
    }
    CHECK(kind == SIM_DELIVERED && t->pending == id);
    CHECK(t->invocation[id].generated == 1 && ++t->invocation[id].answered == 1);
    CHECK(attrib_get()->unanswered == 0);
    t->pending = 0;
    if (probe_setting(t->invocation[id].op)) t->required_parent = id;
    if (t->invocation[id].op == 0x2018) {
        CHECK(n == 12 && t->required_parent == t->invocation[id].parent && t->barriers < 4);
        for (unsigned i = 0; i < t->barriers; i++) CHECK(memcmp(t->random[i], p + 4, 8));
        memcpy(t->random[t->barriers++], p + 4, 8);
        t->invocation[t->required_parent].barrier = 1;
        t->required_parent = 0;
    }
}

static void test_ordinary_startup(void)
{
    sim_pad pad = { .mode = SIM_RECONNECT };
    sim_t *s = sim_new(&pad);
    probe_trace trace = {.startup = 1};
    unsigned i;
    unsigned char mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
    unsigned char original = 0, scan = 2;
    attrib_reset();
    memset(commands, 0, sizeof commands);
    ncommands = 0;
    sim_output_hooks(s, recording_cmd, NULL);
    sim_observe(s, observe_probe, &trace);
    host_t *h = host_prepare(sim_hci(s), "build/attrib-absent.db", NULL);
    CHECK(h != NULL);
    sim_bind_ownership(s, host_ownership(h));
    /* Exercise the original fourteen real setup effects without chip_report's
     * extra read-only diagnostics; barriers still come solely from the ledger. */
    CHECK(hci_sync(h, 0x1002, NULL, 0));
    CHECK(hci_sync(h, 0x1005, NULL, 0));
    CHECK(hci_sync(h, 0x0c01, mask, sizeof mask));
    CHECK(hci_sync(h, 0x0c55, NULL, 0));
    CHECK(hci_sync(h, 0x0c19, NULL, 0));
    CHECK(host_setting(h, 0x0c1a, &scan, 1, &original, 1));
    CHECK(hci_sync(h, 0x1009, NULL, 0));
    le_setup(h);
    host_poll(h, 0);
    trace.startup = 0;
    CHECK(trace.count == sizeof probe_startup / sizeof probe_startup[0] && ncommands == trace.count);
    CHECK(!trace.pending && !trace.required_parent && trace.barriers == 4);
    CHECK(attrib_get()->submitted == trace.count && !attrib_get()->unanswered && !attrib_get()->timeout);
    for (i = 1; i <= trace.count; i++)
        CHECK(trace.invocation[i].generated == 1 && trace.invocation[i].answered == 1);
    host_close(h);
    CHECK(trace.count == sizeof probe_startup / sizeof probe_startup[0] && !trace.pending);
    CHECK(sim_resources(s).closes == 1);
    sim_free(s);
    puts("ORDINARY startup: 14 once-only invocations, 4 fresh required barriers");
}
#endif

static void test_host_profile(void)
{
    sim_pad pad = { .mode = SIM_RECONNECT };
    sim_t *s = sim_new(&pad);
    hci_t transport = sim_hci(s);
    host_t *h;
    FILE *bond;
    unsigned long from;
    unsigned i;
#if WHATEVERPAD_PROBE
    int consumed;
#endif
    static const unsigned char peer[6] = {1, 2, 0xaa, 0xbb, 0xcc, 0xdd};
    static const unsigned char linking_peer[6] = {9, 8, 7, 6, 5, 4};
    unsigned char cc[32] = {1, 0x34, 0x12, 0}, cs[32] = {0, 1, 0x35, 0x12};
    attrib_reset();
    memset(commands, 0, sizeof commands);
    ncommands = 0;
    sim_output_hooks(s, recording_cmd, NULL);
    bond = fopen("build/attrib-preserved.db", "wb");
    CHECK(bond != NULL);
    if (bond) {
        CHECK(fwrite("keep", 1, 4, bond) == 4);
        CHECK(fclose(bond) == 0);
    }
    h = sim_open_host(s, "build/attrib-preserved.db", NULL);
    CHECK(h != NULL);
    if (!h) { transport.ops->close(transport.ctx); sim_free(s); return; }
#if WHATEVERPAD_PROBE
    CHECK(ncommands == 1 && commands[0x1005] == 1);
#endif
    /* Exercise both trace sinks, even with an outstanding, unready link. */
    from = log_seq();
    CHECK(host_tune_set(h, "trace", 1));
    CHECK(link_new(h, linking_peer, 0, 0) != NULL);
    memcpy(cc + 4, secret, sizeof secret);
    memcpy(cs + 4, secret, sizeof secret);
    sim_inject_event(s, 0x0e, cc, 4 + sizeof secret);
    sim_inject_event(s, 0x0f, cs, 4 + sizeof secret);
    host_poll(h, 1);
    metadata_only();
#if WHATEVERPAD_PROBE
    /* Even unknown/discovery events must not expose bytes through older sinks. */
    for (i = 1; i < 256; i++) {
        unsigned char data[255] = {0};
        memcpy(data + 16, secret, sizeof secret);
        if (i != 0x0e && i != 0x0f) sim_inject_event(s, (unsigned char)i, data, sizeof data);
        host_poll(h, 0);
    }
    CHECK(host_pair(h, 30) == 0);
    {
        unsigned char reply[260];
        int len;
        static const unsigned forbidden[] = {0x0402, 0x0408, 0x200e, 0x0c03};
        for (i = 0; i < sizeof forbidden / sizeof forbidden[0]; i++) {
            CHECK(!send_cmd(h, forbidden[i], NULL, 0));
            CHECK(!host_raw_cmd(h, forbidden[i], NULL, 0, reply, &len));
        }
    }
    /* Exceed the old yield threshold and watchdog idle threshold deterministically. */
    i = ncommands;
    host_poll(h, 31000);
    CHECK(ncommands == i);
    i = ncommands;
    consumed = sim_consumed_requests(s);
    sim_peer_request(s, peer, 0x002508);
    host_poll(h, 1);
    CHECK(ncommands == i && sim_consumed_requests(s) == consumed + 1);
    CHECK(sim_resources(s).event_reads == 40 && sim_resources(s).acl_reads == 20);
    CHECK(sim_foreign_commands(s) == 0 && sim_rejects(s) == 0);
    host_close(h);
    CHECK(ncommands == i); /* no guessed restores, readbacks, or Disconnect */
    CHECK(!commands[0x0402] && !commands[0x0408] && !commands[0x200e] && !commands[0x0c03]);
    log_since(from, logs, sizeof logs);
    CHECK(strstr(logs, (const char *)secret) == NULL);
    CHECK(strstr(logs, "5a 51 5f 53 45 43 52 45 54 5f 37 33") == NULL);
    puts("PROBE start/idle/Stop: one buffer query, no idle/Stop submission");
#else
    (void)i;
    (void)from;
    (void)peer;
    host_close(h);
#endif
    host_tune_set(NULL, "trace", 0);
    CHECK(sim_resources(s).closes == 1);
    bond = fopen("build/attrib-preserved.db", "rb");
    CHECK(bond != NULL);
    if (bond) {
        char data[5] = {0};
        CHECK(fread(data, 1, sizeof data, bond) == 4 && !strcmp(data, "keep"));
        CHECK(fclose(bond) == 0);
    }
    CHECK(unlink("build/attrib-preserved.db") == 0);
    sim_free(s);
}

static void test_ordinary_missing_answer(void)
{
#if !WHATEVERPAD_PROBE
    sim_pad pad = {0};
    sim_t *s = sim_new(&pad);
    hci_t transport = sim_hci(s);
    host_t *h;
    probe_trace trace = {.startup = 1, .missing = 1};
    attrib_reset();
    memset(commands, 0, sizeof commands);
    ncommands = 0;
    sim_go_deaf(s);
    sim_output_hooks(s, recording_cmd, NULL);
    sim_observe(s, observe_probe, &trace);
    h = host_prepare(transport, "build/attrib-absent.db", NULL);
    CHECK(h != NULL);
    if (!h) { transport.ops->close(transport.ctx); sim_free(s); return; }
    sim_bind_ownership(s, host_ownership(h));
    CHECK(!host_start(h));
    CHECK(ncommands == 3 && commands[0x1002] == 3);
    CHECK(commands[0x1005] == 0 && attrib_get()->submitted == 3);
    CHECK(trace.count == 3 && trace.pending == 3);
    for (unsigned i = 1; i <= 3; i++) {
        CHECK(trace.invocation[i].op == 0x1002);
        CHECK(!trace.invocation[i].generated && !trace.invocation[i].answered);
    }
    CHECK(attrib_get()->unanswered == 1 && attrib_get()->timeout == 1);
    host_close(h); /* the caller retires the prepared host after failed startup */
    CHECK(ncommands == 3 && attrib_get()->submitted == 3);
    CHECK(attrib_get()->unanswered == 1 && attrib_get()->timeout == 1);
    sim_free(s);
    puts("PRODUCT missing answer: three wire sends, one waiter and final timeout");
#endif
}

static void test_probe_missing_answer(void)
{
#if WHATEVERPAD_PROBE
    sim_pad pad = {0};
    sim_t *s = sim_new(&pad);
    attrib_reset();
    memset(commands, 0, sizeof commands);
    ncommands = 0;
    sim_go_deaf(s);
    sim_output_hooks(s, recording_cmd, NULL);
    CHECK(sim_open_host(s, "build/attrib-absent.db", NULL) == NULL);
    CHECK(ncommands == 1 && commands[0x1005] == 1);
    CHECK(commands[0x1002] == 0 && attrib_get()->submitted == 1);
    CHECK(attrib_get()->timeout == 1 && attrib_get()->unanswered == 1);
    CHECK(sim_resources(s).closes == 1);
    sim_free(s);
#endif
}

static void test_probe_passive_tail(void)
{
#if WHATEVERPAD_PROBE
    static const int arrivals[] = {1000, 1500, 6000, 6001, -1};
    for (unsigned i = 0; i < sizeof arrivals / sizeof arrivals[0]; i++) {
        sim_pad pad = {0};
        sim_t *s = sim_new(&pad);
        long begin = sim_now();
        unsigned long from = log_seq();
        attrib_reset();
        memset(commands, 0, sizeof commands);
        ncommands = 0;
        sim_delay_events(s, 0x0e, 1, arrivals[i]);
        sim_output_hooks(s, recording_cmd, NULL);
        host_t *h = host_prepare(sim_hci(s), "build/attrib-absent.db", NULL);
        CHECK(h != NULL);
        sim_bind_ownership(s, host_ownership(h));
        CHECK(!host_start(h));
        CHECK(ncommands == 1 && commands[0x1005] == 1);
        CHECK(attrib_get()->submitted == 1 && attrib_get()->timeout == 1);
        CHECK(sim_now() - begin == 1000 + PROBE_TAIL_MS);
        CHECK(attrib_get()->classes[ATTRIB_LATE] == (unsigned)(arrivals[i] >= 0 && arrivals[i] <= 6000));
        CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 0);
        CHECK(attrib_get()->classes[ATTRIB_UNMATCHED] == 0);
        CHECK(!host_admits(h) && !own_activate(host_ownership(h)));
        CHECK(!hci_sync(h, 0x1005, NULL, 0) && ncommands == 1);
        log_since(from, logs, sizeof logs);
        CHECK(strstr(logs, "probe tail_complete") != NULL);
        if (arrivals[i] >= 0 && arrivals[i] <= 6000)
            CHECK(strstr(logs, "class=late") != NULL);
        host_close(h);
        CHECK(sim_resources(s).closes == 1);
        sim_free(s);
    }
    puts("PROBE tail: deadline, late, tail boundary, after-tail and missing; exactly one command each");
#endif
}

static void test_usb_secrets(void)
{
    unsigned char ev[32] = {0x0e, 4 + sizeof secret, 1, 0x34, 0x12, 0};
    hci_t h;
    unsigned long from = log_seq();
    reset();
    attrib_reset();
    h = start();
    memcpy(ev + 6, secret, sizeof secret);
    completion(0, ev, 6 + sizeof secret);
    h.ops->pump(h.ctx, 0);
    CHECK(h.ops->cmd(h.ctx, 0x2019, secret, sizeof secret));
    h.ops->diag(h.ctx);
    log_since(from, logs, sizeof logs);
    CHECK(strstr(logs, (const char *)secret) == NULL);
    CHECK(strstr(logs, "5a 51 5f 53 45 43 52 45 54 5f 37 33") == NULL);
    closed_once(h);
    puts("ATTRIB USB diagnostics: sentinel checked in captured log buffer");
}

static void test_usb_timeout_secrets(void)
{
    unsigned char ev[25] = {0x18, 23, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0xe6};
    unsigned long from;
    reset();
#if WHATEVERPAD_PROBE
    usb.wait_budget = 1000 + PROBE_TAIL_MS;
#endif
    attrib_reset();
    hci_t transport = start();
    host_t *h = host_prepare(transport, "build/attrib-absent.db", NULL);
    CHECK(h != NULL);
    CHECK(hci_usb_bind_ownership(&transport, host_ownership(h), NULL, NULL));
    memcpy(ev + 8, secret, sizeof secret);
    from = log_seq();
    completion(0, ev, sizeof ev);
    /* The USB transfer completes, but no HCI answer arrives: command_wait calls diag. */
    unsigned op = WHATEVERPAD_PROBE ? 0x1005 : 0x1002;
    long begin = clock_ms;
    CHECK(!hci_sync(h, op, NULL, 0));
#if WHATEVERPAD_PROBE
    CHECK(clock_ms - begin == 1000 + PROBE_TAIL_MS);
#else
    CHECK(clock_ms - begin == 2000);
#endif
    CHECK(usb.commands == 1 && usb.last_opcode == op);
    CHECK(attrib_get()->timeout == 1 && attrib_get()->unanswered == 1);
    CHECK(log_since(from, logs, sizeof logs) == log_seq());
    CHECK(strstr(logs, "diag:") != NULL);
    CHECK(strstr(logs, (const char *)secret) == NULL);
    CHECK(strstr(logs, "5a 51 5f 53") == NULL);
    CHECK(strstr(logs, "91 a2 b3 c4") == NULL);
    host_close(h);
    CHECK(usb.closes == 1 && !usb.fd_open && !usb.initialized);
    puts("ATTRIB USB timeout: real command timeout diagnostics contain no sentinel bytes");
}

#if WHATEVERPAD_PROBE
typedef struct { long at; int delivered; } late_query;
static void deliver_late_query(void *ud)
{
    late_query *q = ud;
    static const unsigned char answer[] = {0x0e, 11, 1, 5, 0x10, 0, 64, 0, 0, 4, 0, 0, 0};
    if (clock_ms < q->at || q->delivered) return;
    q->delivered = 1;
    completion(0, answer, sizeof answer);
}
#endif

static void test_usb_stalled_query_clock(void)
{
#if !WHATEVERPAD_PROBE
    reset();
    usb.wait_budget = 5000;
    attrib_reset();
    hci_t transport = start();
    host_t *h = host_prepare(transport, "build/attrib-absent.db", NULL);
    CHECK(h != NULL);
    CHECK(hci_usb_bind_ownership(&transport, host_ownership(h), NULL, NULL));
    long begin = clock_ms;
    frozen_waits = 0;
    freeze_usb_clock = 1;
    CHECK(!hci_sync(h, 0x1002, NULL, 0));
    freeze_usb_clock = 0;
    CHECK(frozen_waits <= 64);
    CHECK(clock_ms == begin && usb.commands == 1 && !host_admits(h));
    host_close(h);
    CHECK(usb.closes == 1 && !usb.fd_open && !usb.initialized);
#endif
}

static void test_probe_usb_tail(void)
{
#if WHATEVERPAD_PROBE
    reset();
    usb.wait_budget = 1000 + PROBE_TAIL_MS; /* Exact new diagnostic duration, not a relaxed old USB case. */
    attrib_reset();
    hci_t transport = start();
    host_t *h = host_prepare(transport, "build/attrib-absent.db", NULL);
    CHECK(h != NULL);
    late_query q = {.at = clock_ms + 1500};
    CHECK(hci_usb_bind_ownership(&transport, host_ownership(h), deliver_late_query, &q));
    long begin = clock_ms;
    CHECK(!host_start(h));
    CHECK(q.delivered && clock_ms == begin + 1000 + PROBE_TAIL_MS);
    CHECK(usb.commands == 1 && usb.last_opcode == 0x1005);
    CHECK(attrib_get()->submitted == 1 && attrib_get()->timeout == 1);
    CHECK(attrib_get()->classes[ATTRIB_LATE] == 1 && attrib_get()->unanswered == 0);
    CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 0 &&
          attrib_get()->classes[ATTRIB_UNMATCHED] == 0);
    CHECK(test_usb_receipts.calls == 1 && test_usb_receipts.last == OWN_RECEIPT_NONE);
    CHECK(!host_admits(h) && !own_activate(host_ownership(h)));
    CHECK(!hci_sync(h, 0x1005, NULL, 0) && usb.commands == 1);
    CHECK(usb.starts[0] >= 2); /* The late completion is rearmed for read-only observation. */
    host_close(h);
    CHECK(usb.closes == 1 && !usb.fd_open && !usb.initialized);
    puts("PROBE real USB tail: one late diagnostic, no sequenced receipt or renewed authority");
#endif
}

static void test_probe_usb_receipts(void)
{
#if WHATEVERPAD_PROBE
    static const unsigned char ev[] = {0x0e, 4, 1, 5, 0x10, 0};
    static const unsigned char acl[] = {0x41, 0x20, 1, 0, 0xaa};
    unsigned char packet[32];
    unsigned long from = log_seq();
    hci_t h;
    reset();
    probe_deadline_ms = clock_ms + 1;
    h = start();
    completion(0, ev, sizeof ev);
    completion(EVENTS, acl, sizeof acl);
    CHECK(h.ops->pump(h.ctx, 0) == 1);
    CHECK(h.ops->next_event(h.ctx, packet, sizeof packet) == sizeof ev);
    CHECK(h.ops->next_acl(h.ctx, packet, sizeof packet) == sizeof acl);
    CHECK(h.ops->pump(h.ctx, 0) == 0); /* Arm the next reads before completing them. */
    clock_ms = probe_deadline_ms;
    completion(0, ev, sizeof ev);
    completion(EVENTS, acl, sizeof acl);
    CHECK(h.ops->pump(h.ctx, 0) == 1);
    usb.fail_request = USB_FS_STOP;
    usb.fail_index = 1;
    closed_once(h);
    log_since(from, logs, sizeof logs);
    CHECK(strstr(logs, "probe selection function=0 interface=0 events=0x81 acl_in=0x82 acl_out=0x02") != NULL);
    CHECK(strstr(logs, "probe open node=/dev/ugen0.2 attempts=1 rc=0 errno=0 mono_ms=") != NULL);
    CHECK(strstr(logs, "op=start transfer=0 rc=0 errno=0") != NULL);
    CHECK(strstr(logs, "op=complete transfer=0 rc=0 errno=0 phase=before") != NULL);
    CHECK(strstr(logs, "op=complete transfer=0 rc=0 errno=0 phase=late") != NULL);
    CHECK(strstr(logs, "op=stop transfer=1 rc=-1 errno=5") != NULL);
    CHECK(strstr(logs, "op=uninit transfer=-1 rc=0 errno=0") != NULL);
    CHECK(strstr(logs, "probe close rc=0 errno=0 mono_ms=") != NULL);
    CHECK(strstr(logs, "event_before=1 event_late=1 acl_before=1 acl_late=1") != NULL);
    CHECK(strstr(logs, (const char *)secret) == NULL);
    reset(); usb.reject_node = 1;
    CHECK(!hci_usb_open(&h));
    log_since(from, logs, sizeof logs);
    CHECK(strstr(logs, "attempts=5 rc=-1 errno=6 mono_ms=") != NULL);
    probe_deadline_ms = 0;
    puts("PROBE USB receipts: selection, actual node attempts, operation results and deadline-split completion counts");
#endif
}

static void test_receipt_provenance(void)
{
    unsigned char complete[32] = {0x0e, 4 + sizeof secret, 1, 9, 0x10, 0};
    static const unsigned char nop[] = {0x0e, 3, 1, 0, 0};
    attrib_reset();
    memcpy(complete + 6, secret, sizeof secret);
    attrib_submit(0x1009, 1, 100);
    CHECK(attrib_observe_receipt(complete, 6 + sizeof secret, 101,
                                OWN_RECEIPT_NONE) == ATTRIB_UNMATCHED);
    CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 0 && attrib_get()->unanswered == 1);
    CHECK(attrib_observe_receipt(nop, sizeof nop, 102, OWN_RECEIPT_NONE) == ATTRIB_NOP);
    CHECK(attrib_get()->unanswered == 1);
    CHECK(attrib_observe_receipt(complete, 6 + sizeof secret, 103,
                                OWN_RECEIPT_SEQUENCED) == ATTRIB_SOLICITED);
    CHECK(attrib_get()->classes[ATTRIB_UNMATCHED] == 1 &&
          attrib_get()->classes[ATTRIB_SOLICITED] == 1 && attrib_get()->unanswered == 0);
    metadata_only();
    {   /* Another owner's reply is not ambiguity: the host's counter sum must not move. */
        static const unsigned char cancel[] = {0x0e, 4, 1, 0x02, 0x04, 0};
        attrib_reset();
        CHECK(attrib_observe_receipt(cancel, sizeof cancel, 104, OWN_RECEIPT_FOREIGN) == ATTRIB_IGNORED);
        CHECK(attrib_get()->classes[ATTRIB_UNMATCHED] == 0 && attrib_get()->overflow == 0);
    }
}

static void test_credit_event_forms(void)
{
    static const unsigned char cc[] = {0x0e,3,0,0,0};
    static const unsigned char cc_extra[] = {0x0e,5,255,0,0,0xff,0x7e};
    static const unsigned char cs[] = {0x0f,4,0,1,0,0};
    static const unsigned char failed_cs[] = {0x0f,4,0x0c,1,0,0};
    static const unsigned char bad_length[] = {0x0e,3,1,0,0,0};
    unsigned violations = 0;
    attrib_reset();
    attrib_submit(0x1009, 1, 100);
    CHECK(attrib_observe_receipt(cc, sizeof cc, 101, OWN_RECEIPT_NONE) == ATTRIB_NOP);
    CHECK(attrib_observe_receipt(cc_extra, sizeof cc_extra, 102, OWN_RECEIPT_NONE) == ATTRIB_NOP);
    CHECK(attrib_observe_receipt(cs, sizeof cs, 103, OWN_RECEIPT_NONE) == ATTRIB_NOP);
    violations += attrib_observe_receipt(failed_cs, sizeof failed_cs, 104,
                                         OWN_RECEIPT_NONE) != ATTRIB_UNMATCHED;
    violations += attrib_observe_receipt(bad_length, sizeof bad_length, 105,
                                         OWN_RECEIPT_NONE) != ATTRIB_MALFORMED;
    CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 0 && attrib_get()->unanswered == 1);
    CHECK(violations == 0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--stalled-clock")) {
        test_usb_stalled_query_clock();
        printf("%d USB clock checks, %d failed\n", checks, failures);
        return failures != 0;
    }
    test_classes();
    test_multiset_and_capacity();
    test_requests();
    test_usb_overflow();
#if !WHATEVERPAD_PROBE
    test_ordinary_startup();
#endif
    test_ordinary_missing_answer();
    test_host_profile();
    test_probe_missing_answer();
    test_usb_timeout_secrets();
    test_usb_stalled_query_clock();
    test_probe_passive_tail();
    test_probe_usb_tail();
    test_usb_secrets();
    test_probe_usb_receipts();
    test_receipt_provenance();
    test_credit_event_forms();
    ph_clock_hook = NULL;
    printf("%d attribution checks, %d failed (probe=%d)\n", checks, failures, WHATEVERPAD_PROBE);
    return failures != 0;
}
