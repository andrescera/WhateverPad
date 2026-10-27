/* GPL-3.0-or-later. Packed inquiry traffic through the real USB event parser. */
#define main usb_fixture_main
#include "test_hci_usb.c"
#undef main

static void burst(own_policy policy, int critical_only, int with_read)
{
    reset(); attrib_reset();
    hci_t h = start();
    own_ledger *l = bound(&h, policy);
    h.ops->event_len(h.ctx, 64); /* Owner's Marvell interrupt packet size. */
    h.ops->pump(h.ctx, 0);
    const unsigned char inquiry[] = {0x33, 0x8b, 0x9e, 8, 0};
    own_token token;
    own_ref inq = prepare(l, 0x0401, inquiry, sizeof inquiry, &token);
    hci_submission submission = {.kind = HCI_SUBMIT_COMMAND, .token.command = &token};
    CHECK(h.ops->op_submit(h.ctx, &submission) == OWN_SENT);
    acknowledge(h, 0x0401, 1, 0);
    own_ref read = {0};
    if (with_read) {
        read = prepare(l, 0x1005, NULL, 0, &token);
        CHECK(h.ops->op_submit(h.ctx, &submission) == OWN_SENT);
    }
    h.ops->pump(h.ctx, 0);

    const unsigned char result[17] = {0x22, 15, 1, 0xe7, 0xa0, 1, 2, 3, 4,
                                     1, 0, 0x3c, 4, 0x0c, 0, 0, 0xb0};
    const unsigned char request[12] = {4, 10, 6, 0xfa, 1, 2, 3, 4, 8, 5, 0, 1};
    for (unsigned i = 0; i < EVENTS; i++) {
        unsigned char packet[64];
        int n = 0;
        for (unsigned j = 0; j < (critical_only ? 5u : 3u); j++) {
            const unsigned char *e = critical_only ? request : result;
            int bytes = critical_only ? (int)sizeof request : (int)sizeof result;
            memcpy(packet + n, e, (size_t)bytes); n += bytes;
        }
        if (!critical_only && i % 4 == 0) {
            memcpy(packet + n, request, sizeof request);
            packet[n + 2] += (unsigned char)(i / 4);
            n += sizeof request;
        }
        if (!critical_only && i == EVENTS - 1) {
            const unsigned char complete[] = {1, 1, 0};
            const unsigned char reply[] = {0x0e, 4, 1, 5, 0x10, 0};
            memcpy(packet + n, complete, sizeof complete); n += sizeof complete;
            if (with_read) { memcpy(packet + n, reply, sizeof reply); n += sizeof reply; }
        }
        completion((int)i, packet, n);
    }
    CHECK(h.ops->pump(h.ctx, 0) == 1);
    own_summary s;
    own_snapshot(l, &s);
    printf("BURST_REPRO policy=%d critical=%d read=%d opcode=0x%04x ambiguous=%d overflow=%lu\n",
           policy, critical_only, with_read, s.failed_opcode,
           s.runtime.reason == SESSION_AMBIGUOUS, attrib_get()->overflow);
    if (critical_only) {
        CHECK(s.runtime.reason == SESSION_AMBIGUOUS && s.quarantined);
        CHECK(attrib_get()->overflow > 0 && !own_activate(l));
    } else {
        CHECK(s.runtime.state == SESSION_ACTIVE && !s.quarantined);
        CHECK(!attrib_get()->overflow && !s.failed_opcode);
        unsigned requests = 0, terminals = 0, results = 0, replies = 0;
        unsigned char packet[HCI_PKT_MAX];
        int n;
        while ((n = h.ops->next_event(h.ctx, packet, sizeof packet)) > 0) {
            if (packet[0] == 4) {
                CHECK(n == (int)sizeof request && packet[2] == 6 + requests &&
                      !memcmp(packet + 3, request + 3, sizeof request - 3));
                requests++;
            } else if (packet[0] == 1) {
                CHECK(requests == 10 && !replies);
                own_terminal_event terminal = {.kind = OWN_TERM_INQUIRY};
                own_terminal(l, inq, &terminal, clock_ms);
                terminals++;
            } else if (packet[0] == 0x0e) {
                CHECK(terminals == 1 && n == 6 && le16(packet + 3) == 0x1005 && !packet[5]);
                replies++;
            } else {
                CHECK(n == (int)sizeof result && !memcmp(packet, result, sizeof result));
                results++;
            }
        }
        CHECK(requests == 10 && terminals == 1 && replies == (unsigned)with_read &&
              results > 0 && results < 120);
        own_state state;
        CHECK(own_status(l, inq, &state) == OWN_OK && state == OWN_CLOSED);
        /* A queued read reply is critical even when discovery has filled the ring. */
        if (with_read) {
            CHECK(own_status(l, read, &state) == OWN_OK && state == OWN_CLOSED);
            CHECK(own_request_undo(l, read, clock_ms, &token) == OWN_NO_PROOF);
        }
        CHECK(!own_activate(l) && own_invariant(l) == NULL);
        prepare(l, 0x0401, inquiry, sizeof inquiry, &token);
        CHECK(h.ops->op_submit(h.ctx, &submission) == OWN_SENT);
        acknowledge(h, 0x0401, 1, 0); /* A following search is still admitted. */
    }
    closed_once(h);
    own_free(l);
}

int main(int argc, char **argv)
{
    own_policy policy = argc == 2 && !strcmp(argv[1], "single") ?
                        OWN_POLICY_SINGLE_PRODUCER : OWN_POLICY_NO_RESTORE;
    burst(policy, 0, 0);
    burst(policy, 0, 1);
    burst(policy, 1, 0); /* No discovery to discard: real critical loss stays fatal. */
    printf("event burst: policy=%d checks=%d failures=%d\n", policy, checks, failures);
    return failures ? 1 : 0;
}
