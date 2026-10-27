/* GPL-3.0-or-later. A reply from another HCI owner, with nothing of ours pending,
 * grants nothing and costs nothing; every other unmatched reply stays fatal. */
#include "../src/ownership.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
static own_policy policy_now;
static uint64_t now = 1000;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL foreign policy=%d line=%d: %s\n", (int)policy_now, __LINE__, #x); \
    failures++; \
} } while (0)

typedef struct { unsigned opcode; own_ack_event event; unsigned returned; } known_reply;
/* What the system stack answers for discovery housekeeping: Inquiry Cancel, Remote Name
 * Request Cancel (status + address) and Remote Name Request. We cannot send any of them. */
static const known_reply known[] = {
    {0x0402, OWN_CC, 0}, {0x041a, OWN_CC, 6}, {0x0419, OWN_CS, 0},
};

static own_ledger *fresh(void)
{
    own_ledger *l = own_new(policy_now, 1);
    CHECK(l != NULL && own_activate(l));
    return l;
}

/* A command committed and sent, still waiting for its answer. */
static own_ref send(own_ledger *l, unsigned opcode)
{
    own_command c = {.opcode = (uint16_t)opcode, .ack_by = now + 2000, .terminal_by = now + 20000};
    own_ref ref = {0};
    own_token token;
    own_effect effect;
    uint64_t deadline;
    if (opcode == 0x0401) {
        memcpy(c.params, "\x33\x8b\x9e\x08\x00", 5);
        c.plen = 5;
    }
    CHECK(own_prepare(l, &c, &ref) == OWN_OK);
    CHECK(own_next_hci(l, now, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, now, &effect, &deadline) == OWN_OK);
    own_submitted(l, &token, OWN_SENT, now);
    return ref;
}

static own_provenance deliver(own_ledger *l, const own_ack *a)
{
    own_receipt receipt;
    now += 10;
    own_provenance p = own_backend_receipt(l, a, now, &receipt);
    own_receive(l, &receipt, now);
    return p;
}

static own_provenance wire(own_ledger *l, own_ack_event event, unsigned opcode,
                           unsigned status, unsigned returned)
{
    own_ack a = {.event = event, .opcode = (uint16_t)opcode, .status = (uint8_t)status,
                 .returned = (uint8_t)returned};
    return deliver(l, &a);
}

static int fatal(const own_ledger *l)
{
    own_summary s;
    own_snapshot(l, &s);
    return s.ambiguity_count == 1 && s.runtime.reason == SESSION_AMBIGUOUS;
}

static int healthy(const own_ledger *l)
{
    own_summary s;
    own_snapshot(l, &s);
    return s.ambiguity_count == 0 && s.runtime.reason == SESSION_NONE &&
           s.runtime.state == SESSION_ACTIVE && !s.runtime.unsafe && s.quarantined == 0 &&
           own_invariant(l) == NULL;
}

/* Healthy, and the foreign reply added no record, link or credit. */
static int untouched(const own_ledger *l, const own_summary *before)
{
    own_summary s;
    own_snapshot(l, &s);
    return healthy(l) && s.records == before->records && s.links == before->links &&
           s.credit_nops == before->credit_nops;
}

static void foreign_reply_is_ignored(void)
{
    for (unsigned k = 0; k < sizeof known / sizeof *known; k++)
        for (unsigned status = 0; status <= 0x0c; status += 0x0c) {
            own_ledger *l = fresh();
            own_summary before;
            own_token token;
            own_snapshot(l, &before);
            CHECK(wire(l, known[k].event, known[k].opcode, status, known[k].returned)
                  == OWN_RECEIPT_FOREIGN);
            CHECK(untouched(l, &before) && own_quiet(l));
            CHECK(own_next_hci(l, now, &token) != OWN_OK); /* it queued and completed nothing */
            /* Ordinary work still runs, and its own answer is still matched. */
            send(l, 0x1009);
            CHECK(wire(l, OWN_CC, 0x1009, 0, 0) == OWN_RECEIPT_SEQUENCED);
            CHECK(healthy(l));
            own_free(l);
        }
}

/* The console case: an Inquiry of ours is running (answered, waiting for its Complete)
 * and the system's Inquiry Cancel Complete reaches us with nothing pending. */
static void foreign_reply_during_our_inquiry(void)
{
    own_ledger *l = fresh();
    own_summary before;
    own_state state;
    own_token token;
    own_terminal_event complete = {.kind = OWN_TERM_INQUIRY};
    own_ref inquiry = send(l, 0x0401);
    CHECK(wire(l, OWN_CS, 0x0401, 0, 0) == OWN_RECEIPT_SEQUENCED);
    CHECK(own_status(l, inquiry, &state) == OWN_OK && state == OWN_ACKNOWLEDGED);
    own_snapshot(l, &before);
    CHECK(own_quiet(l)); /* its terminal has a deadline of its own */
    for (unsigned k = 0; k < sizeof known / sizeof *known; k++)
        CHECK(wire(l, known[k].event, known[k].opcode, 0, known[k].returned)
              == OWN_RECEIPT_FOREIGN);
    CHECK(untouched(l, &before));
    CHECK(own_status(l, inquiry, &state) == OWN_OK && state == OWN_ACKNOWLEDGED);
    CHECK(own_request_undo(l, inquiry, now, &token) == OWN_NO_PROOF); /* no authority */
    own_terminal(l, inquiry, &complete, now + 10);
    CHECK(own_status(l, inquiry, &state) == OWN_OK && state == OWN_CLOSED);
    CHECK(untouched(l, &before));
    own_free(l);
}

/* A reply we are waiting for may be the one confused: still fatal. */
static void pending_command_keeps_it_fatal(void)
{
    static const unsigned waiting[] = {0x0401, 0x1009};
    for (unsigned w = 0; w < 2; w++)
        for (unsigned k = 0; k < sizeof known / sizeof *known; k++) {
            own_ledger *l = fresh();
            send(l, waiting[w]);
            CHECK(wire(l, known[k].event, known[k].opcode, 0, known[k].returned)
                  == OWN_RECEIPT_NONE);
            CHECK(fatal(l) && !own_quiet(l));
            own_free(l);
        }
}

/* A command we could send is not a foreign one; the three above can never be ours. */
static void foreign_opcodes_cannot_be_ours(void)
{
    for (unsigned k = 0; k < sizeof known / sizeof *known; k++) {
        own_ledger *l = fresh();
        own_command c = {.opcode = (uint16_t)known[k].opcode, .plen = 6,
                         .ack_by = now + 2000, .terminal_by = now + 4000};
        own_ref ref;
        own_token token;
        CHECK(own_prepare(l, &c, &ref) != OWN_OK);
        CHECK(own_next_hci(l, now, &token) != OWN_OK);
        own_free(l);
    }
}

static void everything_else_stays_fatal(void)
{
    static const struct { own_ack_event event; unsigned opcode, status, returned; } other[] = {
        {OWN_CC, 0x0402, 0, 1}, {OWN_CC, 0x0402, 0, 4},     /* wrong size */
        {OWN_CC, 0x041a, 0, 0}, {OWN_CC, 0x041a, 0, 5}, {OWN_CC, 0x041a, 0, 7},
        {OWN_CS, 0x0402, 0, 0}, {OWN_CC, 0x0419, 0, 0}, {OWN_CS, 0x041a, 0, 0}, /* wrong event */
        {OWN_CC, 0x0408, 0, 0}, {OWN_CS, 0x0408, 0, 0},    /* other housekeeping, not listed */
        {OWN_CC, 0x0c24, 0, 0}, {OWN_CC, 0x0c13, 0, 0}, {OWN_CS, 0x041b, 0, 0},
        {OWN_CC, 0x0001, 0, 0}, {OWN_CC, 0xfc00, 0, 0}, {OWN_CS, 0xfc01, 0, 0},
        {OWN_CC, 0x1001, 0, 0}, {OWN_CC, 0x1009, 0x0c, 0},  /* ours: could be a duplicate */
        {OWN_CC, 0x0c1a, 0, 0}, {OWN_CC, 0x2018, 0, 0},
        {OWN_CS, 0x0401, 0, 0}, {OWN_CS, 0x0401, 0x0c, 0}, {OWN_CS, 0x0406, 0, 0},
        {OWN_CS, 0x0405, 0, 0},
    };
    for (unsigned i = 0; i < sizeof other / sizeof *other; i++) {
        own_ledger *l = fresh();
        CHECK(wire(l, other[i].event, other[i].opcode, other[i].status, other[i].returned)
              == OWN_RECEIPT_NONE);
        CHECK(fatal(l));
        own_free(l);
    }
    for (unsigned flag = 0; flag < 2; flag++)
        for (unsigned k = 0; k < sizeof known / sizeof *known; k++) {
            own_ledger *l = fresh();
            own_ack a = {.event = known[k].event, .opcode = (uint16_t)known[k].opcode,
                         .returned = (uint8_t)known[k].returned,
                         .malformed = (uint8_t)(flag == 0), .overflow = (uint8_t)(flag == 1)};
            CHECK(deliver(l, &a) == OWN_RECEIPT_NONE);
            CHECK(fatal(l));
            own_free(l);
        }
}

static void duplicate_of_our_own_answer_stays_fatal(void)
{
    own_ledger *l = fresh();
    send(l, 0x1009);
    CHECK(wire(l, OWN_CC, 0x1009, 0, 0) == OWN_RECEIPT_SEQUENCED);
    CHECK(wire(l, OWN_CC, 0x1009, 0, 0) == OWN_RECEIPT_NONE);
    CHECK(fatal(l));
    own_free(l);
}

static void quiet_means_nothing_could_change(void)
{
    own_ledger *l = fresh();
    own_peer peer = {.wire = {1, 2, 3, 4, 5, 6}, .identity = {1, 2, 3, 4, 5, 6}};
    link_ref link;
    own_command prepared = {.opcode = 0x1009, .ack_by = now + 2000, .terminal_by = now + 4000};
    own_ref ref;
    CHECK(own_quiet(l));
    CHECK(own_prepare(l, &prepared, &ref) == OWN_OK);
    CHECK(own_quiet(l)); /* prepared, not sent: no reply can be lost */
    own_free(l);

    l = fresh();
    send(l, 0x1009);
    CHECK(!own_quiet(l) && own_reply_awaited(l)); /* a reply is awaited */
    CHECK(wire(l, OWN_CC, 0x1009, 0, 0) == OWN_RECEIPT_SEQUENCED);
    CHECK(own_quiet(l) && !own_reply_awaited(l));
    own_free(l);

    l = fresh();
    CHECK(own_reserve_link(l, &peer, &link) == OWN_OK);
    CHECK(!own_quiet(l)); /* a link whose Disconnection Complete matters */
    own_free(l);

    l = fresh();
    CHECK(own_release(l, SESSION_STOP, now));
    CHECK(!own_quiet(l)); /* no longer admitting */
    own_free(l);

    l = fresh();
    own_detector_trip(l, now);
    CHECK(!own_quiet(l));
    own_free(l);

    /* A restorable setting is authority that an unmatched reply about it would revoke;
     * a lost one would leave it standing. Only SINGLE_PRODUCER retains such a lease. */
    l = fresh();
    {
        own_command setting = {.opcode = 0x0c1a, .plen = 1, .params = {2}, .original_known = 1,
                               .original_len = 1, .original = {1},
                               .ack_by = now + 2000, .terminal_by = now + 4000};
        own_ack barrier = {.event = OWN_CC, .opcode = 0x2018, .random_len = 8,
                           .random = {1, 2, 3, 4, 5, 6, 7, 8}};
        own_token token;
        own_effect effect;
        uint64_t deadline;
        CHECK(own_prepare(l, &setting, &ref) == OWN_OK);
        CHECK(own_next_hci(l, now, &token) == OWN_OK);
        CHECK(own_commit_hci(l, &token, now, &effect, &deadline) == OWN_OK);
        own_submitted(l, &token, OWN_SENT, now);
        CHECK(wire(l, OWN_CC, 0x0c1a, 0, 0) == OWN_RECEIPT_SEQUENCED);
        CHECK(own_next_hci(l, now, &token) == OWN_OK);             /* the LE Rand fence */
        CHECK(own_commit_hci(l, &token, now, &effect, &deadline) == OWN_OK);
        own_submitted(l, &token, OWN_SENT, now);
        CHECK(deliver(l, &barrier) == OWN_RECEIPT_SEQUENCED);
        if (policy_now == OWN_POLICY_SINGLE_PRODUCER) {
            CHECK(own_request_undo(l, ref, now, &token) == OWN_OK); /* the lease is real */
            CHECK(!own_quiet(l));
        } else {
            CHECK(own_request_undo(l, ref, now, &token) == OWN_NO_PROOF);
            CHECK(own_quiet(l));
        }
    }
    own_free(l);
}

int main(void)
{
    for (unsigned p = OWN_POLICY_NO_RESTORE; p <= OWN_POLICY_SINGLE_PRODUCER; p++) {
        policy_now = (own_policy)p;
        foreign_reply_is_ignored();
        foreign_reply_during_our_inquiry();
        pending_command_keeps_it_fatal();
        foreign_opcodes_cannot_be_ours();
        everything_else_stays_fatal();
        duplicate_of_our_own_answer_stays_fatal();
        quiet_means_nothing_could_change();
    }
    printf("foreign replies: checks=%u failures=%u\n", checks, failures);
    return failures != 0;
}
