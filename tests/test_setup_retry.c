/* GPL-3.0-or-later. Wire faults after the real consuming submission boundary. */
#include "sim.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, position, target, kind, remaining, last, sends, wire;
static unsigned baseline[64], nbaseline, recording, random_mode, rng;
static long first, second, third;
static int closing;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL setup position=%u kind=%u: %s\n", target, kind, #x); failures++; \
} } while (0)

static unsigned random_word(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static int command(void *ctx, unsigned op, const void *p, int n)
{
    sim_t *s = ctx;
    wire++;
    if (closing) return sim_model_command(ctx, op, p, n);
    if (op != last) {
        last = op;
        position++;
        sends = 0;
        if (recording) baseline[nbaseline++] = op;
    }
    sends++;
    if (position == target) {
        if (sends == 1) first = sim_now();
        if (sends == 2) second = sim_now();
        if (sends == 3) third = sim_now();
    }
    CHECK(sends <= 3);
    unsigned fault = random_mode ? random_word() % 64 : 63;
    if ((position == target && remaining) || (random_mode && fault < 3)) {
        unsigned mode = random_mode ? fault : kind;
        if (!random_mode) remaining--;
        if (mode == 0) return 1;       /* Accepted transport, command never executed. */
        sim_delay_events(s, 0x0e, 1, mode == 1 ? -1 : 1500);
    }
    if (!random_mode && kind == 3 && position == target && sends == 2)
        sim_delay_events(s, 0x0e, 1, 2000); /* Original first, resend surplus later. */
    return sim_model_command(ctx, op, p, n);
}

static host_t *open_host(sim_t **out)
{
    sim_pad pad = {.seed = 1};
    attrib_reset();
    position = last = sends = 0;
    first = second = third = 0;
    *out = sim_new(&pad);
    CHECK(*out != NULL);
    if (!*out) return NULL;
    sim_output_hooks(*out, command, NULL);
    host_t *h = host_prepare(sim_hci(*out), "build/setup-retry-absent.db", NULL);
    CHECK(h != NULL);
    if (h) sim_bind_ownership(*out, host_ownership(h));
    return h;
}

static void close_host(host_t *h, sim_t *s)
{
    CHECK(own_invariant(host_ownership(h)) == NULL);
    closing = 1;
    host_close(h);
    closing = 0;
    sim_free(s);
}

static void tokens_and_retirement(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_command c = {.opcode = 0x1002, .ack_by = 5000, .terminal_by = 9000, .setup_retry = 1};
    own_ref a, later;
    own_token first_token, repeat;
    own_effect effect;
    uint64_t deadline;
    CHECK(own_prepare(l, &c, &a) == OWN_OK);
    CHECK(own_next_hci(l, 1000, &first_token) == OWN_OK);
    CHECK(own_commit_hci(l, &first_token, 1000, &effect, &deadline) == OWN_OK);
    own_submitted(l, &first_token, OWN_SENT, 1000);
    CHECK(own_prepare(l, &c, &later) == OWN_OK); /* Prepared before history existed. */
    CHECK(own_next_hci(l, 2000, &repeat) == OWN_OK);
    CHECK(own_commit_hci(l, &first_token, 2000, &effect, &deadline) != OWN_OK);
    CHECK(own_commit_hci(l, &repeat, 2000, &effect, &deadline) == OWN_OK);
    CHECK(effect.resend == 1 && deadline == 6000);
    own_submitted(l, &repeat, OWN_SENT, 2000);
    own_ack ack = {.event = OWN_CC, .opcode = 0x1002};
    own_receipt receipt;
    CHECK(own_backend_receipt(l, &ack, 2100, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 2100);
    own_state state;
    CHECK(own_status(l, a, &state) == OWN_OK && state == OWN_CLOSED);
    CHECK(own_prepare(l, &c, &a) == OWN_NO_PROOF);
    CHECK(own_next_hci(l, 2100, &repeat) == OWN_OK);
    CHECK(own_commit_hci(l, &repeat, 2100, &effect, &deadline) == OWN_NO_PROOF);
    CHECK(own_backend_receipt(l, &ack, 2200, &receipt) == OWN_RECEIPT_RETIRED);
    own_receive(l, &receipt, 2200);
    CHECK(own_status(l, later, &state) == OWN_OK && state == OWN_PREPARED);
    own_release(l, SESSION_STOP, 2200);
    CHECK(own_commit_hci(l, &repeat, 2200, &effect, &deadline) != OWN_OK);
    CHECK(own_invariant(l) == NULL);
    own_free(l);
    l = own_new(policy, 1); /* History belongs only to the old transport's ledger. */
    CHECK(own_prepare(l, &c, &a) == OWN_OK);
    own_free(l);
}

static void refused_repeat(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_command c = {.opcode = 0x0c1a, .plen = 1, .params = {1},
                     .original_known = 1, .original_len = 1,
                     .ack_by = 6000, .terminal_by = 9000, .setup_retry = 1};
    own_ref previous, replacement;
    own_token token;
    own_effect effect;
    uint64_t deadline;
    own_receipt receipt;
    own_ack ack = {.event = OWN_CC, .opcode = 0x0c1a};
    CHECK(own_prepare(l, &c, &previous) == OWN_OK);
    CHECK(own_next_hci(l, 1000, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 1000, &effect, &deadline) == OWN_OK);
    own_submitted(l, &token, OWN_SENT, 1000);
    CHECK(own_backend_receipt(l, &ack, 1001, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1001);
    CHECK(own_next_hci(l, 1002, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 1002, &effect, &deadline) == OWN_OK);
    CHECK(effect.opcode == 0x2018);
    own_submitted(l, &token, OWN_SENT, 1002);
    ack = (own_ack){.event = OWN_CC, .opcode = 0x2018, .random_len = 8, .random = {1}};
    CHECK(own_backend_receipt(l, &ack, 1003, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1003);
    c.params[0] = 2;
    CHECK(own_prepare(l, &c, &replacement) == OWN_OK);
    CHECK(own_next_hci(l, 1100, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 1100, &effect, &deadline) == OWN_OK);
    own_submitted(l, &token, OWN_SENT, 1100);
    CHECK(own_next_hci(l, 2100, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 2100, &effect, &deadline) == OWN_OK);
    own_submitted(l, &token, OWN_SENT, 2100);
    ack = (own_ack){.event = OWN_CC, .opcode = 0x0c1a, .status = 12};
    CHECK(own_backend_receipt(l, &ack, 2101, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 2101);
    own_state state;
    CHECK(own_status(l, replacement, &state) == OWN_UNCERTAIN && state == OWN_QUARANTINED);
    CHECK(!own_activate(l));
    CHECK(own_request_undo(l, previous, 2102, &token) != OWN_OK);
    CHECK(own_invariant(l) == NULL);
    own_free(l);
}

int main(void)
{
    sim_t *s;
    log_open("/dev/null");
    tokens_and_retirement(OWN_POLICY_NO_RESTORE);
    tokens_and_retirement(OWN_POLICY_SINGLE_PRODUCER);
    refused_repeat(OWN_POLICY_NO_RESTORE);
    refused_repeat(OWN_POLICY_SINGLE_PRODUCER);
    recording = 1;
    host_t *h = open_host(&s);
    CHECK(h && host_start(h));
    close_host(h, s);
    recording = 0;
    CHECK(nbaseline > 25 && nbaseline < 64);
    unsigned positions = nbaseline;
    /* Two lost sends exercise all three windows at every setup position.
     * An ambiguous resent Rand must fail this try, not poison the next open. */
    for (target = 1; target <= positions; target++) {
        for (kind = 0; kind < 4; kind++) {
            remaining = kind >= 2 ? 1 : 2;
            h = open_host(&s);
            long begin = sim_now();
            int ready = h && host_start(h);
            CHECK(remaining == 0);
            CHECK(second - first == 1000);
            if (kind < 2) CHECK(third - second == 1000);
            CHECK(sim_now() - begin <= 5000 + 64 * 20);
            CHECK(ready == (baseline[target - 1] != 0x2018));
            close_host(h, s);
            remaining = 0;
            h = open_host(&s);
            CHECK(h && host_start(h));
            close_host(h, s);
        }
    }
    for (kind = 2; kind < 4; kind++) {
        target = 1;
        remaining = 1;
        h = open_host(&s);
        CHECK(h && host_start(h));
        unsigned before = wire;
        CHECK(!hci_sync(h, 0x1002, NULL, 0)); /* Never admit the later same opcode. */
        CHECK(wire == before);
        for (unsigned step = 0; step < 200; step++) host_poll(h, 20);
        CHECK(host_admits(h)); /* Surplus cannot settle anything or create authority. */
        close_host(h, s);
    }
    kind = 0;
    target = 1;
    remaining = 3;
    h = open_host(&s);
    CHECK(h && !host_start(h));
    CHECK(remaining == 0 && second - first == 1000 && third - second == 1000);
    CHECK(sim_now() - first == 5000);
    close_host(h, s);
    target = 0;
    random_mode = 1;
    unsigned ready_seeds = 0, tries = 0;
    for (unsigned seed = 1; seed <= 10000; seed++) {
        rng = seed;
        long begin = sim_now();
        for (unsigned attempt = 0; attempt < 5; attempt++) {
            h = open_host(&s);
            int ready = h && host_start(h);
            tries++;
            close_host(h, s);
            if (ready) { ready_seeds++; break; }
        }
        CHECK(sim_now() - begin <= 5 * 64 * 5000);
    }
    CHECK(ready_seeds >= 9990);
    printf("setup retry: positions=%u seeds=10000 ready=%u tries=%u checks=%u failures=%u\n",
           positions, ready_seeds, tries, checks, failures);
    log_close();
    return failures ? 1 : 0;
}
