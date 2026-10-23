/* Reuse the real USB syscall stand-in without editing its behavior of record. */
#define main usb_lifetime_main
#define test_usb_ioctl usb_fixture_ioctl
#include "test_hci_usb.c"
#undef test_usb_ioctl
#undef main
#include "../src/attrib.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include "sim.h"
#include <stdlib.h>

static char json[ATTRIB_JSON_MAX], logs[130000];
static const unsigned char secret[] = "ZQ_SECRET_73";

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
    return sim_hci(ctx).ops->cmd(ctx, op, p, n);
}

static void test_host_profile(void)
{
    sim_pad pad = { .mode = SIM_RECONNECT };
    sim_t *s = sim_new(&pad);
    hci_t transport = sim_hci(s);
    hci_ops ops = *transport.ops;
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
    ops.cmd = recording_cmd;
    transport.ops = &ops;
    bond = fopen("build/attrib-preserved.db", "wb");
    CHECK(bond != NULL);
    if (bond) {
        CHECK(fwrite("keep", 1, 4, bond) == 4);
        CHECK(fclose(bond) == 0);
    }
    h = host_open(transport, "build/attrib-preserved.db", NULL);
    CHECK(h != NULL);
    if (!h) { transport.ops->close(transport.ctx); sim_free(s); return; }
    /* Exercise both trace sinks, even with an outstanding, unready link. */
    from = log_seq();
    CHECK(host_tune_set(h, "trace", 1));
    CHECK(link_new(h, linking_peer) != NULL);
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
    host_poll(h, 31000);
    i = ncommands;
    consumed = sim_consumed_requests(s);
    sim_peer_request(s, peer, 0x002508);
    host_poll(h, 1);
    CHECK(ncommands == i && sim_consumed_requests(s) == consumed + 1);
    CHECK(sim_resources(s).event_reads == 40 && sim_resources(s).acl_reads == 20);
    CHECK(sim_foreign_commands(s) == 0 && sim_rejects(s) == 0);
    host_close(h);
    CHECK(ncommands == i); /* no guessed restores, readbacks, or Disconnect */
    for (i = 0; i < 65536 && commands[i] <= 1; i++) {}
    CHECK(i == 65536);
    CHECK(!commands[0x0402] && !commands[0x0408] && !commands[0x200e] && !commands[0x0c03]);
    log_since(from, logs, sizeof logs);
    CHECK(strstr(logs, (const char *)secret) == NULL);
    CHECK(strstr(logs, "5a 51 5f 53 45 43 52 45 54 5f 37 33") == NULL);
    puts("PROBE start/idle/Stop: no repeated or forbidden command, no restore, no native reply; reads remain open");
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

static void test_probe_missing_answer(void)
{
#if WHATEVERPAD_PROBE
    sim_pad pad = {0};
    sim_t *s = sim_new(&pad);
    hci_t transport = sim_hci(s);
    hci_ops ops = *transport.ops;
    attrib_reset();
    memset(commands, 0, sizeof commands);
    ncommands = 0;
    sim_go_deaf(s);
    ops.cmd = recording_cmd;
    transport.ops = &ops;
    CHECK(host_open(transport, "build/attrib-absent.db", NULL) == NULL);
    CHECK(ncommands == 1 && commands[0x1005] == 1);
    CHECK(attrib_get()->unanswered == 1 && attrib_get()->timeout == 1);
    transport.ops->close(transport.ctx); /* startup transport ownership is todo 15 */
    sim_free(s);
    puts("PROBE missing answer: one submission, one timeout, no retransmission");
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
#if WHATEVERPAD_PROBE
    log_since(from, logs, sizeof logs);
    CHECK(strstr(logs, (const char *)secret) == NULL);
    CHECK(strstr(logs, "5a 51 5f 53 45 43 52 45 54 5f 37 33") == NULL);
#else
    (void)from;
#endif
    closed_once(h);
    puts("ATTRIB USB diagnostics: sentinel checked in probe log buffer");
}

int main(void)
{
    test_classes();
    test_multiset_and_capacity();
    test_requests();
    test_usb_overflow();
    test_host_profile();
    test_probe_missing_answer();
    test_usb_secrets();
    ph_clock_hook = NULL;
    printf("%d attribution checks, %d failed (probe=%d)\n", checks, failures, WHATEVERPAD_PROBE);
    return failures != 0;
}
