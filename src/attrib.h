/* Observations only: an opcode match never grants ownership or undo authority. */
#ifndef WP_ATTRIB_H
#define WP_ATTRIB_H

#include <stddef.h>

#ifndef WHATEVERPAD_PROBE
#define WHATEVERPAD_PROBE 0
#endif

#define ATTRIB_RULES_VERSION 1
#define ATTRIB_JSON_MAX 65536

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
void attrib_submit(unsigned opcode, int sent, long now);
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

#endif
