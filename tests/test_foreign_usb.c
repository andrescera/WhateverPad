/* GPL-3.0-or-later. The two console deaths of build e436fb9159de, through the real USB
 * parser and ledger: a foreign Command Complete for Inquiry Cancel, and a torn Extended
 * Inquiry Result, each with nothing pending. Each stays fatal when something is. */
#define main usb_fixture_main
#include "test_hci_usb.c"
#undef main
#include "../src/log.h"

static char evidence[ATTRIB_EVIDENCE_MAX];

/* Command Complete, 6 bytes, Inquiry Cancel, status 0: the recorded event. */
static const unsigned char cancel_complete[] = {0x0e, 4, 1, 0x02, 0x04, 0x00};

/* What the host's counter check adds up; a rise ends the session. */
static unsigned long counter_sum(void)
{
    const attrib_counts *c = attrib_get();
    unsigned long n = c->overflow + c->timeout + c->untracked;
    for (unsigned i = ATTRIB_DUPLICATE; i < ATTRIB_CLASS_COUNT; i++) n += c->classes[i];
    return n;
}

static int no_evidence(void)
{
    return attrib_evidence_json(evidence, sizeof evidence) == 4 && !strcmp(evidence, "null");
}

static int has(const char *text)
{
    attrib_evidence_json(evidence, sizeof evidence);
    return strstr(evidence, text) != NULL;
}

static unsigned lines_with(unsigned long from, const char *needle)
{
    char logs[8192];
    unsigned n = 0;
    log_since(from, logs, sizeof logs);
    for (const char *p = logs; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

/* One 64-byte (or shorter) piece on the first event reader, as the console delivers. */
static int feed(hci_t h, const unsigned char *piece, int n)
{
    h.ops->pump(h.ctx, 0);              /* re-arm the consumed reader */
    completion(0, piece, n);
    return h.ops->pump(h.ctx, 0);
}

static void eir_event(unsigned char out[257], unsigned seed)
{
    out[0] = 0x2f; out[1] = 255; out[2] = 1;       /* Extended Inquiry Result, one response */
    for (unsigned i = 3; i < 257; i++) out[i] = (unsigned char)(seed + i);
}

static void drain(hci_t h)
{
    unsigned char out[HCI_PKT_MAX];
    while (h.ops->next_event(h.ctx, out, sizeof out) > 0) {}
}

/* n whole Extended Inquiry Results, 64 bytes at a time, each handed to the host intact. */
static void discover(hci_t h, unsigned n)
{
    unsigned char event[257], out[HCI_PKT_MAX];
    for (unsigned i = 0; i < n; i++) {
        eir_event(event, i);
        for (int off = 0; off < 257; off += 64) feed(h, event + off, 257 - off < 64 ? 257 - off : 64);
        CHECK(h.ops->next_event(h.ctx, out, sizeof out) == 257 && !memcmp(out, event, 257));
    }
}

/* Our Inquiry is running: sent, answered, waiting only for its Complete. Nothing pending. */
static own_ledger *inquiring(hci_t *h, own_policy policy)
{
    static const unsigned char inquiry[] = {0x33, 0x8b, 0x9e, 8, 0};
    own_token token;
    reset(); attrib_reset();
    *h = start();
    own_ledger *l = bound(h, policy);
    attrib_host_context(clock_ms - 4600, 1, 1);
    prepare(l, 0x0401, inquiry, sizeof inquiry, &token);
    CHECK(hci_usb_submit(h, &token) == OWN_SENT);
    acknowledge(*h, 0x0401, 1, 0);
    CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED && attrib_get()->unanswered == 0);
    return l;
}

static void still_working(hci_t h, own_ledger *l)
{
    own_token token;
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    acknowledge(h, 0x1009, 0, 0);
    CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED && attrib_get()->unanswered == 0);
}

static int alive(own_ledger *l)
{
    own_summary s;
    own_snapshot(l, &s);
    return s.ambiguity_count == 0 && s.runtime.reason == SESSION_NONE &&
           s.runtime.state == SESSION_ACTIVE;
}

static int dead(own_ledger *l)
{
    own_summary s;
    own_snapshot(l, &s);
    return s.ambiguity_count == 1 && s.runtime.reason == SESSION_AMBIGUOUS;
}

static void foreign_complete_is_ignored(own_policy policy)
{
    unsigned char out[HCI_PKT_MAX];
    hci_t h;
    own_ledger *l = inquiring(&h, policy);
    unsigned long from = log_seq();
    h.ops->pump(h.ctx, 0);
    completion(0, cancel_complete, sizeof cancel_complete);
    h.ops->pump(h.ctx, 0);
    CHECK(h.ops->next_event(h.ctx, out, sizeof out) == 0);   /* the host never sees it */
    CHECK(test_usb_receipts.last == OWN_RECEIPT_FOREIGN);
    CHECK(alive(l) && no_evidence());
    CHECK(counter_sum() == 0);
    CHECK(lines_with(from, "foreign reply 0x0402") == 1 && lines_with(from, "ignored") == 1);
    still_working(h, l);                                       /* and our own work goes on */
    CHECK(alive(l));
    closed_once(h); own_free(l);
}

static void foreign_complete_with_a_pending_command_is_fatal(own_policy policy)
{
    static const unsigned char inquiry[] = {0x33, 0x8b, 0x9e, 8, 0};
    own_token token;
    hci_t h;
    reset(); attrib_reset();
    h = start();
    own_ledger *l = bound(&h, policy);
    prepare(l, 0x0401, inquiry, sizeof inquiry, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);            /* sent, not yet answered */
    h.ops->pump(h.ctx, 0);
    completion(0, cancel_complete, sizeof cancel_complete);
    h.ops->pump(h.ctx, 0);
    CHECK(test_usb_receipts.last == OWN_RECEIPT_NONE && dead(l));
    CHECK(has("\"source\":\"event\"") && has("\"code\":14") && has("\"opcode\":1026"));
    CHECK(has("\"pending\":[{\"opcode\":1025,"));
    closed_once(h); own_free(l);
}

static void other_unmatched_replies_are_still_fatal(own_policy policy)
{
    static const unsigned char other[][6] = {
        {0x0e, 4, 1, 0x08, 0x04, 0},    /* Create Connection Cancel: not listed */
        {0x0e, 4, 1, 0x24, 0x0c, 0},    /* Write Class of Device: not listed */
        {0x0e, 4, 1, 0x01, 0x10, 0},    /* Read Local Version: a reply we could have sent */
        {0x0f, 4, 0, 1, 0x01, 0x04},    /* Inquiry Status, nothing sent: could be a duplicate */
    };
    for (unsigned i = 0; i < sizeof other / sizeof *other; i++) {
        hci_t h;
        own_ledger *l = inquiring(&h, policy);
        h.ops->pump(h.ctx, 0);
        completion(0, other[i], sizeof other[i]);
        h.ops->pump(h.ctx, 0);
        CHECK(test_usb_receipts.last == OWN_RECEIPT_NONE && dead(l));
        closed_once(h); own_free(l);
    }
}

/* The recorded sequence: whole Extended Inquiry Results, then one cut at 64 bytes. */
static void torn_discovery_event_is_dropped(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    unsigned char event[257], out[HCI_PKT_MAX];
    hci_t h;
    own_ledger *l = inquiring(&h, policy);
    unsigned long from;
    discover(h, 2);
    eir_event(event, 9);
    CHECK(feed(h, event, 64) == 0);                /* the other 193 bytes went to another reader */
    clock_ms += 101;
    from = log_seq();
    CHECK(feed(h, complete, sizeof complete) == 1);
    CHECK(h.ops->next_event(h.ctx, out, sizeof out) == sizeof complete);
    CHECK(!memcmp(out, complete, sizeof complete));
    CHECK(alive(l) && no_evidence() && attrib_get()->overflow == 0);
    CHECK(lines_with(from, "torn event dropped") == 1);
    CHECK(lines_with(from, "code=47 held=64 declared=257") == 1);
    discover(h, 1);                                /* the stream is aligned again */
    still_working(h, l);
    CHECK(alive(l));
    closed_once(h); own_free(l);
}

/* The third death: the cut event began mid-way, so its first byte is not an event code. */
static void misaligned_partial_is_dropped(own_policy policy)
{
    unsigned char piece[37] = {0x00, 0xf0};
    hci_t h;
    own_ledger *l = inquiring(&h, policy);
    unsigned long from = log_seq();
    discover(h, 1);
    CHECK(feed(h, piece, sizeof piece) == 0);
    clock_ms += 2350;
    discover(h, 1);
    CHECK(alive(l) && no_evidence() && attrib_get()->overflow == 0);
    CHECK(lines_with(from, "code=0 held=37 declared=242") == 1);
    closed_once(h); own_free(l);
}

static void repeated_gaps_are_logged_sparsely(own_policy policy)
{
    unsigned char event[257];
    hci_t h;
    own_ledger *l = inquiring(&h, policy);
    unsigned long from = log_seq();
    for (unsigned i = 0; i < 3; i++) {
        discover(h, 1);
        eir_event(event, i);
        feed(h, event, 64);
        clock_ms += 101;
    }
    discover(h, 1);
    CHECK(alive(l) && no_evidence());
    CHECK(lines_with(from, "torn event dropped") == 2);   /* the 1st and 2nd; the 3rd is not logged */
    closed_once(h); own_free(l);
}

/* A reply may be inside the lost bytes: nothing is ignored while one is awaited. */
static void torn_event_with_a_pending_command_is_fatal(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    for (unsigned order = 0; order < 2; order++) {
        unsigned char event[257];
        own_token token;
        hci_t h;
        own_ledger *l = inquiring(&h, policy);
        discover(h, 2);
        eir_event(event, 9);
        if (order == 0) {                          /* sent before the cut event began */
            prepare(l, 0x1009, NULL, 0, &token);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
            feed(h, event, 64);
        } else {                                   /* sent while it is cut */
            feed(h, event, 64);
            prepare(l, 0x1009, NULL, 0, &token);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
        }
        clock_ms += 101;
        feed(h, complete, sizeof complete);
        CHECK(dead(l));
        CHECK(has("\"source\":\"stream_gap\"") && has("\"code\":47") && has("\"length\":64"));
        CHECK(has("\"pending\":[{\"opcode\":4105,"));
        closed_once(h); own_free(l);
    }
}

/* Outside discovery traffic, or with a link held, a lost piece may be anything. */
static void torn_event_outside_discovery_is_fatal(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    static const unsigned char unrelated[] = {0xff, 20, 'x', 'y', 'z'};
    hci_t h;
    own_ledger *l;
    own_ref parent;

    reset(); attrib_reset();                       /* no discovery traffic at all */
    h = start();
    l = bound(&h, policy);
    CHECK(feed(h, unrelated, sizeof unrelated) == 0);
    clock_ms += 101;
    feed(h, complete, sizeof complete);
    CHECK(dead(l));
    closed_once(h); own_free(l);

    reset(); attrib_reset();                       /* traffic, but long ago */
    h = start();
    l = bound(&h, policy);
    discover(h, 1);
    clock_ms += 13000;
    CHECK(feed(h, unrelated, sizeof unrelated) == 0);
    clock_ms += 101;
    feed(h, complete, sizeof complete);
    CHECK(dead(l));
    closed_once(h); own_free(l);

    reset(); attrib_reset();                       /* a link is held: its disconnect may be lost */
    h = start();
    l = bound(&h, policy);
    acquire_link(h, l, &parent);
    discover(h, 1);
    {
        unsigned char event[257];
        eir_event(event, 3);
        feed(h, event, 64);
    }
    clock_ms += 101;
    feed(h, complete, sizeof complete);
    CHECK(dead(l));
    closed_once(h); own_free(l);
}

/* A command answered in whole events before the cut does not make the cut harmful. */
static void torn_event_after_an_ordinary_command_is_dropped(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    unsigned char event[257];
    hci_t h;
    own_ledger *l = inquiring(&h, policy);
    discover(h, 1);
    still_working(h, l);
    discover(h, 1);
    eir_event(event, 5);
    CHECK(feed(h, event, 64) == 0);
    clock_ms += 101;
    CHECK(feed(h, complete, sizeof complete) == 1);
    CHECK(alive(l) && no_evidence());
    closed_once(h); own_free(l);
}

/* Review finding: a restorable setting is held (SINGLE_PRODUCER only). A torn Command
 * Complete, begun right after discovery traffic, could be an unmatched reply that would
 * have revoked it; dropping it would leave the lease standing with nothing to expire it. */
static void torn_event_with_a_retained_lease_is_fatal(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    static const unsigned char torn_reply[] = {0x0e, 4, 1};
    unsigned char scan = 1;
    own_token token;
    hci_t h;
    own_ledger *l;
    reset(); attrib_reset();
    h = start();
    l = bound(&h, policy);
    prepare(l, 0x0c1a, &scan, 1, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    acknowledge(h, 0x0c1a, 0, 0);
    fence(h, l, 1);
    discover(h, 1);
    CHECK(feed(h, torn_reply, sizeof torn_reply) == 0);
    clock_ms += 101;
    feed(h, complete, sizeof complete);
    if (policy == OWN_POLICY_SINGLE_PRODUCER) CHECK(dead(l));
    else CHECK(alive(l) && no_evidence());   /* no restore authority is ever held */
    closed_once(h); own_free(l);
}

/* Review finding: bytes of later events can complete the stuck half event, so a reply
 * could be taken or mimicked while the stream is misframed. A command sent after the last
 * event boundary must keep the gap fatal even if its waiter is gone by the time it shows. */
static void torn_event_around_a_command_is_fatal_even_when_it_looks_answered(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    static const unsigned char reply[] = {0x0e, 4, 1, 9, 0x10, 0};   /* what 0x1009 is answered with */
    for (unsigned variant = 0; variant < 3; variant++) {
        unsigned char event[257], tail[64], piece[64];
        own_token token;
        hci_t h;
        own_ledger *l = inquiring(&h, policy);
        discover(h, 2);
        eir_event(event, 9);
        memset(tail, 0x5a, sizeof tail);
        if (variant == 0) {
            /* Sent while the event is cut; the cut event is then completed by later
             * bytes, which also hold a reply-shaped string and the start of the next cut. */
            CHECK(feed(h, event, 64) == 0);
            prepare(l, 0x1009, NULL, 0, &token);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
            for (unsigned i = 0; i < 3; i++) feed(h, tail, sizeof tail);   /* 192 of the 193 */
            piece[0] = 0x5a;
            memcpy(piece + 1, reply, sizeof reply);
            eir_event(event, 3);
            memcpy(piece + 1 + sizeof reply, event, 57);
            feed(h, piece, sizeof piece);
            CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED);       /* the waiter is gone */
        } else if (variant == 2) {
            /* The review's last ordering, in separate pieces: sent while the event is cut,
             * which later bytes complete exactly at a piece end, then a piece of its own
             * with the reply-shaped string (the stream ends on a boundary again), then the
             * next cut. Nothing at the parser tells the reply from a real one; what is
             * known is that a command overlapped an open half event. */
            CHECK(feed(h, event, 64) == 0);
            prepare(l, 0x1009, NULL, 0, &token);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
            for (unsigned i = 0; i < 3; i++) feed(h, tail, sizeof tail);
            feed(h, tail, 1);
            feed(h, reply, sizeof reply);
            CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED);
            eir_event(event, 6);
            feed(h, event, 64);
        } else {
            /* Sent before an event boundary at which it was still awaited; its reply then
             * arrives in the same piece that opens the cut event. */
            prepare(l, 0x1009, NULL, 0, &token);
            CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
            discover(h, 1);
            memcpy(piece, reply, sizeof reply);
            eir_event(event, 4);
            memcpy(piece + sizeof reply, event, 58);
            feed(h, piece, sizeof piece);
            CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED);
        }
        CHECK(alive(l));                          /* nothing has ended the session yet */
        clock_ms += 101;
        feed(h, complete, sizeof complete);
        CHECK(dead(l));
        CHECK(has("\"source\":\"stream_gap\"") && has("\"pending\":[]"));
        closed_once(h); own_free(l);
    }
}

/* A command sent into a half event that the next piece completes exactly: the stream is on
 * a boundary again before and after the reply, yet the command overlapped open framing. */
static void command_sent_into_a_short_half_event_is_latched(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    static const unsigned char reply[] = {0x0e, 4, 1, 9, 0x10, 0};
    unsigned char event[257], result[17] = {0x22, 15, 1};   /* Inquiry Result with RSSI */
    own_token token;
    hci_t h;
    own_ledger *l = inquiring(&h, policy);
    discover(h, 1);
    CHECK(feed(h, result, 8) == 0);                         /* a half event is open */
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);          /* a command goes out */
    CHECK(feed(h, result + 8, 9) == 1);                     /* it completes exactly */
    feed(h, reply, sizeof reply);                           /* and the reply, in its own piece */
    CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED && alive(l));
    drain(h);
    eir_event(event, 7);
    feed(h, event, 64);
    clock_ms += 101;
    feed(h, complete, sizeof complete);
    CHECK(dead(l));
    closed_once(h); own_free(l);
}

/* The overlap of a command with an open half event is remembered, not inferred from the
 * latest boundary: it keeps every later cut fatal until the transport is reopened. */
static void command_overlap_is_sticky_until_reopen(own_policy policy)
{
    static const unsigned char complete[] = {0x01, 1, 0};
    static const unsigned char reply[] = {0x0e, 4, 1, 9, 0x10, 0};
    unsigned char event[257], tail[64];
    own_token token;
    hci_t h;
    own_ledger *l = inquiring(&h, policy);
    unsigned long from = log_seq();
    memset(tail, 0x5a, sizeof tail);
    eir_event(event, 1);
    CHECK(feed(h, event, 64) == 0);
    prepare(l, 0x1009, NULL, 0, &token);
    CHECK(hci_usb_submit(&h, &token) == OWN_SENT);
    for (unsigned i = 0; i < 3; i++) feed(h, tail, sizeof tail);
    feed(h, tail, 1);
    feed(h, reply, sizeof reply);
    CHECK(test_usb_receipts.last == OWN_RECEIPT_SEQUENCED && alive(l));
    CHECK(lines_with(from, "overlapped an open half event") == 1);   /* said once, not per piece */
    drain(h);                                   /* the completed half event and the reply */
    discover(h, 1);
    eir_event(event, 2);
    feed(h, event, 64);
    clock_ms += 101;
    feed(h, complete, sizeof complete);
    CHECK(dead(l));
    closed_once(h); own_free(l);

    l = inquiring(&h, policy);                  /* a fresh transport starts clean */
    discover(h, 1);
    eir_event(event, 3);
    feed(h, event, 64);
    clock_ms += 101;
    feed(h, complete, sizeof complete);
    CHECK(alive(l) && no_evidence());
    closed_once(h); own_free(l);
}

int main(void)
{
    for (unsigned policy = OWN_POLICY_NO_RESTORE; policy <= OWN_POLICY_SINGLE_PRODUCER; policy++) {
        foreign_complete_is_ignored((own_policy)policy);
        foreign_complete_with_a_pending_command_is_fatal((own_policy)policy);
        other_unmatched_replies_are_still_fatal((own_policy)policy);
        torn_discovery_event_is_dropped((own_policy)policy);
        misaligned_partial_is_dropped((own_policy)policy);
        repeated_gaps_are_logged_sparsely((own_policy)policy);
        torn_event_with_a_pending_command_is_fatal((own_policy)policy);
        torn_event_outside_discovery_is_fatal((own_policy)policy);
        torn_event_after_an_ordinary_command_is_dropped((own_policy)policy);
        torn_event_with_a_retained_lease_is_fatal((own_policy)policy);
        torn_event_around_a_command_is_fatal_even_when_it_looks_answered((own_policy)policy);
        command_sent_into_a_short_half_event_is_latched((own_policy)policy);
        command_overlap_is_sticky_until_reopen((own_policy)policy);
    }
    ph_clock_hook = NULL;
    printf("foreign USB: checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
