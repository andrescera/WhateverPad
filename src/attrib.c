#include "attrib.h"
#include "log.h"
#include "util.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PENDING = 16, SAMPLES = 64 };
static const char *const names[ATTRIB_CLASS_COUNT] = {
    "ignored", "solicited", "nop", "duplicate", "malformed", "late", "unmatched", "untracked"
};
static attrib_counts counts;
static struct {
    unsigned opcode;
    unsigned long serial;
    long deadline;
    int used, expired;
} pending[PENDING];
static unsigned long serial;
/* Fixed storage covers even raw opcodes, without evicting an unanswered command.
 * No command parameters, return parameters, or full addresses are stored here. */
static struct {
    unsigned long classes[ATTRIB_CLASS_COUNT];
    unsigned long submitted, unanswered, timeout, untracked_pending;
    unsigned answered_types;
} opcodes[65536];
static struct {
    long time;
    unsigned event, opcode, status, credits;
    enum attrib_class kind;
} samples[SAMPLES];
static unsigned nsamples;
static unsigned long samples_omitted;
static struct {
    unsigned cod, link_type, tail;
    unsigned long count;
} requests[SAMPLES];
static unsigned nrequests;

void attrib_reset(void)
{
    memset(&counts, 0, sizeof counts);
    memset(pending, 0, sizeof pending);
    memset(opcodes, 0, sizeof opcodes);
    memset(samples, 0, sizeof samples);
    memset(requests, 0, sizeof requests);
    serial = nsamples = nrequests = 0;
    samples_omitted = 0;
}

/* DD 4: procedures acknowledge with Status, queries/settings/replies with Complete. */
static unsigned expected(unsigned op)
{
    switch (op) {
    case 0x0401: case 0x0405: case 0x0406: case 0x0409: case 0x040a:
    case 0x0411: case 0x0413: case 0x200d: case 0x2013: case 0x2019:
    case 0x2025: case 0x2026:
        return 0x0f;
    default:
        return 0x0e;
    }
}

void attrib_submit(unsigned op, int sent, long now)
{
    int i;
    if (!sent || op > 0xffff) return;
    counts.submitted++;
    counts.unanswered++;
    opcodes[op].submitted++;
    opcodes[op].unanswered++;
    for (i = 0; i < PENDING && pending[i].used; i++) {}
    if (i == PENDING) {
        counts.untracked++;
        opcodes[op].untracked_pending++;
        return;
    }
    pending[i].used = 1;
    pending[i].expired = 0;
    pending[i].opcode = op;
    pending[i].deadline = now + 1000;
    pending[i].serial = ++serial;
}

void attrib_deadline(unsigned op, long deadline)
{
    int i, newest = -1;
    for (i = 0; i < PENDING; i++)
        if (pending[i].used && pending[i].opcode == op &&
            (newest < 0 || pending[i].serial > pending[newest].serial)) newest = i;
    if (newest >= 0) pending[newest].deadline = deadline;
}

void attrib_poll(long now)
{
    int i;
    for (i = 0; i < PENDING; i++) {
        if (!pending[i].used || pending[i].expired || now < pending[i].deadline) continue;
        pending[i].expired = 1;
        counts.timeout++;
        opcodes[pending[i].opcode].timeout++;
    }
}

static enum attrib_class record(enum attrib_class kind, unsigned ev, unsigned op,
                                unsigned status, unsigned credits, long now)
{
    counts.classes[kind]++;
    opcodes[op].classes[kind]++;
    if (kind != ATTRIB_SOLICITED) {
        if (nsamples < SAMPLES) {
            samples[nsamples].time = now;
            samples[nsamples].event = ev;
            samples[nsamples].opcode = op;
            samples[nsamples].status = status;
            samples[nsamples].credits = credits;
            samples[nsamples++].kind = kind;
        } else samples_omitted++;
    }
    return kind;
}

enum attrib_class attrib_observe(const unsigned char *e, int len, long now)
{
    unsigned ev, op, status, credits, type;
    enum attrib_class kind;
    int i, oldest = -1;
    attrib_poll(now);
    if (len <= 0) return ATTRIB_IGNORED;
    ev = e[0];
    if (ev == 4) {
        if (len < 12 || e[1] < 10 || len < 2 + e[1])
            return record(ATTRIB_MALFORMED, ev, 0, 0, 0, now);
        counts.requests++;
        for (i = 0; i < (int)nrequests; i++)
            if (requests[i].cod == (unsigned)(le16(e + 8) | e[10] << 16) &&
                requests[i].link_type == e[11]) break;
        if (i == SAMPLES) { counts.requests_untracked++; return ATTRIB_IGNORED; }
        if (i == (int)nrequests) {
            requests[i].cod = le16(e + 8) | (unsigned)e[10] << 16;
            requests[i].link_type = e[11];
            nrequests++;
        }
        requests[i].count++;
        /* Bluetooth addresses print MSB first: retain only the displayed suffix. */
        requests[i].tail = le16(e + 2);
        return ATTRIB_IGNORED;
    }
    if (ev != 0x0e && ev != 0x0f) return ATTRIB_IGNORED;
    /* Credit-only NOP Complete has no status byte (Core HCI 7.7.14). */
    if (len >= 5 && ev == 0x0e && e[1] == 3 && le16(e + 3) == 0)
        return record(ATTRIB_NOP, ev, 0, 0, e[2], now);
    if (len < 6 || e[1] < 4 || len < 2 + e[1])
        return record(ATTRIB_MALFORMED, ev, 0, 0, 0, now);
    op = le16(e + (ev == 0x0e ? 3 : 4));
    status = e[ev == 0x0e ? 5 : 2];
    credits = e[ev == 0x0e ? 2 : 3];
    type = ev == 0x0e ? 1 : 2;
    if (!op) return record(ATTRIB_NOP, ev, op, status, credits, now);
    for (i = 0; i < PENDING; i++)
        if (pending[i].used && pending[i].opcode == op && expected(op) == ev &&
            (oldest < 0 || pending[i].serial < pending[oldest].serial)) oldest = i;
    if (oldest >= 0) {
        kind = pending[oldest].expired ? ATTRIB_LATE : ATTRIB_SOLICITED;
        pending[oldest].used = 0;
        counts.unanswered--;
        opcodes[op].unanswered--;
        opcodes[op].answered_types |= type;
    } else if (opcodes[op].untracked_pending && expected(op) == ev) {
        kind = ATTRIB_UNTRACKED;
        opcodes[op].untracked_pending--;
        opcodes[op].unanswered--;
        counts.unanswered--;
        opcodes[op].answered_types |= type;
    } else if (opcodes[op].answered_types & type) {
#ifdef ATTRIB_MUTATE_DUP
        kind = ATTRIB_SOLICITED;
#else
        kind = ATTRIB_DUPLICATE;
#endif
    } else kind = ATTRIB_UNMATCHED;
    return record(kind, ev, op, status, credits, now);
}

void attrib_overflow(void) { counts.overflow++; }
const attrib_counts *attrib_get(void) { return &counts; }

static void append(char *out, size_t cap, size_t *n, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (*n >= cap) return;
    va_start(ap, fmt);
    k = vsnprintf(out + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= cap - *n) *n = cap;
    else *n += (size_t)k;
}

size_t attrib_json(char *out, size_t cap)
{
    size_t n = 0;
    unsigned i, rows = 0, omitted = 0;
    unsigned long unsolicited = 0;
    if (!cap) return 0;
#define PUT(...) append(out, cap, &n, __VA_ARGS__)
    for (i = ATTRIB_DUPLICATE; i < ATTRIB_CLASS_COUNT; i++) unsolicited += counts.classes[i];
    PUT("{\"rules_version\":%d,\"submitted\":%lu,\"untracked\":%lu,\"overflow\":%lu,"
        "\"unanswered\":%lu,\"timeout\":%lu,\"unsolicited\":%lu,\"requests\":%lu,\"requests_untracked\":%lu",
        ATTRIB_RULES_VERSION, counts.submitted, counts.untracked, counts.overflow,
        counts.unanswered, counts.timeout, unsolicited, counts.requests, counts.requests_untracked);
    for (i = ATTRIB_SOLICITED; i < ATTRIB_CLASS_COUNT; i++)
        PUT(",\"%s%s\":%lu", names[i], i == ATTRIB_UNTRACKED ? "_answers" : "", counts.classes[i]);
    PUT(",\"opcodes\":[");
    for (i = 0; i < 65536; i++) {
        unsigned c;
        int used = opcodes[i].submitted != 0;
        for (c = 1; c < ATTRIB_CLASS_COUNT; c++) used |= opcodes[i].classes[c] != 0;
        if (!used) continue;
        if (rows == SAMPLES) { omitted++; continue; }
        PUT("%s{\"opcode\":%u,\"submitted\":%lu,\"unanswered\":%lu,\"timeout\":%lu",
            rows++ ? "," : "", i, opcodes[i].submitted, opcodes[i].unanswered, opcodes[i].timeout);
        for (c = 1; c < ATTRIB_CLASS_COUNT; c++)
            PUT(",\"%s\":%lu", names[c], opcodes[i].classes[c]);
        PUT("}");
    }
    PUT("],\"opcodes_omitted\":%u,\"samples\":[", omitted);
    for (i = 0; i < nsamples; i++)
        PUT("%s{\"time\":%ld,\"event\":%u,\"opcode\":%u,\"status\":%u,\"credits\":%u,\"class\":\"%s\"}",
            i ? "," : "", samples[i].time, samples[i].event, samples[i].opcode,
            samples[i].status, samples[i].credits, names[samples[i].kind]);
    PUT("],\"samples_omitted\":%lu,\"connection_requests\":[", samples_omitted);
    for (i = 0; i < nrequests; i++)
        PUT("%s{\"cod\":%u,\"link_type\":%u,\"count\":%lu,\"addr_tail\":\"%02X:%02X\"}",
            i ? "," : "", requests[i].cod, requests[i].link_type, requests[i].count,
            requests[i].tail >> 8, requests[i].tail & 255);
    PUT("]}");
#undef PUT
    if (n >= cap) { out[0] = 0; return 0; }
    return n;
}

void attrib_log_summary(void)
{
    log_line("attrib: rules=%d submitted=%lu solicited=%lu nop=%lu duplicate=%lu malformed=%lu late=%lu unmatched=%lu "
             "untracked=%lu untracked_answers=%lu overflow=%lu unanswered=%lu timeout=%lu requests=%lu requests_untracked=%lu",
             ATTRIB_RULES_VERSION, counts.submitted, counts.classes[ATTRIB_SOLICITED], counts.classes[ATTRIB_NOP],
             counts.classes[ATTRIB_DUPLICATE], counts.classes[ATTRIB_MALFORMED], counts.classes[ATTRIB_LATE],
             counts.classes[ATTRIB_UNMATCHED], counts.untracked, counts.classes[ATTRIB_UNTRACKED], counts.overflow,
             counts.unanswered, counts.timeout, counts.requests, counts.requests_untracked);
}

enum attrib_class attrib_observe_receipt(const unsigned char *e, int len, long now,
                                         own_provenance provenance)
{
    unsigned ev, op;
    if (provenance == OWN_RECEIPT_SEQUENCED || len <= 0 ||
        (e[0] != 0x0e && e[0] != 0x0f))
        return attrib_observe(e, len, now);
    ev = e[0];
    if (ev == 0x0e && len == 5 && e[1] == 3 && !le16(e + 3))
        return attrib_observe(e, len, now);
    if (len < 6 || e[1] < 4 || len != 2 + e[1] || (ev == 0x0f && e[1] != 4)) {
        attrib_poll(now);
        return record(ATTRIB_MALFORMED, ev, 0, 0, 0, now);
    }
    op = le16(e + (ev == 0x0e ? 3 : 4));
    if (!op && (ev == 0x0e || e[2] == 0)) return attrib_observe(e, len, now);
    attrib_poll(now);
    return record(ATTRIB_UNMATCHED, ev, op, e[ev == 0x0e ? 5 : 2],
                  e[ev == 0x0e ? 2 : 3], now);
}
