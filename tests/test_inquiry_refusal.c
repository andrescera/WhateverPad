/* GPL-3.0-or-later. Inquiry refusal is not an unanswered command or a lease. */
#include "sim.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL inquiry: %s\n", #x); failures++; \
} } while (0)

static unsigned inquiries, terminal_expected, stray;

static int inquiry_command(void *ctx, unsigned op, const void *p, int n)
{
    if (op == OP_INQUIRY && ++inquiries == 1 && !stray) {
        const unsigned char refused[] = {0x0c, 1, 0x01, 0x04};
        sim_inject_event(ctx, 0x0f, refused, sizeof refused);
        if (terminal_expected) {
            const unsigned char status[] = {0};
            sim_schedule_event(ctx, 0x01, status, 1, 10240);
        }
        return 1;
    }
    return sim_model_command(ctx, op, p, n);
}

static void inquiry_case(unsigned complete, unsigned unsolicited, unsigned cancel)
{
    sim_pad pad = {.addr = {1, 2, 3, 4, 5, 6}, .cod = 0x002508,
                   .vid = 0x054c, .pid = 0x05c4, .seed = 1};
    sim_t *s = sim_new(&pad);
    attrib_reset();
    inquiries = 0; terminal_expected = complete; stray = unsolicited;
    sim_output_hooks(s, inquiry_command, NULL);
    host_t *h = sim_open_host(s, "build/inquiry-refusal-absent.db", NULL);
    CHECK(h != NULL);
    if (!h) { sim_free(s); return; }
    long begin = sim_now();
    CHECK(host_pair(h, 60));
    if (stray) {
        const unsigned char status[] = {0};
        const unsigned char unknown[] = {0x7a};
        const unsigned char native[] = {0xa1, 2, 3, 4, 5, 6};
        sim_inject_event(s, 0xff, unknown, sizeof unknown);
        sim_inject_event(s, 0x01, status, sizeof status);
        sim_peer_request(s, native, 0x002508);
    } else {
        own_state state = OWN_FREE;
        for (unsigned step = 0; step < 10; step++) {
            host_poll(h, 20);
            if (own_status(host_ownership(h), h->inquiry, &state) == OWN_REFUSED) break;
        }
        CHECK(own_status(host_ownership(h), h->inquiry, &state) == OWN_REFUSED);
        CHECK(state == OWN_CLOSED && host_admits(h));
        own_token token;
        CHECK(own_request_undo(host_ownership(h), h->inquiry, sim_now(), &token) == OWN_NO_PROOF);
        CHECK(!own_activate(host_ownership(h)));
        if (cancel) {
            CHECK(host_pair(h, 0));
            host_poll(h, 20);
            CHECK(host_pair(h, 60));
            long settle = sim_now() + 500;
            while (host_admits(h) && sim_now() < settle) host_poll(h, 20);
            CHECK(inquiries == 1); /* Cancelling Pair cannot bypass the refusal wait. */
        }
    }
    while (host_admits(h) && !sim_calls(s) && sim_now() - begin < 14000)
        host_poll(h, 20);
    CHECK(host_admits(h) && !host_transport_lost(h));
    CHECK(sim_calls(s) == 1 && host_pairing_left(h) > 0);
    CHECK(inquiries == (stray ? 1u : 2u));
    CHECK(!attrib_get()->timeout && !attrib_get()->overflow);
    CHECK(own_invariant(host_ownership(h)) == NULL);
    host_close(h);
    sim_free(s);
}

int main(void)
{
    log_open("/dev/null");
    g_tune.ps_light = 0;
    inquiry_case(1, 0, 0); /* Old inquiry's terminal arrives. */
    inquiry_case(0, 0, 0); /* Its terminal was taken by the system driver. */
    inquiry_case(0, 1, 0); /* Unknown event, stray terminal, unowned request. */
    inquiry_case(0, 0, 1); /* Pair cancellation/restart preserves backoff. */
    printf("inquiry refusal: policy=%d checks=%u failures=%u\n",
           WHATEVERPAD_ATTRIB_POLICY, checks, failures);
    log_close();
    return failures ? 1 : 0;
}
