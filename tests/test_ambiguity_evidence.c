/* GPL-3.0-or-later. Share the real USB boundary fixture, not a copied driver. */
#define main usb_fixture_main
#include "test_hci_usb.c"
#undef main
#include "../src/log.h"
#include <limits.h>

static char evidence_json[ATTRIB_EVIDENCE_MAX];
static const char evidence_secret[] = "ZQ_SECRET_73";

typedef struct {
    char source[16], session[16];
    unsigned code, subevent, length, opcode, status, status_known;
    long timestamp, since_ready;
    int inquiry, scan;
    unsigned npending, nrecent;
    struct { unsigned opcode; long age; } pending[16];
    struct { unsigned code, length; long age; } recent[4];
} evidence_report;

/* Parse the entire machine-consumed report, including closing delimiters. */
static evidence_report read_evidence(void)
{
    evidence_report r = {0};
    int n = 0;
    size_t length = attrib_evidence_json(evidence_json, sizeof evidence_json);
    CHECK(length > 0 && length < ATTRIB_EVIDENCE_MAX);
    CHECK(strlen(evidence_json) == length);
    CHECK(!strstr(evidence_json, evidence_secret));
    CHECK(!strstr(evidence_json, "5a515f5345435245545f3733"));
    CHECK(!strstr(evidence_json, "5A515F5345435245545F3733"));
    CHECK(sscanf(evidence_json,
        "{\"source\":\"%15[^\"]\",\"code\":%u,\"subevent\":%u,\"length\":%u,"
        "\"opcode\":%u,\"status\":%u,\"status_known\":%u,\"session\":\"%15[^\"]\","
        "\"timestamp_ms\":%ld,\"since_ready_ms\":%ld,\"inquiry\":%d,\"scan\":%d,\"pending\":[%n",
        r.source, &r.code, &r.subevent, &r.length, &r.opcode, &r.status,
        &r.status_known, r.session, &r.timestamp, &r.since_ready,
        &r.inquiry, &r.scan, &n) == 12 && n > 0);
    const char *p = evidence_json + n;
    while (*p != ']') {
        CHECK(r.npending < 16);
        n = 0;
        CHECK(sscanf(p, "{\"opcode\":%u,\"age_ms\":%ld}%n",
                     &r.pending[r.npending].opcode, &r.pending[r.npending].age, &n) == 2 && n > 0);
        r.npending++;
        p += n;
        if (*p != ',') break;
        p++;
        CHECK(*p == '{');
    }
    static const char recent[] = "],\"recent\":[";
    CHECK(!strncmp(p, recent, sizeof recent - 1));
    p += sizeof recent - 1;
    while (*p != ']') {
        CHECK(r.nrecent < 4);
        n = 0;
        CHECK(sscanf(p, "{\"code\":%u,\"length\":%u,\"age_ms\":%ld}%n",
                     &r.recent[r.nrecent].code, &r.recent[r.nrecent].length,
                     &r.recent[r.nrecent].age, &n) == 3 && n > 0);
        r.nrecent++;
        p += n;
        if (*p != ',') break;
        p++;
        CHECK(*p == '{');
    }
    CHECK(!strcmp(p, "]}"));
    return r;
}

static void test_first_event_and_retention(own_policy policy)
{
    unsigned char opaque[2 + sizeof evidence_secret] = {0xff, sizeof evidence_secret};
    static const unsigned char completed[] = {0x13,5,1,0x41,0,3,0};
    static const unsigned char le[] = {0x3e,1,0xfe};
    static const unsigned char foreign[] = {0x0e,4,1,1,0x10,0x0c};
    static const unsigned char nop[] = {0x0e,3,1,0,0};
    unsigned char out[16];
    char first[ATTRIB_EVIDENCE_MAX], logs[4096];
    own_token token;
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    attrib_host_context(900, 1, 0);
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    unsigned long from = log_seq();
    memcpy(opaque + 2, evidence_secret, sizeof evidence_secret);
    for (int i = 1; i <= 5; i++) {
        clock_ms = 1000 + 10 * i;
        if (i == 3) receive_event(h, completed, sizeof completed);
        else if (i == 4) receive_event(h, le, sizeof le);
        else receive_event(h, opaque, sizeof opaque);
        CHECK(log_seq() == from); /* Ordinary arrivals do not log evidence. */
    }
    attrib_resend(0x1009); /* Same logical waiter; age must still start at 1000. */
    clock_ms = 1100;
    receive_event(h, foreign, sizeof foreign);
    CHECK(log_seq() == from + 1);
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "event") && !strcmp(r.session, "active"));
    CHECK(r.code == OWN_CC && r.opcode == 0x1001 && r.length == sizeof foreign);
    CHECK(r.status == 0x0c && r.status_known && r.subevent == 0);
    CHECK(r.timestamp == 1100 && r.since_ready == 200 && r.inquiry == 1 && r.scan == 0);
    CHECK(r.npending == 1 && r.pending[0].opcode == 0x1009 && r.pending[0].age == 100);
    CHECK(r.nrecent == 4);
    CHECK(r.recent[0].code == 0x13 && r.recent[0].length == sizeof completed && r.recent[0].age == 70);
    CHECK(r.recent[1].code == 0x3e && r.recent[1].length == sizeof le && r.recent[1].age == 60);
    CHECK(r.recent[2].code == 0xff && r.recent[2].length == sizeof opaque && r.recent[2].age == 50);
    CHECK(r.recent[3].code == OWN_CC && r.recent[3].length == sizeof foreign && r.recent[3].age == 0);
    own_summary summary;
    own_snapshot(l, &summary);
    CHECK(summary.runtime.reason == SESSION_AMBIGUOUS && summary.runtime.state == SESSION_QUIESCING);
    memcpy(first, evidence_json, sizeof first);
    CHECK(usb.busy[1]);
    completion(1, nop, sizeof nop); /* Real retained read during cleanup. */
    CHECK(h.ops->pump(h.ctx, 0) == 1);
    CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof nop);
    attrib_watch(l, "counter", NULL, 0, clock_ms + 1);
    own_detector_trip(l, (uint64_t)clock_ms + 1);
    attrib_check(l);
    read_evidence();
    CHECK(!strcmp(first, evidence_json) && log_seq() == from + 1);
    log_since(from, logs, sizeof logs);
    CHECK(!strstr(logs, evidence_secret));
    closed_once(h); own_free(l);

    attrib_begin_try();
    read_evidence();
    CHECK(!strcmp(first, evidence_json));
    reset(); h = start(); l = bound(&h, policy);
    read_evidence();
    CHECK(!strcmp(first, evidence_json)); /* Reopen cannot erase the frozen report. */
    static const unsigned char next[] = {0x0e,4,1,2,0x10,0};
    from = log_seq();
    receive_event(h, next, sizeof next);
    r = read_evidence();
    CHECK(log_seq() == from + 1 && r.opcode == 0x1002 && r.status == 0);
    CHECK(r.since_ready == -1 && !r.inquiry && !r.scan);
    CHECK(r.npending == 0 && r.nrecent == 1 && r.recent[0].code == OWN_CC);
    CHECK(strcmp(first, evidence_json) != 0);
    closed_once(h); own_free(l);
}

static void test_pending_before_deadline(own_policy policy)
{
    own_token token;
    own_summary summary;
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    clock_ms = 3000;
    h.ops->pump(h.ctx, 0);
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "deadline") && !strcmp(r.session, "active"));
    CHECK(r.timestamp == 3000 && !r.code && !r.length && !r.opcode && !r.status_known);
    CHECK(r.npending == 1 && r.pending[0].opcode == 0x1009 && r.pending[0].age == 2000);
    own_snapshot(l, &summary);
    CHECK(summary.runtime.reason == SESSION_AMBIGUOUS && summary.reply_timeout);
    CHECK(attrib_get()->timeout == 1 && attrib_get()->unanswered == 1);
    closed_once(h); own_free(l);
}

static void test_lost_ring_event(own_policy policy)
{
    unsigned char events[64 * 3];
    static const unsigned char incoming[] = {0xff,1,0xaa};
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    for (unsigned i = 0; i < 64; i++) {
        events[3*i] = i ? 1 : 0xfe;
        events[3*i+1] = 1;
        events[3*i+2] = 0;
    }
    completion(0, events, sizeof events);
    CHECK(h.ops->pump(h.ctx, 0) == 1 && attrib_get()->overflow == 0);
    completion(1, incoming, sizeof incoming);
    CHECK(h.ops->pump(h.ctx, 0) == 1 && attrib_get()->overflow == 1);
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "event_ring") && !strcmp(r.session, "active"));
    CHECK(r.code == 0xfe && r.length == 3 && !r.status_known && !r.opcode);
    CHECK(r.nrecent == 4 && r.recent[3].code == 0xff);
    closed_once(h); own_free(l);
}

static void test_stream_gap_precedes_new_authority(own_policy policy)
{
    static const unsigned char partial[] = {0xff,20,'Z','Q','_','S','E','C'};
    static const unsigned char answer[] = {0x0e,4,1,9,0x10,0};
    own_token token;
    own_summary summary;
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    completion(0, partial, sizeof partial);
    CHECK(h.ops->pump(h.ctx, 0) == 0 && test_usb_receipts.calls == 0);
    clock_ms += 101; /* Set the stale-fragment state directly; no timed wait. */
    completion(1, answer, sizeof answer);
    CHECK(h.ops->pump(h.ctx, 0) == 1);
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "stream_gap") && !strcmp(r.session, "active"));
    CHECK(r.code == 0xff && r.length == sizeof partial && !r.opcode && !r.status_known);
    CHECK(r.npending == 1 && r.pending[0].opcode == 0x1009 && r.pending[0].age == 101);
    CHECK(r.nrecent == 0); /* The lost partial was never a complete event. */
    own_snapshot(l, &summary);
    CHECK(summary.runtime.reason == SESSION_AMBIGUOUS);
    CHECK(attrib_get()->overflow == 1 && attrib_get()->unanswered == 1);
    CHECK(attrib_get()->classes[ATTRIB_SOLICITED] == 0);
    CHECK(test_usb_receipts.last == OWN_RECEIPT_NONE);
    closed_once(h); own_free(l);
}

static void test_explicit_usb_detectors(own_policy policy)
{
    static const unsigned char malformed_disconnect[] = {5,2,0,0xff};
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    receive_event(h, malformed_disconnect, sizeof malformed_disconnect);
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "event") && r.code == 5 && r.length == 4 && !r.status_known);
    closed_once(h); own_free(l);

    reset(); attrib_reset(); h = start(); l = bound(&h, policy);
    own_token token;
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    usb.command_status = 1;
    CHECK(h.ops->pump(h.ctx, 0) == -1);
    r = read_evidence();
    CHECK(!strcmp(r.source, "dispatch") && !r.code && !strcmp(r.session, "active"));
    CHECK(r.npending == 1 && r.pending[0].opcode == 0x1009);
    closed_once(h); own_free(l);

    reset(); attrib_reset(); h = start(); l = bound(&h, policy);
    own_ref parent;
    link_ref link = acquire_link(h, l, &parent);
    own_acl_lane *lane = own_acl_new(l);
    own_acl_token output;
    static const unsigned char packet[] = {0x41,0x20,0,0};
    CHECK(lane != NULL);
    CHECK(own_acl_prepare(lane, link, packet, sizeof packet, (uint64_t)clock_ms, &output) == OWN_OK);
    CHECK(acl_submit(h, lane, &output) == OWN_SENT);
    completion(OUT, NULL, 0); usb.output_status = 1;
    CHECK(h.ops->pump(h.ctx, 0) == -1);
    r = read_evidence();
    CHECK(!strcmp(r.source, "dispatch") && !r.code && !strcmp(r.session, "active"));
    closed_once(h); own_acl_free(lane); own_free(l);
}

static void test_status_layouts(own_policy policy)
{
    static const struct {
        unsigned char event[33];
        int len;
        unsigned opcode, subevent, status, known, complete;
    } cases[] = {
        {{0xff,4,0x0c,0x0e,0x09,0x10},6,0,0,0,0,1},
        {{0x3e,3,2,0x0c,0xaa},5,0,2,0,0,1},
        {{0x3e,19,1,0x0c},21,0,1,0x0c,1,1},
        {{0x3e,31,0x0a,0x0c},33,0,0x0a,0x0c,1,1},
        {{0x0f,4,0x0c,1,1,4},6,0x0401,0,0x0c,1,1},
        {{0x0f,4,0x0c,1,1},5,0,0,0,0,0},
        {{0x0e,3,1,0,0},5,0,0,0,0,1},
        {{0x0e,4,1,0,0xfc,0xaa},6,0xfc00,0,0,0,1},
        {{0x05,4,0x0c,0xff,0xff,0xff},6,0,0,0x0c,1,1},
        {{0x30,3,0x0c,0xff,0xff},5,0,0,0x0c,1,1},
        {{0x12,8,0x0c},10,0,0,0x0c,1,1},
        {{0x14,6,0x0c},8,0,0,0x0c,1,1}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        attrib_reset();
        own_ledger *l = own_new(policy, 1);
        CHECK(l != NULL);
        attrib_event_seen(cases[i].event, cases[i].len, 1000);
        attrib_watch(l, "dispatch", cases[i].event, cases[i].len, 1000);
        own_detector_trip(l, 1000);
        attrib_check(l);
        evidence_report r = read_evidence();
        CHECK(r.code == cases[i].event[0] && r.length == (unsigned)cases[i].len);
        CHECK(r.opcode == cases[i].opcode && r.subevent == cases[i].subevent);
        CHECK(r.status == cases[i].status && r.status_known == cases[i].known);
        CHECK(!strcmp(r.session, "starting") && r.nrecent == cases[i].complete);
        own_free(l);
    }
}

static void test_full_table_and_capacity(own_policy policy)
{
    unsigned char opaque[257];
    memset(opaque, 0xff, sizeof opaque);
    opaque[1] = 255;
    attrib_reset();
    CHECK(attrib_evidence_json(evidence_json, sizeof evidence_json) == 4);
    CHECK(!strcmp(evidence_json, "null"));
    for (unsigned i = 0; i < 16; i++) attrib_submit(0xffff - i, 1, 0);
    attrib_resend(0xffff);
    attrib_poll(LONG_MAX);
    CHECK(attrib_get()->timeout == 16 && attrib_get()->unanswered == 16);
    for (unsigned i = 0; i < 4; i++) attrib_event_seen(opaque, sizeof opaque, 0);
    own_ledger *l = own_new(policy, 1);
    CHECK(l != NULL);
    attrib_host_context(1, 1, 1);
    attrib_watch(l, "counter", NULL, 0, LONG_MAX);
    own_detector_trip(l, (uint64_t)LONG_MAX);
    attrib_check(l);
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "counter") && !strcmp(r.session, "starting"));
    CHECK(r.npending == 16 && r.nrecent == 4);
    CHECK(r.timestamp == LONG_MAX && r.since_ready == LONG_MAX - 1 && r.inquiry && r.scan);
    for (unsigned i = 0; i < 16; i++)
        CHECK(r.pending[i].opcode == 0xffff - i && r.pending[i].age == LONG_MAX);
    for (unsigned i = 0; i < 4; i++)
        CHECK(r.recent[i].code == 0xff && r.recent[i].length == 257 && r.recent[i].age == LONG_MAX);
    size_t required = strlen(evidence_json) + 1;
    CHECK(required <= ATTRIB_EVIDENCE_MAX);
    for (size_t cap = 0; cap < required; cap++) {
        unsigned char guarded[ATTRIB_EVIDENCE_MAX + 1];
        memset(guarded, 0xaa, sizeof guarded);
        CHECK(attrib_evidence_json((char *)guarded, cap) == 0);
        CHECK(guarded[cap] == 0xaa);
        if (cap) CHECK(guarded[0] == 0);
        else CHECK(guarded[0] == 0xaa);
    }
    CHECK(attrib_evidence_json(evidence_json, required) == required - 1);
    own_free(l);
    attrib_reset();
    CHECK(attrib_evidence_json(evidence_json, 5) == 4 && !strcmp(evidence_json, "null"));
    CHECK(attrib_evidence_json(evidence_json, 4) == 0 && !evidence_json[0]);
}

/* The first ambiguity of a try may arrive while Stop is already cleaning up. The
 * session keeps its first reason, so the ledger's own ambiguity count decides. */
static void test_first_ambiguity_during_cleanup(own_policy policy)
{
    static const unsigned char foreign[] = {0x0e,4,1,1,0x10,0x0c};
    own_summary summary;
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    CHECK(own_release(l, SESSION_STOP, (uint64_t)clock_ms));
    receive_event(h, foreign, sizeof foreign);
    own_snapshot(l, &summary);
    CHECK(summary.runtime.reason == SESSION_STOP && summary.ambiguity_count == 1);
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "event") && !strcmp(r.session, "quiescing"));
    CHECK(r.code == OWN_CC && r.opcode == 0x1001 && r.status == 0x0c && r.status_known);
    closed_once(h); own_free(l);
}

/* A deadline that trips where no watch ran must not freeze an earlier, innocent
 * event as the cause: the report says it was not observed, with the live state. */
static void test_unwatched_deadline_is_not_misattributed(own_policy policy)
{
    static const unsigned char innocent[] = {0xff,1,0xaa};
    own_token token;
    own_summary summary;
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    clock_ms = 1100;
    receive_event(h, innocent, sizeof innocent);   /* watched and checked: no transition */
    clock_ms = 3600;                               /* past the 2 s acknowledgement wait */
    own_tick(l, (uint64_t)clock_ms);               /* a tick outside any watch, as ACL input does */
    own_snapshot(l, &summary);
    CHECK(summary.runtime.reason == SESSION_AMBIGUOUS && summary.reply_timeout);
    h.ops->pump(h.ctx, 0);                         /* the next instrumented boundary */
    evidence_report r = read_evidence();
    CHECK(!strcmp(r.source, "unobserved"));
    CHECK(!r.code && !r.length && !r.opcode && !r.status_known);
    CHECK(r.timestamp == 3600 && r.npending == 1 && r.pending[0].opcode == 0x1009);
    CHECK(r.pending[0].age == 2600);
    CHECK(r.nrecent == 1 && r.recent[0].code == 0xff && r.recent[0].age == 2500);
    closed_once(h); own_free(l);
}

int main(void)
{
    for (unsigned policy = OWN_POLICY_NO_RESTORE; policy <= OWN_POLICY_SINGLE_PRODUCER; policy++) {
        test_first_event_and_retention((own_policy)policy);
        test_pending_before_deadline((own_policy)policy);
        test_lost_ring_event((own_policy)policy);
        test_stream_gap_precedes_new_authority((own_policy)policy);
        test_explicit_usb_detectors((own_policy)policy);
        test_status_layouts((own_policy)policy);
        test_full_table_and_capacity((own_policy)policy);
        test_first_ambiguity_during_cleanup((own_policy)policy);
        test_unwatched_deadline_is_not_misattributed((own_policy)policy);
    }
    ph_clock_hook = NULL;
    printf("ambiguity evidence: checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
