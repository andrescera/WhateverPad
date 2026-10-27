/* Observations only: an opcode match never grants ownership or undo authority. */
#ifndef WP_ATTRIB_H
#define WP_ATTRIB_H

#include <stddef.h>
#include "ownership.h"

#ifndef WHATEVERPAD_PROBE
#define WHATEVERPAD_PROBE 0
#endif

#define ATTRIB_RULES_VERSION 1
#define ATTRIB_JSON_MAX 65536
#define ATTRIB_EVIDENCE_MAX 2048

enum attrib_class {
    ATTRIB_IGNORED, ATTRIB_SOLICITED, ATTRIB_NOP, ATTRIB_DUPLICATE,
    ATTRIB_MALFORMED, ATTRIB_LATE, ATTRIB_UNMATCHED, ATTRIB_UNTRACKED,
    ATTRIB_CLASS_COUNT
};

typedef struct {
    unsigned long classes[ATTRIB_CLASS_COUNT];
    unsigned long submitted, untracked, overflow, unanswered, timeout;
    unsigned long requests, requests_untracked;
} attrib_counts;

/* Process-lifetime counters survive host retries. Reset only at a new run/test. */
void attrib_reset(void);
void attrib_begin_try(void); /* Retire pending entries, retain cumulative observations. */
void attrib_submit(unsigned opcode, int sent, long now);
void attrib_resend(unsigned opcode); /* Another wire send, the same logical waiter. */
/* Override the default 1,000 ms acknowledgement deadline for a synchronous wait. */
void attrib_deadline(unsigned opcode, long deadline);
void attrib_poll(long now);
enum attrib_class attrib_observe(const unsigned char *event, int len, long now);
void attrib_overflow(void);
const attrib_counts *attrib_get(void);
/* Returns 0 and an empty string on insufficient capacity, never partial JSON.
 * All counters are retained; JSON lists the first 64 active opcode rows and
 * reports omitted rows. Non-solicited samples retain the first 64 arrivals. */
size_t attrib_json(char *out, size_t capacity);
void attrib_log_summary(void);

/* Observations only; call watch before a possible detector transition and check
 * immediately after it. Sources are event, event_ring, stream_gap, deadline,
 * dispatch and counter. Only the first ambiguity per try freezes/logs evidence.
 * begin_try clears live context/history but retains the previous frozen report. */
void attrib_host_context(long ready_at, int inquiry, int scan);
void attrib_event_seen(const unsigned char *, int len, long now);
void attrib_watch(own_ledger *, const char *source, const unsigned char *, int len, long now);
void attrib_check(own_ledger *);
/* Compact JSON or null; includes its terminator within ATTRIB_EVIDENCE_MAX.
 * Insufficient capacity returns 0 and an empty string, never partial JSON.
 * since_ready_ms is -1 until a positive ready_at has been supplied. */
size_t attrib_evidence_json(char *, size_t capacity);

/* NONE is unmatched authority: count the observation without answering a
 * pending local command. NOP/malformed observations keep their wire classes. */
enum attrib_class attrib_observe_receipt(const unsigned char *, int len, long now,
                                         own_provenance);

#endif
