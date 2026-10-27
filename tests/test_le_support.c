/* GPL-3.0-or-later. Feature conclusions require an acknowledged answer. */
#include "sim.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include <stdio.h>

static unsigned checks, failures, mode, sends;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL LE mode=%u: %s\n", mode, #x); failures++; \
} } while (0)

static int command(void *ctx, unsigned op, const void *p, int n)
{
    if (op == 0x2001) {
        sends++;
        if (mode == 1) return 1;
        if (mode >= 2) sim_refuse_command(ctx, op, mode == 2 ? 1 : mode == 3 ? 0x0c : 0x11);
    }
    return sim_model_command(ctx, op, p, n);
}

int main(void)
{
    log_open("/dev/null");
    for (mode = 0; mode < 5; mode++) {
        sim_pad pad = {.seed = 1};
        sim_t *s = sim_new(&pad);
        attrib_reset(); sends = 0;
        sim_output_hooks(s, command, NULL);
        host_t *h = host_prepare(sim_hci(s), "build/le-support-absent.db", NULL);
        CHECK(h != NULL);
        if (!h) { sim_free(s); continue; }
        sim_bind_ownership(s, host_ownership(h));
        int ready = host_start(h);
        CHECK(ready == (mode != 1));
        CHECK(sends == (mode == 1 ? 3u : 1u));
        CHECK(h->le_support == (mode == 0 ? 1 : mode == 2 || mode == 4 ? -1 : 0));
        CHECK(h->le_ok == (mode == 0));
        CHECK(own_invariant(host_ownership(h)) == NULL);
        host_close(h); sim_free(s);
    }
    printf("LE conclusions: policy=%d checks=%u failures=%u\n",
           WHATEVERPAD_ATTRIB_POLICY, checks, failures);
    log_close();
    return failures ? 1 : 0;
}
