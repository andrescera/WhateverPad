/* GPL-3.0-or-later. Identical Inquiry retries never turn refusal into authority. */
#include "sim.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures, mode, sends;
static long at[3];
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL inquiry retry mode=%u sends=%u: %s\n", mode, sends, #x); failures++; \
} } while (0)

static int command(void *ctx, unsigned op, const void *p, int n)
{
    if (op != OP_INQUIRY) return sim_model_command(ctx, op, p, n);
    CHECK(n == 5 && !memcmp(p, "\x33\x8b\x9e\x08\x00", 5));
    if (sends < 3) at[sends] = sim_now();
    sends++;
    if (mode == 5 && sends == 1) return 1;          /* the first send is never executed */
    if (mode == 5 && sends == 2) {
        /* The resend is refused; the older inquiry then ends; a surplus copy of
         * the refusal arrives in the same read batch as that Complete. */
        const unsigned char refused[] = {0x0c, 1, 1, 4};
        const unsigned char complete[] = {0};
        sim_inject_event(ctx, 0x0f, refused, sizeof refused);
        sim_schedule_event(ctx, 0x01, complete, sizeof complete, 300);
        sim_schedule_event(ctx, 0x0f, refused, sizeof refused, 301);
        return 1;
    }
    if (mode == 3 || (mode == 0 && sends == 1) || (mode == 4 && sends < 3)) return 1;
    if (sends > 1 && (mode == 1 || mode == 2)) {
        const unsigned char refused[] = {0x0c, 1, 1, 4};
        sim_inject_event(ctx, 0x0f, refused, sizeof refused);
        return 1;
    }
    if (mode == 1) sim_delay_events(ctx, 0x0f, 1, -1);
    if (mode == 2) sim_delay_events(ctx, 0x0f, 1, 1500);
    return sim_model_command(ctx, op, p, n);
}

static void authority(own_policy policy)
{
    own_ledger *l = own_new(policy, 1);
    own_command c = {.opcode = 0x0401, .plen = 5, .params = {0x33, 0x8b, 0x9e, 8, 0},
                     .inquiry_retry = 1, .ack_by = 6000, .terminal_by = 30000};
    own_ref inquiry, other;
    own_token first, repeat, token;
    own_effect effect;
    own_receipt receipt;
    own_state state;
    uint64_t deadline;
    CHECK(own_prepare(l, &c, &inquiry) == OWN_INVALID);
    CHECK(own_activate(l));
    CHECK(own_prepare(l, &c, &inquiry) == OWN_OK);
    CHECK(own_next_hci(l, 100, &first) == OWN_OK);
    CHECK(own_commit_hci(l, &first, 100, &effect, &deadline) == OWN_OK);
    own_submitted(l, &first, OWN_SENT, 100);
    CHECK(own_next_hci(l, 1100, &repeat) == OWN_OK);
    CHECK(own_commit_hci(l, &first, 1100, &effect, &deadline) != OWN_OK);
    CHECK(own_commit_hci(l, &repeat, 1100, &effect, &deadline) == OWN_OK);
    CHECK(effect.resend == 1 && deadline == 5100);
    own_submitted(l, &repeat, OWN_SENT, 1100);
    own_ack ack = {.event = OWN_CS, .opcode = 0x0401, .status = 0x0c};
    CHECK(own_backend_receipt(l, &ack, 1200, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1200);
    CHECK(own_status(l, inquiry, &state) == OWN_REFUSED && state == OWN_CLOSED);
    CHECK(own_request_undo(l, inquiry, 1200, &token) == OWN_NO_PROOF);
    CHECK(!own_activate(l));
    CHECK(own_prepare(l, &c, &other) == OWN_OK);
    CHECK(own_release(l, SESSION_STOP, 1201));
    CHECK(own_commit_hci(l, &repeat, 1202, &effect, &deadline) != OWN_OK);
    CHECK(own_invariant(l) == NULL);
    own_free(l);

    l = own_new(policy, 1);
    CHECK(own_activate(l));
    CHECK(own_prepare(l, &c, &inquiry) == OWN_OK);
    CHECK(own_next_hci(l, 100, &first) == OWN_OK);
    CHECK(own_commit_hci(l, &first, 100, &effect, &deadline) == OWN_OK);
    own_submitted(l, &first, OWN_SENT, 100);
    CHECK(own_next_hci(l, 1100, &repeat) == OWN_OK);
    CHECK(own_commit_hci(l, &repeat, 1100, &effect, &deadline) == OWN_OK);
    own_submitted(l, &repeat, OWN_SENT, 1100);
    CHECK(own_backend_receipt(l, &ack, 1200, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, 1200);
    c = (own_command){.opcode = 0x200c, .plen = 2, .ack_by = 5000, .terminal_by = 9000};
    c.inquiry_retry = 1;
    CHECK(own_prepare(l, &c, &other) == OWN_INVALID);
    c.inquiry_retry = 0;
    CHECK(own_prepare(l, &c, &other) == OWN_OK);
    CHECK(own_next_hci(l, 1300, &token) == OWN_OK);
    CHECK(own_commit_hci(l, &token, 1300, &effect, &deadline) == OWN_OK);
    own_submitted(l, &token, OWN_SENT, 1300);
    for (unsigned status = 0; status <= 12; status += 12) {
        ack.status = (uint8_t)status;
        CHECK(own_backend_receipt(l, &ack, 1400, &receipt) == OWN_RECEIPT_RETIRED);
        own_receive(l, &receipt, 1400);
        CHECK(own_status(l, other, &state) == OWN_OK && state == OWN_SUBMITTED);
    }
    CHECK(own_request_undo(l, other, 1400, &repeat) != OWN_OK);
    CHECK(!own_activate(l) && own_invariant(l) == NULL);
    own_free(l);
}

int main(void)
{
    log_open("/dev/null");
    authority(OWN_POLICY_NO_RESTORE);
    authority(OWN_POLICY_SINGLE_PRODUCER);
    g_tune.ps_light = 0;
    for (mode = 0; mode < 6; mode++) {
        sim_pad pad = {.seed = 1};
        sim_t *s = sim_new(&pad);
        attrib_reset(); sends = 0;
        sim_output_hooks(s, command, NULL);
        host_t *h = sim_open_host(s, "build/inquiry-retry-absent.db", NULL);
        CHECK(h != NULL);
        if (!h) { sim_free(s); continue; }
        CHECK(host_pair(h, 60));
        long end = sim_now() + 6000;
        while (host_admits(h) && sim_now() < end) host_poll(h, 20);
        CHECK(sends == (mode >= 3 ? 3u : 2u));
        CHECK(at[1] - at[0] == 1000);
        if (mode == 3 || mode == 4) CHECK(at[2] - at[1] == 1000);
        /* A retired surplus Status must not restart the refusal wait: the next
         * Inquiry follows the older one's Complete at once. */
        if (mode == 5) CHECK(at[2] - at[1] < 1000 && !h->inq_retry_at && host_pairing_left(h) > 0);
        CHECK(host_admits(h) == (mode != 3));
        if (mode != 3) {
            own_state state;
            own_result result = own_status(host_ownership(h), h->inquiry, &state);
            if (mode == 1 || mode == 2)
                CHECK(result == OWN_REFUSED && state == OWN_CLOSED);
            own_token token;
            CHECK(own_request_undo(host_ownership(h), h->inquiry, sim_now(), &token) == OWN_NO_PROOF);
            CHECK(!own_activate(host_ownership(h)));
            CHECK(!attrib_get()->timeout && !attrib_get()->unanswered);
        }
        CHECK(own_invariant(host_ownership(h)) == NULL);
        host_close(h); sim_free(s);
    }
    printf("inquiry retries: policy=%d checks=%u failures=%u\n",
           WHATEVERPAD_ATTRIB_POLICY, checks, failures);
    log_close();
    return failures ? 1 : 0;
}
