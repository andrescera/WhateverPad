#include "session.h"
#include <string.h>

#define STRING_(x) #x
#define STRING(x) STRING_(x)
static int mutation(const char *s) { return strcmp(STRING(OWN_MUTATION), s) == 0; }
static int attempt_room(const session_t *s)
{
    return (s->attempt & UINT32_MAX) != UINT32_MAX;
}

void session_init(session_t *s)
{
    *s = (session_t){ .state = SESSION_STARTING, .attempt = 1, .revision = 1 };
}

int session_activate(session_t *s)
{
    if (s->state != SESSION_STARTING || s->reason != SESSION_NONE || s->cleanup_pending)
        return 0;
    s->state = SESSION_ACTIVE;
    s->revision++;
    return 1;
}

int session_retry(session_t *s)
{
    if (s->state != SESSION_FAILED || !s->recoverable || s->reason != SESSION_NONE ||
        s->unsafe || !attempt_room(s) || s->revision == UINT64_MAX ||
        (s->cleanup_pending && !mutation("retry_without_cleanup"))) return 0;
    s->attempt++;
    s->revision++;
    s->state = SESSION_STARTING;
    s->deadline = 0;
    s->cleanup_pending = s->recoverable = 0;
    return 1;
}

static void begin_cleanup(session_t *s, uint64_t now)
{
    if (!s->cleanup_pending)
        s->deadline = now > UINT64_MAX - SESSION_TEARDOWN_MS ?
                      UINT64_MAX : now + SESSION_TEARDOWN_MS;
    s->cleanup_pending = 1;
}

int session_request(session_t *s, enum session_reason reason, uint64_t now)
{
    if (reason <= SESSION_NONE || reason > SESSION_AMBIGUOUS ||
        s->reason != SESSION_NONE) return 0;
    s->reason = reason;
    s->state = SESSION_QUIESCING;
    s->recoverable = 0;
    s->revision++;
    begin_cleanup(s, now);
    return 1;
}

void session_fail(session_t *s, uint64_t now, int recoverable)
{
    if (s->reason != SESSION_NONE) return;
    int was_failed = s->state == SESSION_FAILED;
    s->state = SESSION_FAILED;
    /* A subsequent failure cannot upgrade an unsafe attempt. */
    s->recoverable = !!recoverable && (!was_failed || s->recoverable);
    s->revision++;
    begin_cleanup(s, now);
}

void session_cleanup_complete(session_t *s, enum cleanup_result result)
{
    if (!s->cleanup_pending) return;
    s->cleanup_pending = 0;
    s->unsafe |= result != CLEANUP_CONFIRMED;
    if (s->unsafe) s->recoverable = 0;
    s->revision++;
}

void session_cleanup_done(session_t *s)
{
    session_cleanup_complete(s, CLEANUP_CONFIRMED);
}

int session_accepts(const session_t *s)
{
    return s->state == SESSION_ACTIVE && s->reason == SESSION_NONE && !s->cleanup_pending;
}

int session_remaining(const session_t *s, uint64_t now, int budget)
{
    if (budget <= 0) return 0;
    if (s->cleanup_pending) {
        if (now >= s->deadline) return 0;
        uint64_t left = s->deadline - now;
        if (left < (uint64_t)budget) return (int)left;
    }
    return budget;
}

int session_work_remaining(const session_t *s, uint64_t now, int budget)
{
    if (!s->cleanup_pending) return budget > 0 ? budget : 0;
    uint64_t cutoff = s->deadline >= SESSION_RESERVE_MS ?
                      s->deadline - SESSION_RESERVE_MS : 0;
    if (now >= cutoff || budget <= 0) return 0;
    uint64_t left = cutoff - now;
    return left < (uint64_t)budget ? (int)left : budget;
}

const char *session_state_name(const session_t *s)
{
    static const char *const names[] = { "starting", "active", "quiescing", "failed" };
    return names[s->state];
}

int session_response_status(const session_t *s, enum session_request_kind request,
                            uint64_t now, int lock_available)
{
    if (mutation("failed_http_deadline") && s->state == SESSION_FAILED &&
        s->deadline && now >= s->deadline) return 0;
    if (request == SESSION_INFO || request == SESSION_RELEASE) return 200;
    if (request == SESSION_MUTATE) return session_accepts(s) ? 200 : 503;
    if (request != SESSION_RETRY) return 400;
    if (s->reason != SESSION_NONE || s->state != SESSION_FAILED) return 409;
    if (s->cleanup_pending) return 503;
    return s->recoverable && !s->unsafe && lock_available &&
           attempt_room(s) && s->revision != UINT64_MAX ? 200 : 409;
}

int session_queue_retry(const session_t *s, uint64_t now, uint64_t expires,
                        int lock_available, session_retry_ticket *out)
{
    if (now >= expires ||
        session_response_status(s, SESSION_RETRY, now, lock_available) != 200) return 0;
    *out = (session_retry_ticket){ s->attempt, s->revision, expires };
    return 1;
}

int session_apply_retry(session_t *s, const session_retry_ticket *ticket,
                        uint64_t now, int lock_available)
{
    if ((!mutation("expired_retry") && now >= ticket->expires) ||
        ticket->attempt != s->attempt || ticket->revision != s->revision ||
        session_response_status(s, SESSION_RETRY, now, lock_available) != 200) return 0;
    return session_retry(s);
}
