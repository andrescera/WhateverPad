/* GPL-3.0-or-later. Full-host timing controls, not just ledger deadline fields. */
#define main setup_retry_main
#include "test_setup_retry.c"
#undef main

static unsigned query_sends, rand_sends;
static int final_delay;
static long query_first, query_last;
static host_t *frozen_host;
static int (*normal_pump)(void *, int);
static unsigned frozen_pumps, frozen_rands, frozen_sends, stop_after;
static int freeze_clock, freeze_at_start;
static long frozen_at;

static int queued_command(void *ctx, unsigned op, const void *p, int n)
{
    if (op == 0x2018 && ++rand_sends == 1)
        sim_delay_events(ctx, 0x0e, 1, 200);
    if (op == 0x0c55) {
        query_sends++;
        if (query_sends == 1) query_first = sim_now();
        query_last = sim_now();
        sim_delay_events(ctx, 0x0e, 1, query_sends <= 2 ? -1 : final_delay);
    }
    return sim_model_command(ctx, op, p, n);
}

static int frozen_pump(void *ctx, int timeout)
{
    if (!freeze_clock) return normal_pump(ctx, timeout);
    if (++frozen_pumps == stop_after)
        own_release(host_ownership(frozen_host), SESSION_STOP, (uint64_t)sim_now());
    return 0; /* A watchdog Stop bounds the rejected implementation, not a sleep. */
}

static int clock_command(void *ctx, unsigned op, const void *p, int n)
{
    frozen_sends++;
    if (freeze_at_start) return 1;
    if (op == 0x2018 && ++frozen_rands == 4) {
        freeze_clock = 1;
        frozen_at = sim_now();
        return 1; /* Last fence: exercise activation's wait, not command_wait. */
    }
    return sim_model_command(ctx, op, p, n);
}

static void stalled_clock(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        sim_t *s;
        host_t *h = open_host(&s);
        if (!h) { CHECK(0); return; }
        hci_ops ops = *h->hci.ops;
        normal_pump = ops.pump;
        ops.pump = frozen_pump;
        h->hci.ops = &ops;
        frozen_host = h;
        frozen_pumps = frozen_rands = frozen_sends = 0;
        freeze_clock = freeze_at_start = mode != 1;
        frozen_at = sim_now();
        stop_after = mode == 2 ? 4 : 129;
        sim_output_hooks(s, clock_command, NULL);
        CHECK(!host_start(h));
        printf("CLOCK_WAIT mode=%u pumps=%u elapsed=%ld external_stop=%d\n",
               mode, frozen_pumps, sim_now() - frozen_at, frozen_pumps == stop_after);
        CHECK(frozen_pumps <= 64 && sim_now() == frozen_at && !host_admits(h));
        if (mode == 1) CHECK(frozen_rands == 4);
        else CHECK(frozen_sends == 1);
        if (mode == 2) CHECK(frozen_pumps == 4);
        h->dead = 1;
        close_host(h, s);
    }
}

int main(void)
{
    log_open("/dev/null");
    for (unsigned mode = 0; mode < 4; mode++) {
        sim_t *s;
        host_t *h = open_host(&s);
        if (!h) return 1;
        query_sends = rand_sends = 0;
        static const int delays[] = {0, 2700, 2900, 3100};
        final_delay = delays[mode];
        sim_output_hooks(s, queued_command, NULL);
        int ready = host_start(h);
        printf("QUEUED_WINDOW final_delay=%d ready=%d first=%ld third=%ld end=%ld sends=%u\n",
               final_delay, ready, query_first, query_last, sim_now(), query_sends);
        CHECK(ready == (final_delay < 3000));
        CHECK(query_sends == 3 && query_last - query_first == 2000);
        if (!ready) CHECK(!host_admits(h));
        close_host(h, s);
    }
    stalled_clock();
    printf("setup wait: checks=%u failures=%u\n", checks, failures);
    log_close();
    return failures ? 1 : 0;
}
