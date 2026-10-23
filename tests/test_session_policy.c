/* Policy effects, not T's private lifecycle fields. No time passes implicitly. */
#include "../src/session.h"
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { \
    printf("FAIL session_policy:%d: %s\n", __LINE__, #x); failed++; } } while (0)

int main(void)
{
    session_t s;
    session_retry_ticket ticket = {0};
    session_init(&s);
    session_fail(&s, 100, 1);
    CHECK(!session_retry(&s)); /* A5: this is deliberately the first assertion. */
    CHECK(session_response_status(&s, SESSION_RETRY, 101, 1) == 503);
    CHECK(!session_queue_retry(&s, 101, 200, 1, &ticket));
    CHECK(session_remaining(&s, 10100, 50) == 0);
    CHECK(session_work_remaining(&s, 9850, 50) == 0);
    session_fail(&s, 9000, 1);
    CHECK(session_remaining(&s, 10100, 50) == 0);
    session_cleanup_done(&s);
    CHECK(session_response_status(&s, SESSION_INFO, 20000, 1) == 200);
    CHECK(session_response_status(&s, SESSION_RETRY, 20000, 1) == 200);
    CHECK(session_response_status(&s, SESSION_RETRY, 20000, 0) == 409);
    CHECK(session_remaining(&s, 20000, 50) == 50);
    CHECK(session_queue_retry(&s, 20000, 20010, 1, &ticket));
    CHECK(!session_apply_retry(&s, &ticket, 20010, 1));
    CHECK(session_queue_retry(&s, 20000, 20010, 1, &ticket));
    CHECK(session_apply_retry(&s, &ticket, 20009, 1));
    CHECK(!session_apply_retry(&s, &ticket, 20009, 1));
    CHECK(strcmp(session_state_name(&s), "starting") == 0);
    CHECK(!session_accepts(&s));
    CHECK(session_activate(&s));
    CHECK(session_accepts(&s));
    CHECK(session_response_status(&s, SESSION_RETRY, 20009, 1) == 409);
    CHECK(session_request(&s, SESSION_NATIVE, 21000));
    CHECK(!session_request(&s, SESSION_STOP, 22000));
    session_fail(&s, 23000, 1);
    CHECK(session_remaining(&s, 30999, 1000) == 1);
    CHECK(session_work_remaining(&s, 30750, 1000) == 0);
    CHECK(!session_activate(&s));
    CHECK(!session_retry(&s));
    CHECK(!session_accepts(&s));
    CHECK(session_response_status(&s, SESSION_RETRY, 31000, 1) == 409);
    session_cleanup_done(&s);
    CHECK(!session_retry(&s));

    for (int state = 0; state < 5; state++) {
        session_init(&s);
        if (state == 1) session_activate(&s);
        if (state == 2) session_request(&s, SESSION_REST, 5);
        if (state >= 3) session_fail(&s, 5, 1);
        if (state == 4) session_cleanup_done(&s);
        for (uint64_t now = 10; now <= 20010; now += 20000) {
            CHECK(session_response_status(&s, SESSION_INFO, now, 1) == 200);
            CHECK(session_response_status(&s, SESSION_RELEASE, now, 1) == 200);
            CHECK(session_response_status(&s, SESSION_MUTATE, now, 1) ==
                  (state == 1 ? 200 : 503));
            CHECK(session_response_status(&s, SESSION_RETRY, now, 1) ==
                  (state == 3 ? 503 : state == 4 ? 200 : 409));
        }
    }
    for (int outcome = CLEANUP_UNCERTAIN; outcome <= CLEANUP_FAILED; outcome++) {
        session_init(&s);
        session_fail(&s, 0, 1);
        session_cleanup_complete(&s, (enum cleanup_result)outcome);
        CHECK(!session_retry(&s));
        CHECK(session_response_status(&s, SESSION_RETRY, 20000, 1) == 409);
    }
    session_init(&s);
    session_fail(&s, 100, 1);
    CHECK(session_request(&s, SESSION_STOP, 9000));
    CHECK(session_remaining(&s, 10100, 100) == 0);
    session_init(&s);
    session_fail(&s, 1, 1);
    session_cleanup_done(&s);
    CHECK(session_queue_retry(&s, 2, 10, 1, &ticket));
    CHECK(session_request(&s, SESSION_REST, 3));
    CHECK(!session_apply_retry(&s, &ticket, 4, 1));
    session_init(&s);
    session_fail(&s, 0, 0);
    session_fail(&s, 1, 1);
    session_cleanup_done(&s);
    CHECK(!session_retry(&s));
    session_init(&s);
    session_fail(&s, 0, 1);
    session_cleanup_done(&s);
    s.attempt = UINT32_MAX; /* Public attempt identity: exhausted local namespace. */
    CHECK(session_response_status(&s, SESSION_RETRY, 20000, 1) == 409);
    CHECK(!session_retry(&s));
    session_init(&s);
    session_request(&s, SESSION_STOP, UINT64_MAX - 10);
    CHECK(session_remaining(&s, UINT64_MAX, 100) == 0);
    printf("session_policy checks=%d violations=%d\n", checks, failed);
    return failed != 0;
}
