/* GPL-3.0-or-later. Idle watchdog faults after consuming the real send token. */
#include "sim.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, mode, sends, rng, lost, runtime;
static long sent_at[3];
#define CHECK(x) do { checks++; if (!(x)) { \
    if (failures < 20) fprintf(stderr, "FAIL mode=%u sends=%u: %s\n", mode, sends, #x); \
    failures++; \
} } while (0)

static unsigned random_word(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static int command(void *ctx, unsigned op, const void *p, int n)
{
    if (!runtime) return sim_model_command(ctx, op, p, n);
    CHECK(op == OP_READ_BUFFER_SIZE && n == 0);
    if (sends < 3) sent_at[sends] = sim_now();
    sends++;
    unsigned fault = mode == 6 ? random_word() % 64 : 63;
    if ((mode == 1 && sends == 1) || (mode == 3 && sends <= 2) ||
        mode == 5 || fault == 0) {
        lost++;
        return 1; /* Transport accepted; chip never executes the command. */
    }
    if ((mode == 2 && sends == 1) || fault == 1) {
        lost++;
        sim_delay_events(ctx, 0x0e, 1, -1);
    }
    if (mode == 4 && sends == 1)
        sim_delay_events(ctx, 0x0e, 1, 1500);
    return sim_model_command(ctx, op, p, n);
}

static int idle_case(unsigned scenario, unsigned seed)
{
    sim_pad pad = {.seed = seed};
    sim_t *s = sim_new(&pad);
    CHECK(s != NULL);
    attrib_reset();
    runtime = 0;
    sim_output_hooks(s, command, NULL);
    host_t *h = sim_open_host(s, "build/runtime-query-absent.db", NULL);
    CHECK(h != NULL);
    if (!h) { sim_free(s); return 0; }
    mode = scenario;
    rng = seed;
    runtime = 1;
    sends = lost = 0;
    long ready = sim_now();
    unsigned mtu = h->acl_mtu;
    int buffers = h->br.max;
    for (unsigned cycle = 0; cycle < 3 && host_admits(h); cycle++) {
        sends = 0;
        /* This is a virtual-clock test of the 20-second silence threshold. */
        host_poll(h, T_PROBE_IDLE + 1);
        CHECK(sends == 1 && host_admits(h));
        for (unsigned step = 0; step < 51 && host_admits(h); step++) {
            host_poll(h, 100);
            if (h->last_rx > sent_at[0]) break;
        }
        if (host_admits(h)) {
            host_poll(h, 2000); /* Deliver any surplus before the next idle interval. */
            CHECK(host_admits(h));
            CHECK(mode != 5 && sends <= PROBE_TRIES);
            CHECK(!attrib_get()->timeout && !attrib_get()->unanswered);
            CHECK(h->acl_mtu == mtu && h->br.max == buffers);
        } else {
            CHECK(sends == PROBE_TRIES);
            CHECK(sim_now() - sent_at[0] == 5000);
            char reason[192];
            host_failure_reason(h, reason, sizeof reason);
            CHECK(strstr(reason, "0x1005") && strstr(reason, "reply_timeout"));
        }
        if (mode == 5) break;
    }
    int alive = host_admits(h);
    CHECK(sim_now() - ready > T_PROBE_IDLE);
    if (mode != 5 && mode != 6) CHECK(alive);
    if (mode == 5) CHECK(!alive && lost == PROBE_TRIES);
    runtime = 0;
    if (alive) {
        CHECK(!host_transport_lost(h));
        CHECK(host_pair(h, 60)); /* Pair remains admitted, not just a stale page flag. */
        for (unsigned step = 0; step < 100 && !sim_inquiries(s) && host_admits(h); step++)
            host_poll(h, 20);
        CHECK(host_admits(h) && sim_inquiries(s) == 1);
    }
    CHECK(own_invariant(host_ownership(h)) == NULL);
    host_close(h);
    sim_free(s);
    return alive;
}

static void receipt_authority(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_command c = {.opcode = 0x1005, .idle_probe = 1,
                     .ack_by = 7000, .terminal_by = 9000};
    own_ref probe, different;
    own_token first, repeat, token;
    own_effect effect;
    own_receipt receipt;
    own_state state;
    uint64_t deadline;
    CHECK(own_prepare(l, &c, &probe) == OWN_INVALID); /* Never startup activation evidence. */
    CHECK(own_activate(l));
    CHECK(own_prepare(l, &c, &probe) == OWN_OK);
    CHECK(own_next_hci(l, 100, &first) == OWN_OK);
    CHECK(own_commit_hci(l, &first, 100, &effect, &deadline) == OWN_OK);
    CHECK(!effect.resend && deadline == 5100);
    own_submitted(l, &first, OWN_SENT, 100);
    CHECK(own_next_hci(l, 1100, &repeat) == OWN_OK);
    CHECK(own_commit_hci(l, &first, 1100, &effect, &deadline) != OWN_OK);
    CHECK(own_commit_hci(l, &repeat, 1100, &effect, &deadline) == OWN_OK);
    CHECK(effect.resend == 1 && deadline == 5100);
    own_submitted(l, &repeat, OWN_SENT, 1100);
    own_ack ack = {.event = OWN_CC, .opcode = 0x1005};
    CHECK(own_backend_receipt(l, &ack, 1200, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1200); /* Could be the late first reply: liveness only. */
    CHECK(own_status(l, probe, &state) == OWN_OK && state == OWN_CLOSED);
    CHECK(own_commit_hci(l, &repeat, 1200, &effect, &deadline) != OWN_OK);
    CHECK(own_request_undo(l, probe, 1200, &token) == OWN_NO_PROOF);
    CHECK(!own_activate(l));
    c.idle_probe = 0;
    CHECK(own_prepare(l, &c, &different) == OWN_NO_PROOF); /* No later data-bearing read. */
    c = (own_command){.opcode = 0x200c, .plen = 2, .params = {0, 0},
                      .ack_by = 4000, .terminal_by = 9000};
    CHECK(own_prepare(l, &c, &different) == OWN_OK);
    CHECK(own_next_hci(l, 1300, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 1300, &effect, &deadline) == OWN_OK);
    own_submitted(l, &token, OWN_SENT, 1300);
    CHECK(own_backend_receipt(l, &ack, 1400, &receipt) == OWN_RECEIPT_RETIRED);
    own_receive(l, &receipt, 1400); /* Surplus read must not confirm the scan disable. */
    CHECK(own_status(l, different, &state) == OWN_OK && state == OWN_SUBMITTED);
    CHECK(own_request_undo(l, different, 1400, &repeat) != OWN_OK);
    CHECK(!own_activate(l));
    ack.opcode = 0x200c;
    CHECK(own_backend_receipt(l, &ack, 1500, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1500);
    CHECK(own_next_hci(l, 1500, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 1500, &effect, &deadline) == OWN_OK);
    CHECK(effect.opcode == 0x2018 && !effect.resend); /* Still a one-shot state change. */
    own_submitted(l, &token, OWN_SENT, 1500);
    ack = (own_ack){.event = OWN_CC, .opcode = 0x2018, .random_len = 8, .random = {1}};
    CHECK(own_backend_receipt(l, &ack, 1600, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1600);
    CHECK(own_request_undo(l, different, 1600, &token) == OWN_NO_PROOF);
    c.idle_probe = 1;
    CHECK(own_prepare(l, &c, &different) == OWN_INVALID); /* No state-changing retry exception. */
    c = (own_command){.opcode = 0x1005, .idle_probe = 1,
                      .ack_by = 7000, .terminal_by = 9000};
    CHECK(own_reclaim(l, probe) == OWN_OK);
    CHECK(own_prepare(l, &c, &probe) == OWN_OK); /* Later liveness rounds are allowed. */
    CHECK(own_next_hci(l, 1700, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 1700, &effect, &deadline) == OWN_OK);
    own_submitted(l, &token, OWN_SENT, 1700);
    CHECK(own_release(l, SESSION_STOP, 1800));
    ack = (own_ack){.event = OWN_CC, .opcode = 0x1005};
    CHECK(own_backend_receipt(l, &ack, 1801, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1801); /* Stop may settle an ACK, never reopen admission. */
    CHECK(!own_activate(l));
    CHECK(own_commit_hci(l, &token, 1801, &effect, &deadline) != OWN_OK);
    CHECK(own_request_undo(l, probe, 1801, &token) != OWN_OK);
    CHECK(own_invariant(l) == NULL);
    own_free(l);
}

int main(void)
{
    log_open("/dev/null");
    receipt_authority(OWN_POLICY_NO_RESTORE);
    receipt_authority(OWN_POLICY_SINGLE_PRODUCER);
    g_tune.ps_light = 0; /* Marvell's path: no page-scan setting reads. */
    for (unsigned scenario = 0; scenario < 6; scenario++) idle_case(scenario, 1);
    unsigned alive = 0;
    for (unsigned seed = 1; seed <= 10000; seed++) alive += idle_case(6, seed);
    CHECK(alive >= 9990);
    printf("runtime query loss: seeds=10000 alive=%u checks=%u failures=%u\n",
           alive, checks, failures);
    log_close();
    return failures ? 1 : 0;
}
