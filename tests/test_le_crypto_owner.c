/* GPL-3.0-or-later. Link CORE only, with identical policy defines throughout.
 * Wire oracle: a completed P256 command still belongs to its consuming pairing.
 * No peer replies, crypto arithmetic, virtual pads, or host-private inspection. */
#include "../src/host.h"
#include "sim_lifetime.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned checks;
static int failures;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)
typedef struct { unsigned char bytes[64][80]; int len[64], head, count; } queue;
typedef struct {
    own_ledger *ledger;
    queue events, acl;
    unsigned sends, creates, links, p256, terminals, requests[2], forbidden, closes;
    unsigned second_fenced, scanning;
    unsigned terminal_mode;
    uint64_t random;
} backend;
static long clock_ms = 5000;
static long inquiry_end;
static long clock_now(void) { return clock_ms; }
static const unsigned char peers[2][6] = {
    {0x11, 0x22, 0x33, 0x44, 0x55, 0xc0},
    {0x12, 0x22, 0x33, 0x44, 0x55, 0xc0}
};

static void push(queue *q, const unsigned char *p, int n)
{
    assert(q->count < 64 && n > 0 && n <= 80);
    int slot = (q->head + q->count++) % 64;
    memcpy(q->bytes[slot], p, (size_t)n); q->len[slot] = n;
}
static int pop(queue *q, unsigned char *p, int max)
{
    if (!q->count) return 0;
    int n = q->len[q->head];
    assert(n <= max);
    memcpy(p, q->bytes[q->head], (size_t)n);
    q->head = (q->head + 1) % 64; q->count--;
    return n;
}
static void event(backend *b, unsigned code, const unsigned char *p, int n)
{
    unsigned char e[80] = {(unsigned char)code, (unsigned char)n};
    assert(n <= 78);
    memcpy(e + 2, p, (size_t)n); push(&b->events, e, n + 2);
}
static void status(backend *b, unsigned op)
{
    unsigned char r[4] = {0, 1, 0, 0};
    put16(r + 2, op); event(b, 0x0f, r, 4);
}
static void command(backend *b, unsigned op, const unsigned char *p, int plen)
{
    unsigned char r[66] = {0}, cc[70] = {1};
    int n = 1;
    b->sends++;
    if (op == 0x0c03 || op == 0x0402 || op == 0x0408 ||
        op == 0x041a || op == 0x200e) b->forbidden++;
    switch (op) {
    case 0x1002: n = 65; r[28] = 0x80; break;
    case 0x0c55: n = 2; r[1] = 1; break;
    case 0x0c19: n = 2; break;
    case 0x1005:
        n = 8; put16(r + 1, 1021); r[3] = 64; put16(r + 4, 8); break;
    case 0x2002: n = 4; put16(r + 1, 251); r[3] = 6; break;
    case 0x1009: n = 7; r[1] = 0x10; r[6] = 0xc0; break;
    case 0x2018:
        n = 9; b->random++;
        for (int i = 0; i < 8; i++) r[i + 1] = (unsigned char)(b->random >> (8 * i));
        break;
    case 0x200c: CHECK(plen == 2); b->scanning = p[0]; break;
    case 0x0401: status(b, op); inquiry_end = clock_ms + 10000; return;
    case 0x200d: {
        CHECK(plen == 25 && b->creates < 2 && !b->scanning);
        if (plen != 25 || b->creates >= 2) return;
        CHECK(p[5] == 0 && !memcmp(p + 6, peers[b->creates], 6));
        status(b, op);
        r[0] = 1; put16(r + 2, 0x51 + b->creates++);
        r[5] = p[5]; memcpy(r + 6, p + 6, 6);
        put16(r + 12, 12); put16(r + 16, 200);
        if (b->terminal_mode == 1) r[5] ^= 1; /* Same bytes, wrong LE address type. */
        if (b->terminal_mode == 2) {
            unsigned char classic[11] = {0};
            put16(classic + 1, le16(r + 2));
            memcpy(classic + 3, p + 6, 6);
            classic[9] = 1;
            event(b, 3, classic, sizeof classic);
            return;
        }
        event(b, 0x3e, r, 19); return;
    }
    case 0x2025:
        b->p256++; status(b, op);
        r[0] = 8; memset(r + 2, 0x11, 64); event(b, 0x3e, r, 66); return;
    case 0x0406:
        CHECK(plen == 3); status(b, op);
        memcpy(r + 1, p, 2); r[3] = 0x16; event(b, 5, r, 4); return;
    default: break; /* Shared settings all return ordinary Command Complete. */
    }
    put16(cc + 1, op); memcpy(cc + 3, r, (size_t)n);
    event(b, 0x0e, cc, n + 3);
}
static own_submit_result submit(void *ctx, const hci_submission *s)
{
    backend *b = ctx;
    if (s->kind == HCI_SUBMIT_COMMAND) {
        own_effect e; uint64_t deadline;
        const own_token *t = s->token.command;
        if (own_commit_hci(b->ledger, t, clock_ms, &e, &deadline) != OWN_OK)
            return OWN_NOT_SENT;
        command(b, e.opcode, e.params, e.plen);
        own_submitted(b->ledger, t, OWN_SENT, clock_ms);
        if (!e.resend) attrib_submit(e.opcode, 1, clock_ms);
        else attrib_resend(e.opcode);
        attrib_deadline(e.opcode, (long)deadline);
    } else {
        own_acl_effect e;
        own_acl_lane *lane = s->token.acl.lane;
        const own_acl_token *t = s->token.acl.token;
        if (own_acl_commit(b->ledger, lane, t, clock_ms, &e) != OWN_OK)
            return OWN_NOT_SENT;
        b->sends++;
        unsigned handle = le16(e.packet) & 0xfff;
        CHECK(handle == 0x51 || handle == 0x52);
        if (e.len >= 9 && le16(e.packet + 6) == 6 && e.packet[8] == 1) {
            CHECK(e.len == 15 && le16(e.packet + 2) == 11 && le16(e.packet + 4) == 7);
            if (handle == 0x51 || handle == 0x52) b->requests[handle - 0x51]++;
        }
        unsigned char completed[5] = {1};
        put16(completed + 1, handle); put16(completed + 3, 1);
        event(b, 0x13, completed, 5);
        own_acl_submitted(lane, t, OWN_SENT, clock_ms);
    }
    return OWN_SENT;
}
static int pump(void *ctx, int timeout)
{
    backend *b = ctx;
    (void)timeout; clock_ms++;
    if (inquiry_end && clock_ms >= inquiry_end) {
        unsigned char done = 0;
        inquiry_end = 0;
        event(b, 1, &done, 1);
    }
    own_tick(b->ledger, clock_ms); attrib_poll(clock_ms);
    return b->events.count || b->acl.count;
}
static int next_event(void *ctx, unsigned char *p, int max)
{
    backend *b = ctx;
    int n = pop(&b->events, p, max);
    if (!n) return 0;
    sim_ownership_event(b->ledger, p, n, clock_ms);
    if (p[0] == 0x3e && p[2] == 1) b->links++;
    if (p[0] == 0x3e && p[2] == 8) b->terminals++;
    if (p[0] == 0x0e && le16(p + 3) == 0x2018 && b->links == 2)
        b->second_fenced = 1;
    return n;
}
static int next_acl(void *ctx, unsigned char *p, int max)
{ return pop(&((backend *)ctx)->acl, p, max); }
static void close_transport(void *ctx) { ((backend *)ctx)->closes++; }
static const hci_ops ops = {
    .pump = pump, .next_event = next_event, .next_acl = next_acl,
    .close = close_transport, .op_submit = submit
};
static void advertise(backend *b, unsigned peer)
{
    unsigned char r[23] = {2, 1, 0, 0};
    static const unsigned char ad[] = {2, 1, 6, 3, 0x19, 0xc4, 3, 3, 3, 0x12, 0x18};
    memcpy(r + 4, peers[peer], 6); r[10] = sizeof ad;
    memcpy(r + 11, ad, sizeof ad); r[22] = 0xc0;
    event(b, 0x3e, r, sizeof r);
}
static void settle(host_t *h, backend *b)
{
    for (int i = 0; i < 64; i++) {
        unsigned before = b->sends;
        host_poll(h, 0);
        if (!b->events.count && !b->acl.count && b->sends == before) return;
    }
    CHECK(0); /* Bounded protocol quiescence, not a wall-clock delay. */
}
int main(void)
{
    backend b = {0};
    char dir[] = "/tmp/whateverpad-crypto-owner-XXXXXX", db[128];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(db, sizeof db, "%s/bonds", dir);
    ph_clock_hook = clock_now; attrib_reset();
    host_t *h = host_prepare((hci_t){&ops, &b}, db, NULL);
    CHECK(h != NULL);
    if (h) {
        b.ledger = host_ownership(h);
        int started = host_start(h); CHECK(started);
        if (started) {
            CHECK(host_pair(h, 60)); settle(h, &b);
            advertise(&b, 0);
            for (int i = 0; i < 1000 && !b.requests[0]; i++) host_poll(h, 0);
            CHECK(b.links == 1 && b.p256 == 1 && b.terminals == 1 && b.requests[0] == 1);
            advertise(&b, 1);
            for (int i = 0; i < 1000 && !b.second_fenced; i++) host_poll(h, 0);
            settle(h, &b);
            CHECK(b.creates == 2 && b.links == 2 && b.second_fenced);
            CHECK(b.p256 == 1 && b.terminals == 1);
            CHECK(b.requests[0] == 1 && b.requests[1] == 0);
            /* Actual inbound SMP Pairing Failed (opcode 05, reason 05). */
            unsigned char failed[] = {0x51, 0x20, 6, 0, 2, 0, 6, 0, 5, 5};
            push(&b.acl, failed, sizeof failed);
            for (int i = 0; i < 1000 && !b.requests[1]; i++) host_poll(h, 0);
            settle(h, &b);
            CHECK(b.p256 == 2 && b.terminals == 2);
            CHECK(b.requests[0] == 1 && b.requests[1] == 1);
        }
        host_close(h);
    } else close_transport(&b);
    CHECK(b.forbidden == 0 && b.closes == 1);
    ph_clock_hook = NULL;
    static const char *suffixes[] = {"", ".new", ".irk", ".irk.new"};
    for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char path[160]; snprintf(path, sizeof path, "%s%s", db, suffixes[i]);
        CHECK(unlink(path) == 0 || errno == ENOENT);
    }
    CHECK(rmdir(dir) == 0);
    printf("LE crypto owner: %u checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
