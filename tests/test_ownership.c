/* Effect-level fixtures for DD 2/6, A4/A8, and the wire-indistinguishability case. */
#include "../src/ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { \
    printf("FAIL ownership:%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static own_peer peer(unsigned n)
{
    own_peer p = {0};
    p.adapter[0] = 7;
    p.identity[0] = p.wire[0] = (uint8_t)n;
    return p;
}

static own_command command(unsigned op, unsigned value)
{
    own_command c = {0};
    c.opcode = (uint16_t)op;
    c.ack_by = 5000;
    c.terminal_by = 8000;
    c.plen = op == 0x200c ? 2 : op == 0x0c01 ? 8 : op == 0x0405 ? 13 : 0;
    c.params[0] = (uint8_t)value;
    c.original_known = 1;
    c.original_len = c.plen;
    return c;
}

static own_ref prepare(own_ledger *l, unsigned op, unsigned value)
{
    own_ref r = {0};
    own_command c = command(op, value);
    CHECK(own_prepare(l, &c, &r) == OWN_OK);
    return r;
}

static own_token submit(own_ledger *l, unsigned op, uint64_t now)
{
    own_token t = {{0}};
    own_effect e;
    CHECK(own_next_hci(l, now, &t) == OWN_OK);
    CHECK(own_commit(l, &t, now, &e) == OWN_OK);
    CHECK(e.opcode == op);
    own_submitted(l, &t, OWN_SENT, now);
    CHECK(own_commit(l, &t, now, &e) != OWN_OK);
    return t;
}

static own_receipt ack(own_ledger *l, unsigned op, unsigned status, uint64_t random,
                       uint64_t now)
{
    own_ack a = {0};
    own_receipt receipt;
    a.opcode = (uint16_t)op;
    a.event = op == 0x0405 || op == 0x0409 || op == 0x200d || op == 0x0406 ||
              op == 0x2025 || op == 0x2026 || op == 0x0401 || op == 0x0411 ||
              op == 0x0413 || op == 0x2013 || op == 0x2019 ? OWN_CS : OWN_CC;
    a.status = (uint8_t)status;
    if (op == 0x2018) {
        a.random_len = 8;
        for (unsigned i = 0; i < 8; i++) a.random[i] = (uint8_t)(random >> (8*i));
    }
    own_backend_receipt(l, &a, now, &receipt);
    own_receive(l, &receipt, now);
    CHECK(own_invariant(l) == NULL);
    return receipt;
}

static void barrier(own_ledger *l, uint64_t random, uint64_t now)
{
    submit(l, 0x2018, now);
    ack(l, 0x2018, 0, random, now);
}

static void no_undo(own_ledger *l, own_ref r, uint64_t now)
{
    own_token t;
    CHECK(own_request_undo(l, r, now, &t) != OWN_OK);
}

static link_ref connect_link(own_ledger *l, unsigned n, unsigned handle, own_ref *out,
                            uint64_t now)
{
    link_ref link = {0};
    own_command c = command(0x0405, 0);
    c.peer = peer(n);
    memcpy(c.params, c.peer.wire, 6);
    CHECK(own_reserve_link(l, &c.peer, &link) == OWN_OK);
    c.link = link;
    CHECK(own_prepare(l, &c, out) == OWN_OK);
    submit(l, 0x0405, now);
    /* Terminal-before-CS is retained, not published. */
    own_terminal_event term = { .kind = OWN_TERM_CONNECTION, .peer = c.peer,
                               .handle = (uint16_t)handle };
    own_terminal(l, *out, &term, now + 1);
    CHECK(own_input(l, link, (uint16_t)handle, now + 2) != OWN_OK);
    ack(l, 0x0405, 0, 0, now + 3);
    CHECK(own_input(l, link, (uint16_t)handle, now + 4) != OWN_OK);
    barrier(l, n, now + 4);
    CHECK(own_input(l, link, (uint16_t)handle, now + 12) == OWN_OK);
    return link;
}

static void local(own_ledger *l, own_effect_kind kind, uint64_t now, int success)
{
    own_token t, again;
    own_effect e;
    CHECK(own_next_local(l, now, &t) == OWN_OK);
    CHECK(own_commit(l, &t, now, &e) == OWN_OK);
    CHECK(e.kind == kind);
    CHECK(own_commit(l, &t, now, &e) != OWN_OK);
    if (kind == OWN_EFFECT_RETIRE || kind == OWN_EFFECT_DELETE) {
        /* A callback can reenter here, before any result notification. */
        if (own_next_local(l, now, &again) == OWN_OK) {
            own_effect other;
            own_ledger *reentrant = own_clone(l);
            CHECK(own_commit(reentrant, &again, now, &other) != OWN_OK ||
                  other.kind != kind || other.link.generation != e.link.generation ||
                  other.link.slot != e.link.slot);
            own_free(reentrant);
        }
    }
    own_backend_local_result(l, &t, success);
}

static void ready(own_ledger *l, link_ref link, uint64_t now)
{
    local(l, OWN_EFFECT_CONNECT, now + 13, 1);
    local(l, OWN_EFFECT_ADD, now + 14, 1);
    own_backend_identity(l, link, 100 + link.slot, 1);
    local(l, OWN_EFFECT_BIND, now + 15, 1);
    CHECK(own_input(l, link, (uint16_t)(40 + link.slot), now + 16) == OWN_OK);
    local(l, OWN_EFFECT_INPUT, now + 16, 1);
}

static void positive(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_ref r;
    own_token t;
    own_effect e;
    CHECK(l != NULL && own_activate(l));
    link_ref link = connect_link(l, 1, 40, &r, 0);
    ready(l, link, 0);
    CHECK(own_release(l, SESSION_STOP, 20));
    CHECK(own_input(l, link, 40, 20) != OWN_OK);
    local(l, OWN_EFFECT_RETIRE, 20, 1);
    local(l, OWN_EFFECT_DELETE, 20, 1);
    if (policy == OWN_POLICY_SINGLE_PRODUCER) {
        CHECK(own_request_undo(l, r, 20, &t) == OWN_OK);
        CHECK(own_commit(l, &t, 20, &e) == OWN_OK);
        CHECK(e.opcode == 0x0406 && e.plen == 3 && e.params[0] == 40 &&
              e.params[1] == 0 && e.params[2] == 0x13);
        own_submitted(l, &t, OWN_SENT, 20);
        ack(l, 0x0406, 0, 0, 21);
        no_undo(l, r, 21);
        own_terminal_event term = { .kind = OWN_TERM_DISCONNECT, .handle = 40 };
        own_terminal(l, r, &term, 22);
    } else {
        own_summary s;
        no_undo(l, r, 20);
        own_snapshot(l, &s);
        CHECK(s.radio_left == 1);
    }
    own_transport_closed(l, 30);
    local(l, OWN_EFFECT_FINALIZE, 30, 1);
    own_finalized(l, CLEANUP_CONFIRMED);
    own_summary s;
    own_snapshot(l, &s);
    CHECK(s.finalized && s.transport_closed && !own_retry(l));
    CHECK(own_invariant(l) == NULL);
    printf("positive policy=%d usable-input=1 local-cleanup=1 sequenced-only=1\n", policy);
    own_free(l);
}

static void basic(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_ref r = prepare(l, 0x200c, 1);
    own_token t;
    own_effect e;
    CHECK(own_next_hci(l, 0, &t) == OWN_OK);
    CHECK(own_release(l, SESSION_STOP, 1));
    CHECK(own_commit(l, &t, 1, &e) != OWN_OK); /* final boundary, Appendix D */
    no_undo(l, r, 1);
    own_free(l);

    l = own_new(policy, 1);
    r = prepare(l, 0x200c, 1);
    CHECK(own_next_hci(l, 0, &t) == OWN_OK);
    CHECK(own_commit(l, &t, 5000, &e) != OWN_OK);
    CHECK(own_reclaim(l, r) == OWN_OK);
    own_free(l);

    for (int result = OWN_NOT_SENT; result <= OWN_SEND_UNCERTAIN; result++) {
        l = own_new(policy, 1);
        r = prepare(l, 0x200c, 1);
        CHECK(own_next_hci(l, 0, &t) == OWN_OK);
        CHECK(own_commit(l, &t, 0, &e) == OWN_OK);
        own_submitted(l, &t, (own_submit_result)result, 0);
        own_readback(l, 0x200c, e.params, e.plen);
        no_undo(l, r, 1); /* SENT/readback are not a lease */
        if (result == OWN_SENT) {
            ack(l, 0x200c, 0x0c, 0, 1);
            no_undo(l, r, 2);
        }
        CHECK(own_invariant(l) == NULL);
        own_free(l);
    }
    const unsigned forbidden[] = {0x0402, 0x0408, 0x200e, 0x0c03, 0x0c56,
                                  0xffff, 0x0c13, 0x0c24, 0x0c45, 0x0406, 0x2018};
    l = own_new(policy, 1);
    for (unsigned i = 0; i < sizeof forbidden / sizeof *forbidden; i++) {
        own_command c = command(forbidden[i], 0);
        CHECK(own_prepare(l, &c, &r) != OWN_OK);
    }
    own_free(l);

    l = own_new(policy, 1);
    r = prepare(l, 0x200c, 0);
    submit(l, 0x200c, 0);
    own_tick(l, 5000); /* G: a lost ACK cannot be substituted for a later ON */
    CHECK(own_next_hci(l, 5000, &t) != OWN_OK);
    own_command c = command(0x200c, 1);
    own_ref replacement;
    CHECK(own_prepare(l, &c, &replacement) != OWN_OK);
    ack(l, 0x200c, 0, 0, 5001);
    no_undo(l, r, 5002);
    CHECK(own_reclaim(l, r) != OWN_OK);
    own_transport_closed(l, 5003);
    CHECK(own_reclaim(l, r) == OWN_OK);
    own_free(l);

    l = own_new(policy, 1);
    own_ack a = { .event = OWN_CC, .opcode = 0 };
    own_receipt receipt;
    CHECK(own_backend_receipt(l, &a, 0, &receipt) == OWN_RECEIPT_NONE);
    own_receive(l, &receipt, 0);
    own_summary s;
    own_snapshot(l, &s);
    CHECK(s.credit_nops == 1 && s.ambiguity_count == 0);
    a.opcode = 0x1009;
    own_backend_receipt(l, &a, 0, &receipt);
    own_receive(l, &receipt, 0);
    own_snapshot(l, &s);
    CHECK(s.runtime.reason == SESSION_AMBIGUOUS);
    own_free(l);

    l = own_new(policy, 1);
    prepare(l, 0x200c, 1);
    submit(l, 0x200c, 0);
    ack(l, 0, 0, 0, 5000); /* Credits cannot hide an expired command. */
    own_snapshot(l, &s);
    CHECK(s.credit_nops == 1 && s.runtime.reason == SESSION_AMBIGUOUS);
    own_free(l);

    l = own_new(policy, 1);
    CHECK(own_activate(l));
    own_ref parent;
    link_ref input_link = connect_link(l, 1, 40, &parent, 0);
    prepare(l, 0x1009, 0);
    submit(l, 0x1009, 20);
    CHECK(own_input(l, input_link, 40, 5000) == OWN_STOPPED);
    own_snapshot(l, &s);
    CHECK(s.runtime.reason == SESSION_AMBIGUOUS);
    own_free(l);
}

static void collisions(own_policy policy)
{
    const unsigned ops[] = {0x200c, 0x0c01};
    for (unsigned op = 0; op < 2; op++) {
        own_ledger *l = own_new(policy, 1);
        own_ref old = prepare(l, ops[op], 0);
        submit(l, ops[op], 0);
        own_receipt old_receipt = ack(l, ops[op], 0, 0, 1);
        barrier(l, 111, 2);
        own_ref replacement = prepare(l, ops[op], 1);
        submit(l, ops[op], 20);
        /* H: the duplicate old success is indistinguishable at forward ACK time. */
        ack(l, ops[op], 0, 0, 21);
        no_undo(l, replacement, 21);
        submit(l, 0x2018, 22);
        ack(l, ops[op], 0x0c, 0, 23); /* the real ON refusal precedes barrier */
        ack(l, 0x2018, 0, 112, 24);
        CHECK(own_release(l, SESSION_STOP, 25) == 0); /* first ambiguity wins */
        no_undo(l, old, 25);
        no_undo(l, replacement, 25);
        own_receive(l, &old_receipt, 26);
        no_undo(l, replacement, 26);
        own_free(l);

        l = own_new(policy, 1);
        prepare(l, ops[op], 0);
        submit(l, ops[op], 0);
        ack(l, ops[op], 0, 0, 1);
        barrier(l, 111, 2);
        replacement = prepare(l, ops[op], 1);
        submit(l, ops[op], 20);
        ack(l, ops[op], 0, 0, 21);
        submit(l, 0x2018, 22);
        ack(l, 0x2018, 0, 111, 23); /* two duplicates: stale Rand repeats */
        own_summary s;
        own_snapshot(l, &s);
        CHECK(s.runtime.reason == SESSION_AMBIGUOUS);
        no_undo(l, replacement, 24);
        own_free(l);
    }
    /* DD:1102 strongest counter-case: histories A and B have identical wire
     * success and equal readback. Stop before B's real refusal/barrier: no undo.
     * A8 intentionally permits forward progress, never premature authority.
     */
    for (int history = 0; history < 2; history++) {
        own_ledger *l = own_new(policy, 1);
        own_ref r = prepare(l, 0x200c, 1);
        submit(l, 0x200c, 0);
        ack(l, 0x200c, 0, 0, 1); /* local success A / foreign success B */
        const uint8_t readback[] = {1, 0};
        own_readback(l, 0x200c, readback, 2);
        no_undo(l, r, 2);
        own_release(l, SESSION_STOP, 3);
        no_undo(l, r, 3);
        if (history) ack(l, 0x200c, 0x0c, 0, 4);
        own_free(l);
    }
    /* Foreign Create ACK, expected-peer completion and fresh input, then Stop
     * before local refusal: even apparent usable input cannot bypass the barrier.
     */
    own_ledger *l = own_new(policy, 1);
    own_command c = command(0x0405, 0);
    c.peer = peer(9);
    memcpy(c.params, c.peer.wire, 6);
    CHECK(own_reserve_link(l, &c.peer, &c.link) == OWN_OK);
    own_ref r;
    CHECK(own_prepare(l, &c, &r) == OWN_OK);
    submit(l, 0x0405, 0);
    ack(l, 0x0405, 0, 0, 1);
    own_terminal_event term = { .kind = OWN_TERM_CONNECTION, .peer = c.peer, .handle = 60 };
    own_terminal(l, r, &term, 2);
    CHECK(own_input(l, c.link, 60, 3) != OWN_OK);
    own_release(l, SESSION_STOP, 4);
    own_summary left;
    own_snapshot(l, &left);
    CHECK(left.radio_left == 1); /* Observed but unowned radio link is reported. */
    no_undo(l, r, 4);
    ack(l, 0x0405, 0x0c, 0, 5);
    own_free(l);
}

static void replacements_and_capacity(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_ref first = {0}, previous = {0};
    own_receipt stale = {{0}};
    for (unsigned i = 0; i < 40; i++) {
        own_command c = command(0x200c, (i & 1) + 1);
        own_ref r;
        CHECK(own_prepare(l, &c, &r) == OWN_OK);
        uint64_t now = 4*i;
        submit(l, 0x200c, now);
        own_receipt receipt = ack(l, 0x200c, 0, 0, now + 1);
        if (i == 0) { first = r; stale = receipt; }
        else {
            own_receive(l, &stale, now + 2);
            no_undo(l, r, now + 2);
        }
        barrier(l, 1000 + i, now + 2);
        if (i) CHECK(own_reclaim(l, previous) == OWN_OK);
        previous = r;
    }
    no_undo(l, first, 160);
    own_token t;
    if (policy == OWN_POLICY_SINGLE_PRODUCER) {
        CHECK(own_request_undo(l, previous, 160, &t) == OWN_OK);
        own_effect e;
        CHECK(own_commit(l, &t, 160, &e) == OWN_OK);
        CHECK(e.plen == 2 && e.params[0] == 0 && e.params[1] == 0);
    } else no_undo(l, previous, 160);
    CHECK(own_invariant(l) == NULL);
    own_free(l);

    l = own_new(policy, 1);
    for (unsigned i = 0; i < OWN_MAX; i++) prepare(l, 0x1009, 0);
    own_command c = command(0x1009, 0);
    own_ref r;
    CHECK(own_prepare(l, &c, &r) == OWN_NO_SPACE);
    own_free(l);

    l = own_new(policy, 1);
    for (unsigned i = 0; i <= OWN_BARRIER_MAX; i++) {
        r = prepare(l, 0x200c, (i & 1) + 1);
        uint64_t now = 4*i;
        submit(l, 0x200c, now);
        ack(l, 0x200c, 0, 0, now + 1);
        barrier(l, i + 1, now + 2);
        if (i == OWN_BARRIER_MAX) no_undo(l, r, now + 3);
        /* Superseded setting records can be reclaimed, never live authority. */
        if (i) {
            own_ref old = {r.attempt, r.serial - 1};
            CHECK(own_reclaim(l, old) == OWN_OK);
        }
    }
    own_summary s;
    own_snapshot(l, &s);
    CHECK(s.barriers == OWN_BARRIER_MAX);
    own_free(l);
}

static void links_and_virtual(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    CHECK(own_activate(l));
    own_ref records[4];
    link_ref links[4];
    for (unsigned i = 0; i < 4; i++) {
        /* Longest diagnostic query stream must not consume retained capacity. */
        for (unsigned q = 0; q < 24; q++) {
            own_ref r = prepare(l, 0x1009, 0);
            submit(l, 0x1009, 100*i);
            ack(l, 0x1009, 0, 0, 100*i);
            CHECK(own_reclaim(l, r) == OWN_OK);
        }
        links[i] = connect_link(l, i + 1, 40 + i, &records[i], 100*i);
        ready(l, links[i], 100*i);
    }
    own_peer p = peer(10);
    link_ref fifth;
    CHECK(own_reserve_link(l, &p, &fifth) == OWN_NO_SPACE);
    /* Freshness expires independently of local generation and the lease. */
    no_undo(l, records[0], 1317);
    /* Lost disconnect followed by handle reuse must trip, not select by slot. */
    own_disconnected(l, links[0], 40, 1320);
    local(l, OWN_EFFECT_RETIRE, 1320, 1);
    local(l, OWN_EFFECT_DELETE, 1320, 1);
    CHECK(own_detach_link(l, links[0]) == OWN_OK);
    CHECK(!own_link_current(l, links[0]));
    own_command c = command(0x0405, 0);
    c.peer = peer(11);
    memcpy(c.params, c.peer.wire, 6);
    CHECK(own_reserve_link(l, &c.peer, &c.link) == OWN_OK);
    CHECK(c.link.generation != links[0].generation);
    CHECK(own_input(l, links[0], 40, 1321) != OWN_OK);
    own_ref reused;
    CHECK(own_prepare(l, &c, &reused) == OWN_OK);
    submit(l, 0x0405, 1350);
    ack(l, 0x0405, 0, 0, 1351);
    barrier(l, 500, 1352);
    own_terminal_event term = { .kind = OWN_TERM_CONNECTION, .peer = c.peer, .handle = 40 };
    own_terminal(l, reused, &term, 1353);
    no_undo(l, reused, 1354);
    own_summary s;
    own_snapshot(l, &s);
    CHECK(s.runtime.reason == SESSION_AMBIGUOUS);
    CHECK(own_invariant(l) == NULL);
    own_free(l);

    for (int proof = 0; proof < 2; proof++) {
        l = own_new(policy, 1);
        own_activate(l);
        own_ref r;
        link_ref link = connect_link(l, 1, 40, &r, 0);
        local(l, OWN_EFFECT_CONNECT, 13, 1);
        local(l, OWN_EFFECT_ADD, 14, 1);
        own_release(l, SESSION_STOP, 15); /* pending identity may drain, never Bind */
        local(l, OWN_EFFECT_RETIRE, 15, 1);
        own_backend_identity(l, link, 700, proof);
        own_token t;
        if (proof) local(l, OWN_EFFECT_DELETE, 16, 0); /* failed delete not retried */
        CHECK(own_next_local(l, 17, &t) != OWN_OK);
        CHECK(own_detach_link(l, link) != OWN_OK || proof);
        if (!proof) {
            own_transport_closed(l, 18);
            local(l, OWN_EFFECT_FINALIZE, 19, 1);
            own_finalized(l, CLEANUP_UNCERTAIN);
            own_backend_identity(l, link, 700, 1);
            CHECK(own_next_local(l, 20000, &t) != OWN_OK);
        }
        own_free(l);
    }
    l = own_new(policy, 1);
    own_activate(l);
    own_ref r;
    connect_link(l, 1, 40, &r, 0);
    local(l, OWN_EFFECT_CONNECT, 13, 1);
    own_token t;
    own_effect e;
    CHECK(own_next_local(l, 14, &t) == OWN_OK); /* queued Add before C/reentry */
    own_release(l, SESSION_REST, 15);
    CHECK(own_commit(l, &t, 15, &e) != OWN_OK);
    local(l, OWN_EFFECT_RETIRE, 15, 1);
    CHECK(own_next_local(l, 16, &t) != OWN_OK);
    own_free(l);
}

static void boundaries(own_policy policy)
{
    for (int supported = 0; supported < 2; supported++) {
        own_ledger *l = own_new(policy, supported);
        own_ref r = prepare(l, 0x200c, 1);
        submit(l, 0x200c, 0);
        ack(l, 0x200c, 0, 0, 1);
        own_release(l, SESSION_STOP, 2); /* no barrier after latch */
        own_token t;
        CHECK(own_next_hci(l, 3, &t) != OWN_OK);
        no_undo(l, r, 3);
        own_free(l);
    }
    own_ledger *l = own_new(policy, 1);
    own_ref r = prepare(l, 0x200c, 1);
    submit(l, 0x200c, 0);
    own_receipt receipt = ack(l, 0x200c, 0, 0, 1);
    barrier(l, 42, 2);
    own_release(l, SESSION_STOP, 2);
    own_token t;
    own_effect e;
    if (policy == OWN_POLICY_SINGLE_PRODUCER) {
        CHECK(own_request_undo(l, r, 3, &t) == OWN_OK);
        CHECK(own_commit(l, &t, 3, &e) == OWN_OK);
        own_submitted(l, &t, OWN_SENT, 3);
        own_receive(l, &receipt, 4);
        CHECK(own_invariant(l) == NULL);
        no_undo(l, r, 4);
        ack(l, 0x200c, 0x0c, 0, 5); /* undo failure never rearms */
    }
    for (unsigned completion = 1; completion <= 64; completion++) {
        CHECK(own_drain_allowed(l, 9751, completion));
        CHECK(!own_drain_allowed(l, 9752, completion));
        CHECK(!own_drain_allowed(l, 10003, completion));
    }
    CHECK(!own_drain_allowed(l, 10, 65));
    own_free(l);
}

static void interrupted_acquisitions(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    CHECK(own_activate(l));
    own_ref r;
    link_ref link = connect_link(l, 1, 40, &r, 0);
    local(l, OWN_EFFECT_CONNECT, 13, 1);
    own_token add;
    own_effect effect;
    CHECK(own_next_local(l, 14, &add) == OWN_OK);
    CHECK(own_commit(l, &add, 14, &effect) == OWN_OK);
    CHECK(own_release(l, SESSION_STOP, 15));
    /* The successful acquisition must survive a release inside AddDevice. */
    own_backend_local_result(l, &add, 1);
    local(l, OWN_EFFECT_RETIRE, 15, 1);
    own_backend_identity(l, link, 800, 1);
    local(l, OWN_EFFECT_DELETE, 16, 1);
    own_free(l);

    l = own_new(policy, 1);
    own_command c = command(0x0405, 0);
    c.peer = peer(1);
    memcpy(c.params, c.peer.wire, 6);
    CHECK(own_reserve_link(l, &c.peer, &c.link) == OWN_OK);
    CHECK(own_prepare(l, &c, &r) == OWN_OK);
    submit(l, 0x0405, 0);
    own_terminal_event term = { .kind = OWN_TERM_CONNECTION, .peer = c.peer, .handle = 40 };
    own_terminal(l, r, &term, 1);
    own_disconnected(l, c.link, 40, 2); /* terminal before ACK, then disconnect */
    ack(l, 0x0405, 0, 0, 3);
    own_token t;
    if (own_next_hci(l, 4, &t) == OWN_OK) {
        CHECK(own_commit(l, &t, 4, &effect) == OWN_OK);
        own_submitted(l, &t, OWN_SENT, 4);
        ack(l, 0x2018, 0, 17, 5);
    }
    CHECK(own_activate(l));
    CHECK(own_input(l, c.link, 40, 6) != OWN_OK);
    no_undo(l, r, 6);
    CHECK(own_detach_link(l, c.link) == OWN_OK);
    own_free(l);

    l = own_new(policy, 1);
    r = prepare(l, 0x200c, 1);
    submit(l, 0x200c, 0);
    own_fail(l, 1, 1);
    own_transport_closed(l, 2);
    local(l, OWN_EFFECT_FINALIZE, 3, 1);
    own_finalized(l, CLEANUP_CONFIRMED);
    CHECK(!own_retry(l)); /* Unanswered wire command cannot safely reopen a lane. */
    own_free(l);

    l = own_new(policy, 1);
    CHECK(own_activate(l));
    c = command(0x0405, 0);
    c.peer = peer(1);
    memcpy(c.params, c.peer.wire, 6);
    CHECK(own_reserve_link(l, &c.peer, &c.link) == OWN_OK);
    CHECK(own_prepare(l, &c, &r) == OWN_OK);
    submit(l, 0x0405, 0);
    ack(l, 0x0405, 0, 0, 1);
    barrier(l, 90, 2);
    term = (own_terminal_event){ .kind = OWN_TERM_CONNECTION, .peer = c.peer, .status = 4 };
    own_terminal(l, r, &term, 12);
    CHECK(own_detach_link(l, c.link) == OWN_OK);
    CHECK(own_reclaim(l, r) == OWN_OK);
    own_free(l);

    l = own_new(policy, 1);
    CHECK(own_activate(l));
    link = connect_link(l, 1, 40, &r, 0);
    ready(l, link, 0);
    own_release(l, SESSION_STOP, 20);
    own_tick(l, 10021); /* Optional work is over; mandatory local close still runs. */
    own_transport_closed(l, 10021);
    local(l, OWN_EFFECT_RETIRE, 10021, 1);
    local(l, OWN_EFFECT_DELETE, 10021, 1);
    local(l, OWN_EFFECT_FINALIZE, 10021, 1);
    own_finalized(l, CLEANUP_CONFIRMED);
    own_summary late;
    own_snapshot(l, &late);
    CHECK(late.finalized && late.links == 0 && late.radio_left == 1);
    CHECK(late.runtime.deadline == 10020); /* Closure does not renew the deadline. */
    own_free(l);

    for (int fence_sent = 0; fence_sent < 2; fence_sent++) {
        l = own_new(policy, 1);
        CHECK(own_activate(l));
        c = command(0x0405, 0);
        c.peer = peer(1);
        memcpy(c.params, c.peer.wire, 6);
        CHECK(own_reserve_link(l, &c.peer, &c.link) == OWN_OK);
        CHECK(own_prepare(l, &c, &r) == OWN_OK);
        submit(l, 0x0405, 0);
        ack(l, 0x0405, 0, 0, 1);
        if (fence_sent) submit(l, 0x2018, 2);
        term = (own_terminal_event){ .kind = OWN_TERM_CONNECTION, .peer = c.peer, .handle = 40 };
        own_terminal(l, r, &term, 3);
        own_disconnected(l, c.link, 40, 4);
        if (fence_sent) ack(l, 0x2018, 0, 91, 5);
        own_snapshot(l, &late);
        CHECK(late.runtime.reason == SESSION_NONE && late.ambiguity_count == 0);
        CHECK(own_input(l, c.link, 40, 6) != OWN_OK);
        CHECK(own_detach_link(l, c.link) == OWN_OK);
        CHECK(own_reclaim(l, r) == OWN_OK);
        own_ref query = prepare(l, 0x1009, 0);
        submit(l, 0x1009, 7);
        ack(l, 0x1009, 0, 0, 8);
        CHECK(own_reclaim(l, query) == OWN_OK);
        own_free(l);
    }
}

static void continuity_and_identity(own_policy policy)
{
    own_ledger *a = own_new(policy, 1), *b = own_new(policy, 1);
    own_ref ra = prepare(a, 0x200c, 1), rb = prepare(b, 0x200c, 2);
    own_token ta, tb;
    own_effect e;
    CHECK(own_next_hci(a, 0, &ta) == OWN_OK);
    CHECK(own_next_hci(b, 0, &tb) == OWN_OK);
    CHECK(own_commit(b, &ta, 0, &e) != OWN_OK); /* A different ledger is not an epoch. */
    CHECK(own_commit(b, &tb, 0, &e) == OWN_OK && e.params[0] == 2);
    no_undo(a, ra, 0); no_undo(b, rb, 0);
    own_free(a); own_free(b);

    for (int submitted = 0; submitted < 2; submitted++) {
        own_ledger *l = own_new(policy, 1);
        own_ref old = prepare(l, 0x200c, 1);
        submit(l, 0x200c, 0);
        ack(l, 0x200c, 0, 0, 1);
        barrier(l, 42, 2);
        own_ref replacement = prepare(l, 0x200c, 0);
        CHECK(own_next_hci(l, 20, &ta) == OWN_OK);
        CHECK(own_commit(l, &ta, 20, &e) == OWN_OK);
        own_submitted(l, &ta, submitted ? OWN_SENT : OWN_NOT_SENT, 20);
        if (submitted) {
            ack(l, 0x200c, 0x0c, 0, 21);
            if (policy == OWN_POLICY_SINGLE_PRODUCER) {
                no_undo(l, old, 21);
                barrier(l, 43, 22);
            }
        }
        no_undo(l, replacement, 22);
        CHECK(own_reclaim(l, replacement) == OWN_OK);
        if (policy == OWN_POLICY_SINGLE_PRODUCER)
            CHECK(own_request_undo(l, old, 22, &ta) == OWN_OK);
        else no_undo(l, old, 22);
        own_setting_changed(l, 0x200c, 23); /* Equal foreign writes revoke too. */
        CHECK(own_commit(l, &ta, 23, &e) != OWN_OK);
        no_undo(l, old, 23);
        own_free(l);
    }
    own_ledger *l = own_new(policy, 1);
    CHECK(own_activate(l));
    own_ref r;
    connect_link(l, 1, 40, &r, 0);
    /* The old link disappears, but its disconnect event is lost. A new peer
     * reports the same handle in another free slot; neither may command it. */
    own_command c = command(0x0405, 0);
    c.peer = peer(2);
    memcpy(c.params, c.peer.wire, 6);
    CHECK(own_reserve_link(l, &c.peer, &c.link) == OWN_OK);
    CHECK(own_prepare(l, &c, &r) == OWN_OK);
    submit(l, 0x0405, 20);
    ack(l, 0x0405, 0, 0, 21);
    barrier(l, 43, 22);
    own_terminal_event term = { .kind = OWN_TERM_CONNECTION, .peer = c.peer, .handle = 40 };
    own_terminal(l, r, &term, 23);
    no_undo(l, r, 24);
    own_summary snap;
    own_snapshot(l, &snap);
    CHECK(snap.runtime.reason == SESSION_AMBIGUOUS);
    own_free(l);
}

static void command_families(own_policy policy)
{
    const struct { unsigned op, plen; own_terminal_kind terminal; } procedures[] = {
        {0x0401, 5, OWN_TERM_INQUIRY}, {0x0411, 2, OWN_TERM_AUTH},
        {0x0413, 3, OWN_TERM_ENCRYPTION}, {0x2013, 14, OWN_TERM_UPDATE},
        {0x2019, 28, OWN_TERM_ENCRYPTION}, {0x2025, 0, OWN_TERM_P256},
        {0x2026, 64, OWN_TERM_DHKEY}
    };
    for (unsigned i = 0; i < sizeof procedures / sizeof *procedures; i++) {
        for (int early = 0; early < 2; early++) {
            own_ledger *l = own_new(policy, 1);
            own_activate(l);
            own_ref parent;
            link_ref link = connect_link(l, 1, 40, &parent, 0);
            own_command c = command(procedures[i].op, 0);
            c.plen = (uint8_t)procedures[i].plen;
            c.original_known = 0;
            c.peer = peer(1);
            if (c.opcode != 0x0401 && c.opcode != 0x2025) c.link = link;
            if (c.opcode == 0x0411 || c.opcode == 0x0413 ||
                c.opcode == 0x2013 || c.opcode == 0x2019) c.params[0] = 40;
            own_ref r;
            CHECK(own_prepare(l, &c, &r) == OWN_OK);
            submit(l, c.opcode, 20);
            own_ref second;
            CHECK(own_prepare(l, &c, &second) == OWN_BUSY);
            own_terminal_event event = { .kind = procedures[i].terminal, .handle = 40 };
            if (early) own_terminal(l, r, &event, 21);
            ack(l, c.opcode, 0, 0, 22);
            own_state state;
            own_status(l, r, &state);
            CHECK(state == (early ? OWN_CLOSED : OWN_ACKNOWLEDGED));
            no_undo(l, r, 23);
            if (!early) own_terminal(l, r, &event, 24);
            CHECK(own_reclaim(l, r) == OWN_OK);
            own_free(l);
        }
    }
    const unsigned queries[] = {
        0x080e,0x0c14,0x0c15,0x0c17,0x0c19,0x0c1b,0x0c1d,0x0c23,
        0x0c39,0x0c42,0x0c44,0x0c46,0x0c48,0x0c51,0x0c55,0x0c58,
        0x0c6c,0x1001,0x1002,0x1003,0x1004,0x1005,0x1009,0x2002
    };
    own_ledger *l = own_new(policy, 1);
    for (unsigned i = 0; i < sizeof queries / sizeof *queries; i++) {
        own_command c = command(queries[i], 0);
        if (queries[i] == 0x1004) { c.plen = 1; c.params[0] = 1; }
        c.original_known = 0;
        own_ref r;
        CHECK(own_prepare(l, &c, &r) == OWN_OK);
        submit(l, c.opcode, 0);
        ack(l, c.opcode, 0, 0, 1);
        CHECK(own_reclaim(l, r) == OWN_OK);
    }
    own_free(l);

    l = own_new(policy, 1);
    own_activate(l);
    own_ref parent;
    link_ref link = connect_link(l, 1, 40, &parent, 0);
    own_command foreign = command(0x040b, 0);
    foreign.plen = 22;
    foreign.original_known = 0;
    foreign.link = link;
    foreign.peer = peer(2);
    memcpy(foreign.params, foreign.peer.wire, 6);
    own_ref r;
    CHECK(own_prepare(l, &foreign, &r) == OWN_NO_PROOF);
    foreign = command(0x0411, 0);
    foreign.plen = 2;
    foreign.original_known = 0;
    foreign.link = link;
    foreign.peer = peer(1);
    foreign.params[0] = 41; /* Local generation is not another wire handle. */
    CHECK(own_prepare(l, &foreign, &r) == OWN_NO_PROOF);
    own_free(l);

    for (int reuse = 0; reuse < 2; reuse++) {
        l = own_new(policy, 1);
        own_activate(l);
        link = connect_link(l, 1, 40, &parent, 0);
        own_command c = command(0x0411, 0);
        c.plen = 2; c.original_known = 0;
        c.link = link; c.peer = peer(1); c.params[0] = 40;
        CHECK(own_prepare(l, &c, &r) == OWN_OK);
        own_token held;
        own_effect effect;
        CHECK(own_next_hci(l, 20, &held) == OWN_OK);
        own_disconnected(l, link, 40, 21);
        if (reuse) {
            CHECK(own_detach_link(l, link) == OWN_OK);
            own_peer replacement = peer(2);
            link_ref other;
            CHECK(own_reserve_link(l, &replacement, &other) == OWN_OK);
            CHECK(other.generation != link.generation);
        }
        CHECK(own_commit(l, &held, 22, &effect) != OWN_OK);
        CHECK(own_reclaim(l, r) == OWN_OK);
        own_free(l);
    }
    for (int terminal_first = 0; policy == OWN_POLICY_SINGLE_PRODUCER && terminal_first < 2;
         terminal_first++) {
        l = own_new(policy, 1);
        own_activate(l);
        link = connect_link(l, 1, 40, &parent, 0);
        ready(l, link, 0);
        own_token t;
        own_effect e;
        CHECK(own_request_undo(l, parent, 20, &t) == OWN_OK);
        CHECK(own_commit(l, &t, 20, &e) == OWN_OK && e.opcode == 0x0406);
        own_submitted(l, &t, OWN_SENT, 20);
        CHECK(own_input(l, link, 40, 21) != OWN_OK);
        local(l, OWN_EFFECT_RETIRE, 21, 1);
        local(l, OWN_EFFECT_DELETE, 21, 1);
        own_terminal_event refusal = { .kind = OWN_TERM_DISCONNECT, .handle = 40,
                                      .status = 0x0c };
        if (terminal_first) {
            own_terminal(l, parent, &refusal, 22);
            ack(l, 0x0406, 0, 0, 23);
        } else {
            ack(l, 0x0406, 0, 0, 22);
            own_terminal(l, parent, &refusal, 23);
        }
        own_state state;
        CHECK(own_status(l, parent, &state) == OWN_UNCERTAIN);
        CHECK(state == OWN_QUARANTINED);
        own_summary snap;
        own_snapshot(l, &snap);
        CHECK(snap.runtime.reason == SESSION_NONE && snap.ambiguity_count == 0);
        no_undo(l, parent, 24);
        own_release(l, SESSION_STOP, 24);
        own_snapshot(l, &snap);
        CHECK(snap.radio_left == 1);
        own_free(l);
    }
}

static void replacement_veto(own_policy policy)
{
    for (unsigned outcome = 0; outcome < 3; outcome++) {
        own_ledger *l = own_new(policy, 1);
        own_ref old = prepare(l, 0x200c, 1);
        submit(l, 0x200c, 0);
        ack(l, 0x200c, 0, 0, 1);
        barrier(l, 71, 2);
        own_ref next = prepare(l, 0x200c, 0);
        own_command refresh = command(0x200c, 1);
        own_ref ignored;
        CHECK(own_prepare(l, &refresh, &ignored) == OWN_BUSY);
        own_token t;
        own_effect e;
        CHECK(own_next_hci(l, 20, &t) == OWN_OK);
        CHECK(own_commit(l, &t, 20, &e) == OWN_OK);
        const uint8_t foreign[] = {7, 0}, equal_old[] = {1, 0};
        own_readback(l, 0x200c, foreign, 2);
        own_submitted(l, &t, outcome ? OWN_SENT : OWN_NOT_SENT, 20);
        if (outcome) ack(l, 0x200c, outcome == 1 ? 0x0c : 0, 0, 21);
        if (outcome == 2) barrier(l, 72, 22);
        no_undo(l, old, 23);
        no_undo(l, next, 23);
        own_readback(l, 0x200c, equal_old, 2); /* Equality cannot revive the veto. */
        no_undo(l, old, 24);
        no_undo(l, next, 24);
        own_free(l);
    }
}

static void old_refusal(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_ref historical = prepare(l, 0x200c, 0);
    submit(l, 0x200c, 0);
    ack(l, 0x200c, 0x0c, 0, 1);
    CHECK(own_reclaim(l, historical) == OWN_OK);
    own_ref old = prepare(l, 0x200c, 1);
    submit(l, 0x200c, 2);
    ack(l, 0x200c, 0, 0, 3);
    barrier(l, 81, 4);
    own_ref replacement = prepare(l, 0x200c, 0);
    submit(l, 0x200c, 20);
    ack(l, 0x200c, 0x0c, 0, 21); /* Copy of historical refusal, not this write. */
    no_undo(l, old, 21);
    if (policy == OWN_POLICY_SINGLE_PRODUCER) submit(l, 0x2018, 22);
    ack(l, 0x200c, 0, 0, 23); /* The actual current write succeeded. */
    if (policy == OWN_POLICY_SINGLE_PRODUCER) ack(l, 0x2018, 0, 82, 24);
    no_undo(l, old, 25);
    no_undo(l, replacement, 25);
    own_summary summary;
    own_snapshot(l, &summary);
    CHECK(summary.runtime.reason == SESSION_AMBIGUOUS);
    own_free(l);
}

static void residual_r1(void)
{
    /* Deliberately outside A8's SINGLE_PRODUCER assumption: another writer
     * supplies both the outstanding opcode and a fresh Rand before local answers.
     * Raw bytes cannot distinguish it. This is a reproduction, not a safety gate.
     */
    for (own_policy policy = OWN_POLICY_NO_RESTORE; policy <= OWN_POLICY_SINGLE_PRODUCER; policy++) {
        own_ledger *l = own_new(policy, 1);
        own_ref r = prepare(l, 0x200c, 1);
        submit(l, 0x200c, 0);
        ack(l, 0x200c, 0, 0, 1); /* Foreign ON; real local ON reply still pending. */
        const uint8_t readback[] = {1, 0};
        own_readback(l, 0x200c, readback, sizeof readback);
        barrier(l, 12345, 2); /* Foreign fresh Rand; violates producer assumption. */
        own_release(l, SESSION_STOP, 3);
        own_token t;
        int issuable = own_request_undo(l, r, 3, &t) == OWN_OK;
        CHECK(issuable == (policy == OWN_POLICY_SINGLE_PRODUCER));
        printf("residual-R1 policy=%d undo-issuable=%d (excluded producer assumption)\n",
               policy, issuable);
        own_free(l);
    }
}

static void acl_output(own_policy policy)
{
    {
        own_ledger *l = own_new(policy, 1);
        own_acl_lane *lane = own_acl_new(l);
        own_command c = command(0x0405, 0);
        own_ref parent;
        own_acl_token token;
        own_acl_effect effect;
        link_ref link, replacement;
        const uint8_t packet[] = {40, 0x20, 0, 0};
        CHECK(l && lane && own_activate(l));
        c.peer = peer(1);
        memcpy(c.params, c.peer.wire, 6);
        CHECK(own_reserve_link(l, &c.peer, &link) == OWN_OK);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 0, &token) == OWN_NO_PROOF);
        c.link = link;
        CHECK(own_prepare(l, &c, &parent) == OWN_OK);
        submit(l, 0x0405, 0);
        own_terminal_event terminal = {.kind = OWN_TERM_CONNECTION, .peer = c.peer, .handle = 40};
        own_terminal(l, parent, &terminal, 1);
        ack(l, 0x0405, 0, 0, 2);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 2, &token) == OWN_NO_PROOF);
        barrier(l, 1, 3);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 4, &token) == OWN_OK);
        own_observe_disconnect(l, 40, 5);
        CHECK(own_detach_link(l, link) == OWN_OK);
        CHECK(own_reserve_link(l, &c.peer, &replacement) == OWN_OK);
        CHECK(replacement.slot == link.slot && replacement.generation != link.generation);
        CHECK(own_acl_commit(l, lane, &token, 6, &effect) != OWN_OK);
        own_acl_free(lane); own_free(l);
    }
    for (unsigned loss = 0; loss < 7; loss++) {
        own_ledger *l = own_new(policy, 1);
        own_ledger *foreign = own_new(policy, 1);
        own_acl_lane *lane = own_acl_new(l), *other = own_acl_new(l);
        own_acl_token token, next;
        own_acl_effect effect;
        own_ref parent;
        uint8_t packet[] = {40, 0x20, 1, 0, 0xa5};
        CHECK(l && foreign && lane && other && own_activate(l));
        link_ref link = connect_link(l, 1, 40, &parent, 0);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 20, &token) == OWN_OK);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 20, &next) == OWN_BUSY);
        packet[4] = 0x5a;
        CHECK(own_acl_commit(foreign, lane, &token, 20, &effect) == OWN_INVALID);
        CHECK(own_acl_commit(l, other, &token, 20, &effect) == OWN_INVALID);
        if (loss == 1) own_release(l, SESSION_STOP, 21);
        if (loss == 2) own_observe_disconnect(l, 40, 21);
        if (loss == 3) own_detector_trip(l, 21);
        if (loss == 4) own_transport_closed(l, 21);
        if (loss == 5) {
            prepare(l, 0x1009, 0);
            submit(l, 0x1009, 21); /* Missing ACK must revoke ACL too. */
        }
        if (loss == 6) own_observe_disconnect(l, 41, 21); /* foreign handle */
        uint64_t now = loss == 5 ? 5000 : 21;
        own_result result = own_acl_commit(l, lane, &token, now, &effect);
        CHECK((result == OWN_OK) == (loss == 0 || loss == 6));
        if (result == OWN_OK) {
            CHECK(effect.handle == 40 && effect.link.generation == link.generation);
            CHECK(effect.len == sizeof packet && effect.packet[4] == 0xa5);
            CHECK(own_acl_commit(l, lane, &token, now, &effect) != OWN_OK);
            own_acl_submitted(lane, &token, OWN_SENT, now);
            CHECK(own_acl_prepare(lane, link, packet, sizeof packet, now, &next) == OWN_OK);
            CHECK(own_acl_commit(l, lane, &token, now, &effect) != OWN_OK);
            CHECK(own_acl_commit(l, lane, &next, now, &effect) == OWN_OK);
            own_acl_submitted(lane, &next, OWN_NOT_SENT, now);
            if (!loss) {
                own_acl_free(lane);
                lane = own_acl_new(l);
                CHECK(lane != NULL);
                CHECK(own_acl_prepare(lane, link, packet, sizeof packet, now, &next) == OWN_OK);
                CHECK(own_acl_commit(l, lane, &token, now, &effect) == OWN_INVALID);
                CHECK(own_acl_commit(l, lane, &next, now, &effect) == OWN_OK);
                own_acl_submitted(lane, &next, OWN_SENT, now);
            }
        }
        CHECK(own_invariant(l) == NULL);
        own_acl_free(other); own_acl_free(lane);
        own_free(foreign); own_free(l);
    }
    {
        own_ledger *l = own_new(policy, 1);
        own_acl_lane *lane = own_acl_new(l);
        own_acl_token token;
        own_acl_effect effect;
        own_ref parent;
        uint8_t packet[OWN_ACL_MAX] = {40, 0x20};
        uint8_t before[4096], after[4096];
        CHECK(l && lane && own_activate(l));
        link_ref link = connect_link(l, 1, 40, &parent, 0);
        own_tick(l, 20);
        size_t size = own_canonical(l, before, sizeof before);
        CHECK(size <= sizeof before);
        CHECK(own_acl_prepare(lane, link, packet, 3, 20, &token) == OWN_INVALID);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet + 1, 20, &token) == OWN_INVALID);
        CHECK(own_acl_prepare(lane, link, NULL, 5, 20, &token) == OWN_INVALID);
        CHECK(own_acl_prepare(lane, link, packet, 5, 20, &token) == OWN_INVALID);
        unsigned payload = sizeof packet - 4;
        packet[2] = (uint8_t)payload; packet[3] = (uint8_t)(payload >> 8);
        packet[0] = 41;
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 20, &token) != OWN_OK);
        packet[0] = 40;
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 20, &token) == OWN_OK);
        CHECK(own_acl_commit(l, lane, &token, 20, &effect) == OWN_OK);
        CHECK(effect.len == sizeof packet && !memcmp(effect.packet, packet, sizeof packet));
        own_acl_submitted(lane, &token, OWN_SENT, 20);
        CHECK(own_canonical(l, after, sizeof after) == size && !memcmp(before, after, size));
        /* Output must not manufacture A8's recent accepted-input evidence. */
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 2000, &token) == OWN_OK);
        CHECK(own_acl_commit(l, lane, &token, 2000, &effect) == OWN_OK);
        own_acl_submitted(lane, &token, OWN_SENT, 2000);
        own_release(l, SESSION_STOP, 2001);
        no_undo(l, parent, 2001);
        own_acl_free(lane); own_free(l);
    }
    {
        own_ledger *l = own_new(policy, 1);
        own_acl_lane *lane = own_acl_new(l);
        own_acl_token token;
        own_acl_effect effect;
        own_ref parent;
        uint8_t packet[] = {40, 0x20, 0, 0};
        CHECK(l && lane && own_activate(l));
        link_ref link = connect_link(l, 1, 40, &parent, 0);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 20, &token) == OWN_OK);
        CHECK(own_acl_commit(l, lane, &token, 20, &effect) == OWN_OK);
        own_acl_submitted(lane, &token, OWN_SEND_UNCERTAIN, 20);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 21, &token) != OWN_OK);
        own_acl_free(lane); own_free(l);
    }
}

static void typed_hci_commit(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_token token;
    own_effect effect;
    uint64_t ack_by = 0;
    own_ref parent;
    CHECK(l && own_activate(l));
    link_ref link = connect_link(l, 1, 40, &parent, 0);
    CHECK(own_next_local(l, 20, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 20, &effect, &ack_by) == OWN_INVALID);
    CHECK(own_commit(l, &token, 20, &effect) == OWN_OK); /* not consumed by wrong backend */
    own_backend_local_result(l, &token, 1);
    prepare(l, 0x1009, 0);
    CHECK(own_next_hci(l, 20, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 20, &effect, &ack_by) == OWN_OK);
    CHECK(effect.opcode == 0x1009 && ack_by == 5000);
    own_submitted(l, &token, OWN_SENT, 20);
    ack(l, 0x1009, 0, 0, 21);
    own_acl_lane *lane = own_acl_new(l);
    own_acl_token acl;
    uint8_t packet[] = {40, 0x20, 0, 0};
    CHECK(lane != NULL);
    CHECK(own_acl_prepare(lane, link, packet, sizeof packet, 22, &acl) == OWN_OK);
    own_acl_free(lane); own_free(l);
}

static void typed_hci_phase_deadlines(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_ref parent = prepare(l, 0x200c, 1);
    own_token token;
    own_effect effect;
    uint64_t ack_by = 0;
    submit(l, 0x200c, 0);
    ack(l, 0x200c, 0, 0, 1);
    CHECK(own_next_hci(l, 20, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 20, &effect, &ack_by) == OWN_OK);
    CHECK(effect.opcode == 0x2018 && ack_by == 1020);
    own_submitted(l, &token, OWN_SENT, 20);
    ack(l, 0x2018, 0, 1, 21);
    own_release(l, SESSION_STOP, 22);
    if (policy == OWN_POLICY_SINGLE_PRODUCER) {
        CHECK(own_request_undo(l, parent, 22, &token) == OWN_OK);
        CHECK(own_commit_hci(l, &token, 22, &effect, &ack_by) == OWN_OK);
        CHECK(effect.opcode == 0x200c && ack_by == 1022);
        own_submitted(l, &token, OWN_SENT, 22);
    }
    own_free(l);
}

static void credit_events(own_policy policy)
{
    for (unsigned form = 0; form < 2; form++) {
        own_ledger *l = own_new(policy, 1);
        own_ack credit = {.event = form ? OWN_CS : OWN_CC};
        own_receipt receipt;
        own_summary summary;
        own_token next;
        CHECK(l && own_activate(l));
        prepare(l, 0x1009, 0);
        submit(l, 0x1009, 0);
        CHECK(own_backend_receipt(l, &credit, 1, &receipt) == OWN_RECEIPT_NONE);
        own_receive(l, &receipt, 1);
        own_snapshot(l, &summary);
        CHECK(summary.credit_nops == 1 && summary.ambiguity_count == 0 &&
              summary.runtime.reason == SESSION_NONE);
        CHECK(own_next_hci(l, 1, &next) == OWN_BUSY);
        ack(l, 0x1009, 0, 0, 2);
        CHECK(own_next_hci(l, 2, &next) != OWN_BUSY);
        own_free(l);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--residual-r1") == 0) {
        residual_r1();
        return 0;
    }
    CHECK(argc == 1);
    for (own_policy policy = OWN_POLICY_NO_RESTORE; policy <= OWN_POLICY_SINGLE_PRODUCER; policy++) {
        positive(policy);
        basic(policy);
        collisions(policy);
        replacements_and_capacity(policy);
        links_and_virtual(policy);
        boundaries(policy);
        interrupted_acquisitions(policy);
        continuity_and_identity(policy);
        command_families(policy);
        replacement_veto(policy);
        old_refusal(policy);
        acl_output(policy);
        typed_hci_commit(policy);
        typed_hci_phase_deadlines(policy);
        credit_events(policy);
    }
    printf("ownership checks=%d violations=%d\n", checks, failed);
    return failed != 0;
}
