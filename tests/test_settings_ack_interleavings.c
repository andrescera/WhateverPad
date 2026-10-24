/* GPL-3.0-or-later. Appendix H scan replacement; link CORE + tests/sim.c.
 * Use identical policy defines for all translation units. Only the deliberately
 * SINGLE_PRODUCER ack_by_opcode mutant should fail the wire-effect oracle. */
#include "sim.h"
#include "../src/host_int.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
static const char *test_name;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, test_name, #c); \
    failures++; \
} } while (0)

enum { SCAN = 0x0c1a, RAND = 0x2018, CC = 0x0e };
typedef struct {
    sim_t *sim;
    sim_origin old;
    unsigned char old_cc[4];
    int armed, duplicate, closing, baseline, capable;
    unsigned startup_writes, startup_rands, startup_answers, pending_rand;
    unsigned attempts, target, refusal_generated, refusal_delivered, copies;
    unsigned phase_rands, phase_answers, closing_scan, forbidden;
    long target_at, rand_at;
} fixture;
static fixture *active;

static void observe(void *ctx, enum sim_observation kind, sim_origin origin,
                    unsigned code, const unsigned char *p, int n)
{
    fixture *f = ctx;
    if (kind == SIM_COMMAND) {
        CHECK(origin.invocation && !origin.foreign && !origin.copy);
        if (code == 0x0c03 || code == 0x0c56 || code == 0x0402 ||
            code == 0x0408 || code == 0x041a || code == 0x200e) f->forbidden++;
        if (code == SCAN) {
            if (f->closing) f->closing_scan++;
            else if (f->armed) {
                f->attempts++;
                f->target = origin.invocation;
                f->target_at = sim_now();
                CHECK(n == 1 && p[0] == 0);
            } else {
                f->startup_writes++;
                CHECK(f->baseline && n == 1 && p[0] == 2);
            }
        }
        if (code == RAND && !f->closing) {
            if (f->armed) { f->phase_rands++; f->rand_at = sim_now(); }
            else {
                CHECK(!f->pending_rand);
                f->pending_rand = origin.invocation;
                f->startup_rands++;
            }
        }
        return;
    }
    if (code != CC || n < 4) return;
    unsigned op = le16(p + 1);
    if (kind == SIM_GENERATED) {
        if (f->armed && !origin.copy && origin.invocation == f->target && op == SCAN) {
            CHECK(n == 4 && p[3] == 0x0c && sim_scan(f->sim) == 2);
            f->refusal_generated++;
        }
        return;
    }
    if (kind != SIM_DELIVERED) return;
    if (origin.copy) {
        CHECK(f->armed && op == SCAN && origin.invocation == f->old.invocation);
        CHECK(n == 4 && memcmp(p, f->old_cc, 4) == 0);
        f->copies++;
        return;
    }
    if (!f->armed && !f->closing) {
        if (op == 0x1002) {
            CHECK(n == 68 && !p[3] && (p[4 + 27] & 0x80));
            f->capable = n == 68 && !p[3] && (p[4 + 27] & 0x80);
        }
        if (op == 0x0c19 && !f->baseline) {
            CHECK(n == 5 && !p[3] && p[4] == 0);
            f->baseline = 1;
        }
        if (op == SCAN) {
            CHECK(n == 4 && !p[3] && !origin.foreign);
            if (n == 4 && !p[3]) {
                f->old = origin;
                memcpy(f->old_cc, p, 4);
            }
        }
        if (op == RAND) {
            CHECK(n == 12 && !p[3] && origin.invocation == f->pending_rand);
            f->startup_answers++;
            f->pending_rand = 0;
        }
    } else if (f->armed) {
        if (op == SCAN && origin.invocation == f->target) {
            CHECK(n == 4 && p[3] == 0x0c && sim_now() >= f->target_at + 100);
            f->refusal_delivered++;
        }
        if (op == RAND) f->phase_answers++;
    }
}

static int command(void *ctx, unsigned op, const void *params, int n)
{
    fixture *f = active;
    const unsigned char *p = params;
    if (f->armed && !f->closing) {
        if (op == SCAN && n == 1 && !p[0] && !f->attempts) {
            if (f->duplicate) {
                sim_origin copy = f->old;
                copy.copy = 1;
                /* Queue the actual old bytes before arming the real-CC fault. */
                sim_causal_event(ctx, copy, CC, f->old_cc, 4, 0);
            }
            sim_delay_events(ctx, CC, 1, 100);
            sim_refuse_command(ctx, SCAN, 0x0c);
        } else if (op == RAND) sim_delay_events(ctx, CC, 1, 200);
    }
    return sim_model_command(ctx, op, params, n);
}

static void run(int duplicate)
{
    fixture f = {.duplicate = duplicate};
    char dir[] = "/tmp/whateverpad-settings-XXXXXX", db[128];
    host_t *h = NULL;
    sim_pad pad = {.mode = SIM_WAIT_PAIRING, .seed = 17};
    test_name = duplicate ? "copied success" : "no-copy control";
    char *made = mkdtemp(dir);
    CHECK(made != NULL);
    if (!made) return;
    snprintf(db, sizeof db, "%s/bonds", dir);
    active = &f;
    attrib_reset();
    f.sim = sim_new(&pad);
    CHECK(f.sim != NULL);
    if (!f.sim) goto clean;
    hci_t io = sim_hci(f.sim);
    CHECK(sim_scan(f.sim) == 0);
    sim_observe(f.sim, observe, &f);
    sim_output_hooks(f.sim, command, NULL);
    h = host_prepare(io, db, NULL);
    CHECK(h != NULL);
    if (!h) { io.ops->close(io.ctx); goto free_sim; }
    sim_bind_ownership(f.sim, host_ownership(h));
    int started = host_start(h);
    CHECK(started);
    CHECK(f.baseline && f.capable && f.startup_writes == 1 && f.old.invocation);
    /* All actually submitted startup fences must have arrived before arming. */
    CHECK(!f.pending_rand && f.startup_rands == f.startup_answers);
    CHECK(sim_scan(f.sim) == 2);
    if (started) {
        unsigned char off = 0;
        f.armed = 1;
        (void)hci_sync(h, SCAN, &off, 1); /* A8: forward success is not a lease. */
    }
    f.closing = 1;
    long stopped = sim_now();
    host_close(h);
    CHECK(sim_now() - stopped <= 2000);
    CHECK(f.attempts == 1 && f.refusal_generated == 1);
    CHECK(f.copies == (unsigned)duplicate && !f.phase_answers);
    if (duplicate) CHECK(!f.refusal_delivered && stopped < f.target_at + 100);
    else CHECK(f.refusal_delivered == 1); /* Stop before the continuity fence. */
    if (f.phase_rands) {
        CHECK(f.target_at + 100 < f.rand_at + 200); /* B1: refusal before Rand. */
        CHECK(stopped < f.rand_at + 200);
    }
    CHECK(f.closing_scan == 0); /* Independent oracle: actual wire invocations. */
    CHECK(f.forbidden == 0 && sim_scan(f.sim) == 2);
free_sim:;
    sim_lifetime life = sim_resources(f.sim);
    CHECK(life.opens == 1 && life.closes == 1 && life.uninits == 1);
    CHECK(!life.event_reads && !life.acl_reads);
    sim_free(f.sim);
clean:
    ph_clock_hook = NULL;
    active = NULL;
    static const char *suffixes[] = {"", ".new", ".irk", ".irk.new"};
    for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char path[160];
        snprintf(path, sizeof path, "%s%s", db, suffixes[i]);
        CHECK(unlink(path) == 0 || errno == ENOENT);
    }
    CHECK(rmdir(dir) == 0);
}

int main(void)
{
    run(0);
    run(1);
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
