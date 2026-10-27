#include "attrib.h"
#include "probe.h"
#include "log.h"
#include "util.h"

#include <limits.h>
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
    long deadline, submitted_at;
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

enum { RECENT = 4 };
static const char *const evidence_sources[] = {
    "event", "event_ring", "stream_gap", "deadline", "dispatch", "counter", "unobserved"
};
#define EVIDENCE_UNOBSERVED ((unsigned)(sizeof evidence_sources / sizeof *evidence_sources) - 1)
typedef struct {
    unsigned code, subevent, length, opcode, status, status_known;
} event_metadata;
typedef struct {
    unsigned source, npending, nrecent;
    event_metadata event;
    const char *session;
    long timestamp, since_ready;
    int inquiry, scan;
    struct { unsigned opcode; long age; } pending[PENDING];
    struct { unsigned code, length; long age; } recent[RECENT];
} ambiguity_evidence;
static ambiguity_evidence candidate, frozen;
static struct {
    long ready_at;
    int inquiry, scan, captured, valid, frozen_valid;
    unsigned head, count;
    own_ledger *ledger;
    uint64_t attempt;
    struct { unsigned code, length; long time; } recent[RECENT];
} evidence;

void attrib_reset(void)
{
    memset(&counts, 0, sizeof counts);
    memset(pending, 0, sizeof pending);
    memset(opcodes, 0, sizeof opcodes);
    memset(samples, 0, sizeof samples);
    memset(requests, 0, sizeof requests);
    serial = nsamples = nrequests = 0;
    samples_omitted = 0;
    memset(&candidate, 0, sizeof candidate);
    memset(&frozen, 0, sizeof frozen);
    memset(&evidence, 0, sizeof evidence);
}

void attrib_begin_try(void)
{
    memset(pending, 0, sizeof pending);
    counts.unanswered = 0;
    for (unsigned op = 0; op < 65536; op++) opcodes[op].unanswered = opcodes[op].untracked_pending = 0;
    int retained = evidence.frozen_valid;
    memset(&evidence, 0, sizeof evidence);
    evidence.frozen_valid = retained;
    memset(&candidate, 0, sizeof candidate);
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
    pending[i].submitted_at = now;
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

void attrib_resend(unsigned op)
{
    counts.submitted++;
    opcodes[op].submitted++;
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

static long evidence_age(long now, long then)
{
    if (now <= then) return 0;
    unsigned long age = (unsigned long)now - (unsigned long)then;
    return age > LONG_MAX ? LONG_MAX : (long)age;
}

static event_metadata event_meta(const unsigned char *ev, int len)
{
    event_metadata m = {0};
    if (!ev || len <= 0) return m;
    m.code = ev[0];
    m.length = (unsigned)len;
    if (len >= 3 && ev[0] == 0x3e && ev[1]) m.subevent = ev[2];
    if (len < 2 || len != 2 + ev[1]) return m;
    if (ev[0] == OWN_CC && len >= 5 && ev[1] >= 3) {
        m.opcode = le16(ev + 3);
        /* Only documented status-returning commands used by this host.
         * Vendor completions and unknown return layouts remain opaque. */
        if (len >= 6) switch (m.opcode) {
        case 0x040b: case 0x040c: case 0x040d: case 0x042b: case 0x042c:
        case 0x080e:
        case 0x0c01: case 0x0c14: case 0x0c15: case 0x0c17: case 0x0c19:
        case 0x0c1a: case 0x0c1b: case 0x0c1c: case 0x0c1d: case 0x0c23:
        case 0x0c39: case 0x0c42: case 0x0c44: case 0x0c46: case 0x0c48:
        case 0x0c51: case 0x0c55: case 0x0c58: case 0x0c6c:
        case 0x1001: case 0x1002: case 0x1003: case 0x1004: case 0x1005:
        case 0x1009: case 0x2001: case 0x2002: case 0x200b: case 0x200c:
        case 0x2018:
            m.status = ev[5];
            m.status_known = 1;
            break;
        }
    } else if (ev[0] == OWN_CS && len == 6) {
        m.opcode = le16(ev + 4);
        m.status = ev[2];
        m.status_known = 1;
    } else {
        /* Exact sizes keep arbitrary payload bytes out of the status field. */
        int status_at = 0;
        switch (ev[0]) {
        case 0x01: if (len == 3) status_at = 2; break;  /* Inquiry Complete */
        case 0x03: if (len == 13) status_at = 2; break; /* Connection Complete */
        case 0x05: if (len == 6) status_at = 2; break;  /* Disconnection Complete */
        case 0x06: if (len == 5) status_at = 2; break;  /* Authentication Complete */
        case 0x07: if (len == 257) status_at = 2; break; /* Remote Name Complete */
        case 0x08: if (len == 6) status_at = 2; break;  /* Encryption Change */
        case 0x12: if (len == 10) status_at = 2; break; /* Role Change */
        case 0x14: if (len == 8) status_at = 2; break;  /* Mode Change */
        case 0x30: if (len == 5) status_at = 2; break;  /* Encryption Key Refresh */
        case 0x36: if (len == 9) status_at = 2; break;  /* Simple Pairing Complete */
        case 0x3e:
            if ((m.subevent == 0x01 && len == 21) ||
                (m.subevent == 0x0a && len == 33) ||
                (m.subevent == 0x03 && len == 12) ||
                (m.subevent == 0x04 && len == 14) ||
                (m.subevent == 0x08 && len == 68) ||
                (m.subevent == 0x09 && len == 36)) status_at = 3;
            break;
        }
        if (status_at) {
            m.status = ev[status_at];
            m.status_known = 1;
        }
    }
    return m;
}

void attrib_host_context(long ready_at, int inquiry, int scan)
{
    evidence.ready_at = ready_at;
    evidence.inquiry = !!inquiry;
    evidence.scan = !!scan;
}

void attrib_event_seen(const unsigned char *ev, int len, long now)
{
    if (!ev || len < 2 || len != 2 + ev[1]) return;
    unsigned slot = (evidence.head + evidence.count) % RECENT;
    evidence.recent[slot].code = ev[0];
    evidence.recent[slot].length = (unsigned)len;
    evidence.recent[slot].time = now;
    if (evidence.count < RECENT) evidence.count++;
    else evidence.head = (evidence.head + 1) % RECENT;
}

static void snapshot(unsigned src, const unsigned char *ev, int len, long now, const char *session)
{
    memset(&candidate, 0, sizeof candidate);
    candidate.source = src;
    candidate.event = event_meta(ev, len);
    candidate.session = session;
    candidate.timestamp = now;
    candidate.since_ready = evidence.ready_at > 0 ? evidence_age(now, evidence.ready_at) : -1;
    candidate.inquiry = evidence.inquiry;
    candidate.scan = evidence.scan;
    for (unsigned i = 0; i < PENDING; i++) {
        if (!pending[i].used) continue; /* Expired but unanswered entries still matter. */
        unsigned row = candidate.npending++;
        candidate.pending[row].opcode = pending[i].opcode;
        candidate.pending[row].age = evidence_age(now, pending[i].submitted_at);
    }
    candidate.nrecent = evidence.count;
    for (unsigned i = 0; i < evidence.count; i++) {
        unsigned slot = (evidence.head + i) % RECENT;
        candidate.recent[i].code = evidence.recent[slot].code;
        candidate.recent[i].length = evidence.recent[slot].length;
        candidate.recent[i].age = evidence_age(now, evidence.recent[slot].time);
    }
}

void attrib_watch(own_ledger *l, const char *source, const unsigned char *ev, int len, long now)
{
    if (!l || evidence.captured || !source) return;
    own_summary summary;
    own_snapshot(l, &summary);
    /* The ledger's first ambiguity is the offender; the session reason cannot say so
     * while a Stop, Rest or takeover is already cleaning up. */
    if (summary.ambiguity_count) return;
    unsigned src;
    for (src = 0; src < sizeof evidence_sources / sizeof *evidence_sources; src++)
        if (!strcmp(source, evidence_sources[src])) break;
    if (src == sizeof evidence_sources / sizeof *evidence_sources) return;
    snapshot(src, ev, len, now, session_state_name(&summary.runtime));
    evidence.ledger = l;
    evidence.attempt = summary.runtime.attempt;
    evidence.valid = 1;
}

void attrib_check(own_ledger *l)
{
    if (!l || evidence.captured) return;
    own_summary summary;
    own_snapshot(l, &summary);
    if (!summary.ambiguity_count) {
        evidence.valid = 0;     /* its watch saw no transition: it cannot name a later one */
        return;
    }
    /* A transition at a boundary nobody watched is reported as such, with the live
     * state, rather than blamed on an earlier, innocent step. */
    if (!evidence.valid || evidence.ledger != l || summary.runtime.attempt != evidence.attempt)
        snapshot(EVIDENCE_UNOBSERVED, NULL, 0, now_ms(), session_state_name(&summary.runtime));
    frozen = candidate;
    evidence.captured = evidence.frozen_valid = 1;
#if !WHATEVERPAD_PROBE
    char json[ATTRIB_EVIDENCE_MAX];
    if (attrib_evidence_json(json, sizeof json)) log_line("attrib evidence: %s", json);
#endif
}

size_t attrib_evidence_json(char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    if (cap > ATTRIB_EVIDENCE_MAX) cap = ATTRIB_EVIDENCE_MAX;
#define PUT(...) append(out, cap, &n, __VA_ARGS__)
    if (!evidence.frozen_valid) PUT("null");
    else {
        const ambiguity_evidence *s = &frozen;
        PUT("{\"source\":\"%s\",\"code\":%u,\"subevent\":%u,\"length\":%u,"
            "\"opcode\":%u,\"status\":%u,\"status_known\":%u,\"session\":\"%s\","
            "\"timestamp_ms\":%ld,\"since_ready_ms\":%ld,\"inquiry\":%d,\"scan\":%d,\"pending\":[",
            evidence_sources[s->source], s->event.code, s->event.subevent, s->event.length,
            s->event.opcode, s->event.status, s->event.status_known, s->session,
            s->timestamp, s->since_ready, s->inquiry, s->scan);
        for (unsigned i = 0; i < s->npending; i++)
            PUT("%s{\"opcode\":%u,\"age_ms\":%ld}", i ? "," : "",
                s->pending[i].opcode, s->pending[i].age);
        PUT("],\"recent\":[");
        for (unsigned i = 0; i < s->nrecent; i++)
            PUT("%s{\"code\":%u,\"length\":%u,\"age_ms\":%ld}", i ? "," : "",
                s->recent[i].code, s->recent[i].length, s->recent[i].age);
        PUT("]}");
    }
#undef PUT
    if (n >= cap) { out[0] = 0; return 0; }
    return n;
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
    /* Answers nothing of ours: the host's counter check must not mistake it for ambiguity. */
    if (provenance == OWN_RECEIPT_RETIRED || provenance == OWN_RECEIPT_FOREIGN)
        return ATTRIB_IGNORED;
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
#if WHATEVERPAD_PROBE
    if (op == 0x1005 && probe_deadline_ms && now >= probe_deadline_ms &&
        now <= probe_deadline_ms + PROBE_TAIL_MS) {
        /* Probe-only diagnostics: NONE still reaches own_receive unchanged, so
         * a late observation cannot grant authority or reopen admission. */
        enum attrib_class kind = attrib_observe(e, len, now);
        if (kind == ATTRIB_LATE)
            log_line("probe arrival class=late event=0x%02x mono_ms=%ld", ev, now);
        return kind;
    }
#endif
    attrib_poll(now);
    return record(ATTRIB_UNMATCHED, ev, op, e[ev == 0x0e ? 5 : 2],
                  e[ev == 0x0e ? 2 : 3], now);
}
