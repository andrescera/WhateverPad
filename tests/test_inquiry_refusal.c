/* GPL-3.0-or-later. Inquiry refusal is not an unanswered command or a lease. */
#include "sim.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL inquiry: %s\n", #x); failures++; \
} } while (0)

static unsigned inquiries, terminal_expected, stray, refusals;

static int inquiry_command(void *ctx, unsigned op, const void *p, int n)
{
    if (op == OP_INQUIRY && ++inquiries <= refusals && !stray) {
        const unsigned char refused[] = {0x0c, 1, 0x01, 0x04};
        sim_inject_event(ctx, 0x0f, refused, sizeof refused);
        if (terminal_expected == 3) {
            /* The system's own inquiry keeps running; its result for the pad reaches us. */
            const unsigned char pad_result[15] = {1, 1, 2, 3, 4, 5, 6,
                                                  1, 0, 0x08, 0x25, 0x00, 0, 0, 0xb0};
            sim_schedule_event(ctx, 0x22, pad_result, sizeof pad_result, 700);
        } else if (terminal_expected) {
            const unsigned char status[] = {0};
            unsigned delay = terminal_expected == 2 ? 1400 : 10240;
            const unsigned char result[15] = {1, 0xe7, 0xa0, 1, 2, 3, 4,
                                              1, 0, 0x3c, 4, 0x0c, 0, 0, 0xb0};
            sim_schedule_event(ctx, 0x01, status, 1, (int)delay);
            if (terminal_expected == 2)
                sim_schedule_event(ctx, 0x22, result, sizeof result, (int)delay + 1);
        }
        return 1;
    }
    return sim_model_command(ctx, op, p, n);
}

static void inquiry_case(unsigned complete, unsigned unsolicited, unsigned cancel, unsigned n)
{
    sim_pad pad = {.addr = {1, 2, 3, 4, 5, 6}, .cod = 0x002508,
                   .vid = 0x054c, .pid = 0x05c4, .seed = 1};
    sim_t *s = sim_new(&pad);
    attrib_reset();
    inquiries = 0; terminal_expected = complete; stray = unsolicited; refusals = n;
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
        const unsigned codes[] = {0x07, 0x10, 0x12, 0x14, 0x1a, 0x20, 0x21};
        const int lengths[] = {255, 1, 8, 6, 1, 7, 21};
        const unsigned char metadata[255] = {0};
        for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++)
            sim_inject_event(s, (unsigned char)codes[i], metadata, lengths[i]);
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
    long limit = n <= 3 ? 15000L * (long)n : 14000;
    while (host_admits(h) && !sim_calls(s) && sim_now() - begin < limit)
        host_poll(h, 20);
    CHECK(host_admits(h) && !host_transport_lost(h));
    CHECK(sim_calls(s) == 1 && host_pairing_left(h) > 0);
    CHECK(inquiries == (stray || complete == 3 ? 1u : n + 1u));
    CHECK(!attrib_get()->timeout && !attrib_get()->overflow);
    CHECK(own_invariant(host_ownership(h)) == NULL);
    host_close(h);
    sim_free(s);
}

/* The system's own Inquiry Cancel reaches our reads while we pair. The recorded event is
 * a Command Complete, status 0. With nothing in flight it is another owner's reply; while
 * our Inquiry's Status is still awaited, the reply we wait for may be the one confused.
 * (Right behind that Status it would find the next command in flight, also fatal by
 * design: only an idle moment is covered.) The host's own counter check must stay quiet
 * too, so the case runs the whole host, and a witness proves the reply was delivered. */
static unsigned foreign_pending;
static long last_command;
static const unsigned char cancelled[] = {1, 0x02, 0x04, 0x00};
static int foreign_command(void *ctx, unsigned op, const void *p, int n)
{
    last_command = sim_now();
    if (op != OP_INQUIRY) return sim_model_command(ctx, op, p, n);
    inquiries++;
    if (foreign_pending) sim_inject_event(ctx, 0x0e, cancelled, sizeof cancelled);
    return sim_model_command(ctx, op, p, n);
}

static void foreign_case(unsigned pending)
{
    sim_pad pad = {.addr = {1, 2, 3, 4, 5, 6}, .cod = 0x002508,
                   .vid = 0x054c, .pid = 0x05c4, .seed = 1};
    sim_t *s = sim_new(&pad);
    attrib_reset();
    inquiries = 0; foreign_pending = pending; last_command = 0;
    sim_output_hooks(s, foreign_command, NULL);
    host_t *h = sim_open_host(s, "build/inquiry-foreign-absent.db", NULL);
    CHECK(h != NULL);
    if (!h) { sim_free(s); return; }
    long begin = sim_now();
    CHECK(host_pair(h, 60));
    if (pending) {
        while (host_admits(h) && !sim_calls(s) && sim_now() - begin < 14000)
            host_poll(h, 20);
        CHECK(!host_admits(h) || host_transport_lost(h));
    } else {
        /* Pad called, then half a second with no command: nothing is awaited. */
        while (host_admits(h) && sim_now() - begin < 14000 &&
               !(sim_calls(s) && sim_now() - last_command >= 500))
            host_poll(h, 20);
        CHECK(host_admits(h) && sim_calls(s) == 1 && sim_ledger_dropped(s) == 0);
        sim_inject_event(s, 0x0e, cancelled, sizeof cancelled);
        for (unsigned step = 0; step < 10 && host_admits(h); step++) host_poll(h, 20);
        CHECK(sim_ledger_dropped(s) == 1);          /* delivered, and ignored */
        CHECK(host_admits(h) && !host_transport_lost(h));
        CHECK(inquiries == 1);
        CHECK(own_invariant(host_ownership(h)) == NULL);
    }
    host_close(h);
    sim_free(s);
}

int main(void)
{
    log_open("/dev/null");
    g_tune.ps_light = 0;
    inquiry_case(1, 0, 0, 1); /* Old inquiry's terminal arrives. */
    inquiry_case(0, 0, 0, 1); /* Its terminal was taken by the system driver. */
    inquiry_case(0, 1, 0, 1); /* Unknown event, stray terminal, unowned request. */
    inquiry_case(0, 0, 1, 1); /* Pair cancellation/restart preserves backoff. */
    inquiry_case(2, 0, 0, 1); /* Owner order: refusal, old Complete at 1.4 s, old results. */
    /* A system inquiry that restarts itself refuses us on every cycle, not once. */
    inquiry_case(2, 0, 0, 3); /* Three refusals, each followed by the old Complete. */
    inquiry_case(0, 0, 0, 3); /* Three refusals, every Complete lost: timed waits. */
    inquiry_case(3, 0, 0, 1000); /* Always refused: its results still find the pad. */
    foreign_case(0); /* Console death 2: a foreign Inquiry Cancel Complete, nothing pending. */
    foreign_case(1); /* The same event while our Inquiry's Status is awaited stays fatal. */
    printf("inquiry refusal: policy=%d checks=%u failures=%u\n",
           WHATEVERPAD_ATTRIB_POLICY, checks, failures);
    log_close();
    return failures ? 1 : 0;
}
