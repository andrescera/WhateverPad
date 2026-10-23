/* GPL-3.0-or-later. Single-threaded policy; the outer owner performs I/O. */
#ifndef WHATEVERPAD_SESSION_H
#define WHATEVERPAD_SESSION_H

#include <stdint.h>

enum session_state { SESSION_STARTING, SESSION_ACTIVE, SESSION_QUIESCING, SESSION_FAILED };
enum session_reason { SESSION_NONE, SESSION_STOP, SESSION_REST, SESSION_NATIVE, SESSION_AMBIGUOUS };
enum cleanup_result { CLEANUP_CONFIRMED, CLEANUP_UNCERTAIN, CLEANUP_FAILED };
enum session_request_kind { SESSION_INFO, SESSION_RELEASE, SESSION_RETRY, SESSION_MUTATE };
#define SESSION_TEARDOWN_MS 10000
#define SESSION_RESERVE_MS 250

typedef struct {
    enum session_state state;
    enum session_reason reason;
    uint64_t attempt, revision, deadline;
    int recoverable, cleanup_pending, unsafe;
} session_t;

typedef struct {
    uint64_t attempt, revision, expires;
} session_retry_ticket;

void session_init(session_t *);
int session_activate(session_t *);
int session_retry(session_t *);
int session_request(session_t *, enum session_reason, uint64_t now);
void session_fail(session_t *, uint64_t now, int recoverable);
void session_cleanup_complete(session_t *, enum cleanup_result);
void session_cleanup_done(session_t *); /* Confirmed completion, as in T. */
int session_accepts(const session_t *);
int session_remaining(const session_t *, uint64_t now, int budget);
int session_work_remaining(const session_t *, uint64_t now, int budget);
const char *session_state_name(const session_t *);
/* Status selection only. Framing/writing and lock acquisition belong to web/owner. */
int session_response_status(const session_t *, enum session_request_kind,
                            uint64_t now, int lock_available);
int session_queue_retry(const session_t *, uint64_t now, uint64_t expires,
                        int lock_available, session_retry_ticket *);
int session_apply_retry(session_t *, const session_retry_ticket *,
                        uint64_t now, int lock_available);

#endif
