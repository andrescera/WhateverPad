/* GPL-3.0-or-later. Full-host timing controls, not just ledger deadline fields. */
#define main setup_retry_main
#include "test_setup_retry.c"
#undef main

static unsigned query_sends, rand_sends;
static int final_delay;
static long query_first, query_last;

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
    printf("setup wait: checks=%u failures=%u\n", checks, failures);
    log_close();
    return failures ? 1 : 0;
}
