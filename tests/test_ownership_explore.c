/* Breadth-first exploration of production transitions. Oracle producer labels
 * stay here: the backend receives only own_ack's wire fields. A8 B1 constrains
 * real answers; injected old/foreign copies are not constrained by that queue.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/ownership.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEPTH 12
#define KEY_MAX 8192
#ifndef OWN_EXPLORE_WORKERS
#define OWN_EXPLORE_WORKERS 4
#endif
enum scenario { SCAN, MASK, QUERY, CLASSIC, LE_CREATE, INQUIRY, AUTH, ENCRYPT, P256, DHKEY, VIRTUAL, SCENARIOS };
static const char *const scenario_names[] = {
    "scan", "mask", "query", "classic", "le-create", "inquiry",
    "auth", "encryption", "p256", "dhkey", "virtual"
};
enum action {
    PREPARE_OLD, PREPARE_NEW, SEND, NOT_SENT, UNCERTAIN, HOLD, COMMIT_HELD,
    LOCAL_SUCCESS, LOCAL_REFUSAL, OLD_ACK, FOREIGN_ACK, OLD_RAND, RAND,
    TERMINAL_OK, TERMINAL_FAIL, OLD_TERMINAL, DISCONNECT, DETACH,
    INPUT, OLD_INPUT, LOCAL_OK, LOCAL_FAIL, LOCAL_STOP, IDENTITY, UNPROVEN_ID,
    STOP, REST, NATIVE, FAILURE, CLOSE, RETRY, TIMEOUT,
    CUTOFF_MINUS_ONE, CUTOFF, DEADLINE, OVERFLOW, MALFORMED, WRONG_EVENT,
    FOREIGN_WRITE, READBACK, REUSED_HANDLE, OLD_DISCONNECT, READBACK_FOREIGN,
    OLD_REFUSAL, DISCONNECT_FAILURE, ACTIONS
};
static const char *const action_names[] = {
    "prepare-old", "prepare-new", "send", "not-sent", "uncertain", "hold-token", "commit-held",
    "real-success", "real-refusal", "old-ack", "foreign-ack", "old-rand", "fresh-rand",
    "terminal", "terminal-failure", "old-terminal", "disconnect", "detach",
    "input", "old-input", "local-effect", "local-failure", "release-inside-local",
    "own-identity", "unproven-identity", "Stop", "rest", "native", "failure", "close",
    "Retry", "timeout", "cutoff-1", "cutoff", "deadline+1", "overflow", "malformed",
    "wrong-event", "foreign-write", "equal-readback", "reused-handle", "old-disconnect",
    "foreign-value-readback", "old-refusal", "disconnect-failure"
};

/* Fixed-width oracle state, serialized separately from the production key.
 * The two command identities, generations and producers are never collapsed.
 */
enum {
    NOW, REF_A, REF_S = REF_A + 4, LINK_A = REF_S + 4,
    LINK_G = LINK_A + 2, LINK_SLOT = LINK_G + 2,
    ACCEPTED = LINK_SLOT + 2, CONFIRMED = ACCEPTED + 4,
    TERMINALS = CONFIRMED + 4, SENDS = TERMINALS + 4, HANDLES = SENDS + 4,
    ORIGINAL_VALUE = HANDLES + 4, UNDOS = ORIGINAL_VALUE + 4,
    UNDO_TERMINALS = UNDOS + 4,
    CONNECTS = UNDO_TERMINALS + 4, RETIRES = CONNECTS + 2, DELETES = RETIRES + 2,
    ADDS = DELETES + 2, IDENTIFIED = ADDS + 2, BOUND = IDENTIFIED + 2,
    INPUT_SEEN = BOUND + 2, LAST_INPUT = INPUT_SEEN + 2, LIVE = LAST_INPUT + 2,
    PENDING_I = LIVE + 2, PENDING_PHASE, PENDING_OP, PENDING_EVENT,
    Q2_I, Q2_PHASE, Q2_OP, Q2_EVENT, Q3_I, Q3_PHASE, Q3_OP, Q3_EVENT,
    Q4_I, Q4_PHASE, Q4_OP, Q4_EVENT,
    BOOK_WAIT, BOOK_OP, BOOK_EVENT,
    BARRIER_I, OLD_OP, OLD_EVENT, OLD_RANDOM, RAND_COUNTER, FOREIGN_USED,
    OLD_REFUSAL_OP, OLD_REFUSAL_EVENT,
    TIMEOUT_SEEN, G_REACHED, H_STARTED, H_REACHED, NOISE, HELD, POLICY_META,
    SETTING_VALUE, SETTING_OWNER, FINALIZES, G_BLOCKED_NEW, O_COUNT
};
typedef struct {
    own_ledger *l;
    uint64_t o[O_COUNT];
    own_token held;
} world;

typedef struct {
    uint64_t hash;
    uint8_t *key;
    uint16_t len;
    uint8_t depth, path[DEPTH];
} node;
typedef struct {
    node *nodes;
    uint32_t *slots;
    size_t count, capacity, slot_capacity;
    int saw_g, saw_h;
} search;

static own_ref ref_at(const world *w, unsigned i)
{
    return (own_ref){w->o[REF_A+i], w->o[REF_S+i]};
}
static link_ref link_at(const world *w, unsigned i)
{
    return (link_ref){w->o[LINK_A+i], w->o[LINK_G+i], (uint8_t)w->o[LINK_SLOT+i]};
}
static own_peer peer(unsigned i, int le)
{
    own_peer p = {0};
    p.adapter[0] = 1;
    p.le = (uint8_t)le;
    p.identity[0] = p.wire[0] = (uint8_t)(i + 1);
    return p;
}
static int ref_index(const world *w, own_ref r)
{
    for (unsigned i = 0; i < 4; i++)
        if (r.serial && w->o[REF_A+i] == r.attempt && w->o[REF_S+i] == r.serial)
            return (int)i;
    return -1;
}
static int link_index(const world *w, link_ref r)
{
    for (unsigned i = 0; i < 2; i++)
        if (r.generation && w->o[LINK_A+i] == r.attempt &&
            w->o[LINK_G+i] == r.generation && w->o[LINK_SLOT+i] == r.slot)
            return (int)i;
    return -1;
}
static int is_create(enum scenario s) { return s == CLASSIC || s == LE_CREATE; }
static int is_setting(enum scenario s) { return s == SCAN || s == MASK; }

static own_command command(const world *w, enum scenario s, unsigned i)
{
    static const unsigned op[] = {0x200c,0x0c01,0x1009,0x0405,0x200d,0x0401,
                                  0x0411,0x2019,0x2025,0x2026,0x1009};
    static const unsigned plen[] = {2,8,0,13,25,5,2,28,0,64,0};
    own_command c = {0};
    c.opcode = (uint16_t)op[s];
    c.plen = (uint8_t)plen[s];
    c.params[0] = (uint8_t)i; /* Two setting values; foreign value is 7. */
    c.ack_by = w->o[NOW] + 100;
    c.terminal_by = w->o[NOW] + 200;
    c.original_known = is_setting(s);
    c.original_len = c.plen;
    if (is_setting(s)) c.original[0] = (uint8_t)w->o[SETTING_VALUE];
    if (is_create(s)) {
        c.peer = peer(i, s == LE_CREATE);
        c.link = link_at(w, i);
        memcpy(c.params + (s == LE_CREATE ? 6 : 0), c.peer.wire, 6);
        if (s == LE_CREATE) c.params[5] = c.peer.address_type;
    } else if (s == AUTH || s == ENCRYPT || s == DHKEY) {
        c.link = link_at(w, 0);
        c.peer = peer(0, 0);
        c.params[0] = 40;
    }
    return c;
}

static own_provenance wire(world *w, unsigned op, unsigned event, unsigned status,
                           unsigned random_len, uint64_t random, int malformed, int overflow)
{
    own_ack a = { .event = (own_ack_event)event, .opcode = (uint16_t)op,
                  .status = (uint8_t)status, .random_len = (uint8_t)random_len,
                  .malformed = (uint8_t)malformed, .overflow = (uint8_t)overflow };
    for (unsigned i = 0; i < 8; i++) a.random[i] = (uint8_t)(random >> (8*i));
    own_receipt r;
    own_provenance provenance = own_backend_receipt(w->l, &a, w->o[NOW], &r);
    if (provenance == OWN_RECEIPT_SEQUENCED) w->o[BOOK_WAIT] = 0;
    own_receive(w->l, &r, w->o[NOW]);
    return provenance;
}

static const char *effect(world *w, enum scenario s, const own_effect *e,
                          int sent, int local_success, int release_inside)
{
    own_summary snapshot;
    own_snapshot(w->l, &snapshot);
    if (snapshot.finalized) return "I12: effect after finalization";
    int i = ref_index(w, e->owner);
    int g = link_index(w, e->link);
    int active = snapshot.runtime.reason == SESSION_NONE &&
                 !snapshot.runtime.cleanup_pending &&
                 (snapshot.runtime.state == SESSION_ACTIVE || snapshot.runtime.state == SESSION_STARTING);
    if (e->kind == OWN_EFFECT_HCI) {
        if (e->opcode == 0x0402 || e->opcode == 0x0408 ||
            e->opcode == 0x200e || e->opcode == 0x0c03) return "foreign cancel/Reset";
        if (e->phase != OWN_UNDO && !active) return "I3: normal HCI after latch";
        if (i < 0) return "unknown effect invocation";
        if (e->phase == OWN_FORWARD &&
            (e->opcode == 0x0411 || e->opcode == 0x0413 || e->opcode == 0x2013 ||
             e->opcode == 0x2019 || e->opcode == 0x2026) &&
            (g < 0 || !w->o[LIVE+g])) return "I10: stale prepared link target";
        if (e->phase == OWN_UNDO) {
            if (w->o[POLICY_META] == OWN_POLICY_NO_RESTORE) return "NO_RESTORE emitted undo";
            if (snapshot.ambiguity_count ||
                !w->o[ACCEPTED+i] || !w->o[CONFIRMED+i] ||
                snapshot.transport_closed) return "I2: undo without actual acquisition/barrier";
            if (is_setting(s) && (e->opcode != command(w, s, 0).opcode ||
                                 e->params[0] != w->o[ORIGINAL_VALUE+i]))
                return "I15: undo is not original bytes";
            if (is_setting(s) && (w->o[SETTING_OWNER] != (uint64_t)i+1 ||
                                 w->o[SETTING_VALUE] != (uint64_t)i))
                return "I2: restore would clobber a foreign/replacement value";
            if (e->opcode == 0x0406 &&
                (w->o[TERMINALS+i] != 1 || g < 0 || !w->o[INPUT_SEEN+g] ||
                 w->o[NOW] < w->o[LAST_INPUT+g] ||
                 w->o[NOW] - w->o[LAST_INPUT+g] > OWN_FRESH_MS ||
                 e->handle != w->o[HANDLES+i])) return "I10: unowned/stale handle";
            if (snapshot.runtime.cleanup_pending &&
                w->o[NOW] >= snapshot.runtime.deadline - SESSION_RESERVE_MS)
                return "I11: undo after work cutoff";
        }
        if (!sent) return NULL;
        if (e->phase == OWN_UNDO && ++w->o[UNDOS+i] > 1)
            return "I2: repeated undo for one acquisition";
        if (w->o[BOOK_WAIT]) return "A3: second backend-unacknowledged command";
        w->o[BOOK_WAIT] = 1;
        w->o[BOOK_OP] = e->opcode;
        w->o[BOOK_EVENT] = e->phase == OWN_BARRIER ? OWN_CC :
            e->opcode == 0x0406 || is_create(s) || s == INQUIRY || s == AUTH ||
            s == ENCRYPT || s == P256 || s == DHKEY ? OWN_CS : OWN_CC;
        if (e->phase == OWN_BARRIER) {
            if (w->o[BARRIER_I]) return "two outstanding barriers";
            w->o[BARRIER_I] = (uint64_t)i + 1;
        } else {
            unsigned slot = !w->o[PENDING_I] ? PENDING_I : !w->o[Q2_I] ? Q2_I :
                            !w->o[Q3_I] ? Q3_I : Q4_I;
            if (w->o[slot]) return "oracle real-answer queue exhausted";
            w->o[slot] = (uint64_t)i + 1;
            w->o[slot+1] = e->phase;
            w->o[slot+2] = e->opcode;
            w->o[slot+3] = w->o[BOOK_EVENT];
            if (e->phase == OWN_FORWARD && ++w->o[SENDS+i] > 1)
                return "A3: forward command retransmitted";
        }
        return NULL;
    }
    if (e->kind == OWN_EFFECT_FINALIZE) {
        if (w->o[FINALIZES] == snapshot.runtime.attempt) return "I12: repeated local finalization";
        w->o[FINALIZES] = snapshot.runtime.attempt;
        return NULL;
    }
    if (g < 0) return "stale generation side effect";
    if ((e->kind == OWN_EFFECT_CONNECT || e->kind == OWN_EFFECT_ADD ||
         e->kind == OWN_EFFECT_BIND || e->kind == OWN_EFFECT_INPUT) && !active)
        return "I3: normal local effect after latch";
    switch (e->kind) {
    case OWN_EFFECT_CONNECT:
        if (++w->o[CONNECTS+g] > 1) return "I8: repeated connect";
        break;
    case OWN_EFFECT_RETIRE:
        if (++w->o[RETIRES+g] > w->o[CONNECTS+g]) return "I8: repeated retirement";
        break;
    case OWN_EFFECT_ADD: if (local_success) w->o[ADDS+g] = 1; break;
    case OWN_EFFECT_BIND:
        if (!w->o[IDENTIFIED+g]) return "Bind without own identity";
        if (local_success) w->o[BOUND+g] = 1;
        break;
    case OWN_EFFECT_INPUT:
        if (!w->o[BOUND+g]) return "input before virtual readiness";
        break;
    case OWN_EFFECT_DELETE:
        if (!w->o[ADDS+g] || !w->o[IDENTIFIED+g] || ++w->o[DELETES+g] > 1)
            return "I9: repeated/unowned deletion";
        break;
    default: break;
    }
    if (release_inside) own_release(w->l, SESSION_STOP, w->o[NOW]);
    return NULL;
}

/* Returns 1 for an enabled transition, 0 for a disabled one. */
static int apply(world *w, enum scenario s, enum action action, const char **error)
{
    own_summary snap;
    own_snapshot(w->l, &snap);
    uint64_t now = w->o[NOW];
    int i = 0;
    own_token token;
    own_effect e;
    own_command c;
    *error = NULL;
    if (action == PREPARE_OLD || action == PREPARE_NEW) {
        i = action == PREPARE_NEW;
        if (s == VIRTUAL || w->o[REF_S+i] || (i && !w->o[REF_S])) return 0;
        if (is_create(s)) {
            own_peer p = peer((unsigned)i, s == LE_CREATE);
            link_ref link;
            if (own_reserve_link(w->l, &p, &link) != OWN_OK) return 0;
            w->o[LINK_A+i] = link.attempt;
            w->o[LINK_G+i] = link.generation;
            w->o[LINK_SLOT+i] = link.slot;
        }
        c = command(w, s, (unsigned)i);
        own_ref ref;
        own_result result = own_prepare(w->l, &c, &ref);
        if (result != OWN_OK) {
            if (i == 1 && is_setting(s) && result == OWN_STOPPED &&
                w->o[TIMEOUT_SEEN] && !w->o[G_BLOCKED_NEW]) {
                w->o[G_BLOCKED_NEW] = 1;
                return 1;
            }
            return 0;
        }
        w->o[REF_A+i] = ref.attempt; w->o[REF_S+i] = ref.serial;
        return 1;
    }
    if (action == HOLD) {
        if (w->o[HELD] || own_next_hci(w->l, now, &w->held) != OWN_OK) return 0;
        w->o[HELD] = 1;
        return 1;
    }
    if (action == SEND || action == NOT_SENT || action == UNCERTAIN || action == COMMIT_HELD) {
        if (action == COMMIT_HELD) {
            if (!w->o[HELD]) return 0;
            token = w->held;
            w->o[HELD] = 0;
        } else if (own_next_hci(w->l, now, &token) != OWN_OK) return 0;
        if (own_commit(w->l, &token, now, &e) != OWN_OK) return action == COMMIT_HELD;
        int sent = action != NOT_SENT && action != UNCERTAIN;
        *error = effect(w, s, &e, sent, 1, 0);
        if (snap.runtime.reason != SESSION_NONE && e.phase != OWN_UNDO)
            *error = "I3: stale token upgraded by cleanup deadline";
        if (e.phase == OWN_UNDO && snap.runtime.reason == SESSION_NONE &&
            snap.runtime.state != SESSION_FAILED && action != COMMIT_HELD)
            *error = "unsolicited undo";
        own_submitted(w->l, &token, action == NOT_SENT ? OWN_NOT_SENT :
                      action == UNCERTAIN ? OWN_SEND_UNCERTAIN : OWN_SENT, now);
        return 1;
    }
    if (action == LOCAL_SUCCESS || action == LOCAL_REFUSAL) {
        if (!w->o[PENDING_I]) return 0;
        i = (int)w->o[PENDING_I] - 1;
        unsigned status = action == LOCAL_REFUSAL ? 0x0c : 0;
        if (!status && w->o[PENDING_PHASE] == OWN_FORWARD) {
            w->o[ACCEPTED+i] = 1;
            if (is_setting(s)) {
                uint64_t previous_owner = w->o[SETTING_OWNER];
                w->o[ORIGINAL_VALUE+i] =
                    previous_owner >= 1 && previous_owner <= 2 &&
                    w->o[REF_A+previous_owner-1] == w->o[REF_A+i] &&
                    w->o[ACCEPTED+previous_owner-1] && w->o[CONFIRMED+previous_owner-1] ?
                    w->o[ORIGINAL_VALUE+previous_owner-1] : w->o[SETTING_VALUE];
                w->o[SETTING_OWNER] = (uint64_t)i + 1;
                w->o[SETTING_VALUE] = (uint64_t)i;
            }
            w->o[OLD_OP] = w->o[PENDING_OP];
            w->o[OLD_EVENT] = w->o[PENDING_EVENT];
        }
        if (!status && w->o[PENDING_PHASE] == OWN_UNDO && is_setting(s)) {
            w->o[SETTING_VALUE] = w->o[ORIGINAL_VALUE+i];
            w->o[SETTING_OWNER] = 0;
        }
        if (w->o[TIMEOUT_SEEN] && w->o[G_BLOCKED_NEW] && i == 0 && is_setting(s))
            w->o[G_REACHED] = 1;
        if (status && w->o[H_STARTED] && w->o[BARRIER_I]) w->o[H_REACHED] = 1;
        wire(w, (unsigned)w->o[PENDING_OP], (unsigned)w->o[PENDING_EVENT],
             status, 0, 0, 0, 0);
        for (unsigned k = 0; k < 4; k++) {
            w->o[PENDING_I+k] = w->o[Q2_I+k];
            w->o[Q2_I+k] = w->o[Q3_I+k];
            w->o[Q3_I+k] = w->o[Q4_I+k];
            w->o[Q4_I+k] = 0;
        }
        return 1;
    }
    if (action == RAND || action == OLD_RAND) {
        if (!w->o[BARRIER_I] || (action == RAND && w->o[PENDING_I]) ||
            (action == OLD_RAND && !w->o[OLD_RANDOM])) return 0;
        i = (int)w->o[BARRIER_I] - 1;
        uint64_t random = action == RAND ? ++w->o[RAND_COUNTER] : w->o[OLD_RANDOM];
        if (action == RAND) {
            w->o[CONFIRMED+i] = w->o[ACCEPTED+i] && snap.runtime.reason == SESSION_NONE &&
                                  !snap.runtime.cleanup_pending;
            w->o[OLD_RANDOM] = random;
            w->o[BARRIER_I] = 0;
        }
        wire(w, 0x2018, OWN_CC, 0, 8, random, 0, 0);
        return 1;
    }
    if (action == OLD_ACK || action == FOREIGN_ACK || action == OLD_REFUSAL) {
        if (!w->o[PENDING_I] || w->o[PENDING_PHASE] != OWN_FORWARD ||
            snap.runtime.reason != SESSION_NONE || w->o[FOREIGN_USED]) return 0;
        if (action == OLD_ACK && (!w->o[OLD_OP] || w->o[OLD_OP] != w->o[PENDING_OP]))
            return 0;
        if (action == OLD_REFUSAL &&
            (!w->o[OLD_REFUSAL_OP] || w->o[OLD_REFUSAL_OP] != w->o[PENDING_OP]))
            return 0;
        w->o[FOREIGN_USED] = 1;
        if (action == OLD_ACK && is_setting(s)) w->o[H_STARTED] = 1;
        if (action == FOREIGN_ACK && is_setting(s)) {
            w->o[SETTING_VALUE] = 7;
            w->o[SETTING_OWNER] = 7;
        }
        unsigned packet_op = (unsigned)(action == OLD_ACK ? w->o[OLD_OP] :
            action == OLD_REFUSAL ? w->o[OLD_REFUSAL_OP] : w->o[PENDING_OP]);
        unsigned packet_event = (unsigned)(action == OLD_ACK ? w->o[OLD_EVENT] :
            action == OLD_REFUSAL ? w->o[OLD_REFUSAL_EVENT] : w->o[PENDING_EVENT]);
        wire(w, packet_op, packet_event,
             action == OLD_REFUSAL ? 0x0c : 0, 0, 0, 0, 0);
        return 1;
    }
    if (action == DISCONNECT_FAILURE) {
        for (i = 0; i < 4; i++)
            if (w->o[UNDOS+i] && w->o[HANDLES+i] && !w->o[UNDO_TERMINALS+i]) break;
        if (i == 4) return 0;
        w->o[UNDO_TERMINALS+i] = 1;
        own_terminal_event refusal = { .kind = OWN_TERM_DISCONNECT,
            .handle = (uint16_t)w->o[HANDLES+i], .status = 0x0c };
        own_terminal(w->l, ref_at(w, (unsigned)i), &refusal, now);
        return 1;
    }
    if (action == TERMINAL_OK || action == TERMINAL_FAIL ||
        action == OLD_TERMINAL || action == REUSED_HANDLE) {
        i = action == OLD_TERMINAL ? 0 : w->o[REF_S+1] ? 1 : 0;
        if (action == REUSED_HANDLE && (!is_create(s) || i != 1)) return 0;
        if (!w->o[REF_S+i] || w->o[TERMINALS+i] || !w->o[SENDS+i] ||
            is_setting(s) || s == QUERY || s == VIRTUAL) return 0;
        static const own_terminal_kind kinds[] = {
            OWN_TERM_COMMAND, OWN_TERM_COMMAND, OWN_TERM_COMMAND,
            OWN_TERM_CONNECTION, OWN_TERM_CONNECTION, OWN_TERM_INQUIRY,
            OWN_TERM_AUTH, OWN_TERM_ENCRYPTION, OWN_TERM_P256, OWN_TERM_DHKEY,
            OWN_TERM_COMMAND
        };
        own_terminal_event term = { .kind = kinds[s],
            .peer = peer((unsigned)i, s == LE_CREATE), .handle = (uint16_t)(40+i),
            .status = action == TERMINAL_FAIL ? 4 : 0 };
        if (action == REUSED_HANDLE) term.handle = 40;
        w->o[HANDLES+i] = term.handle;
        w->o[TERMINALS+i] = action == TERMINAL_FAIL ? 2 : 1;
        if (is_create(s) && action != TERMINAL_FAIL) w->o[LIVE+i] = 1;
        own_terminal(w->l, ref_at(w, (unsigned)i), &term, now);
        return 1;
    }
    if (action == INPUT || action == OLD_INPUT || action == DISCONNECT ||
        action == OLD_DISCONNECT || action == DETACH) {
        if (!is_create(s) && s != VIRTUAL && s != AUTH && s != ENCRYPT && s != DHKEY)
            return 0;
        i = action == OLD_INPUT || action == OLD_DISCONNECT ? 0 : w->o[LINK_G+1] ? 1 : 0;
        if (!w->o[LINK_G+i]) return 0;
        link_ref link = link_at(w, (unsigned)i);
        unsigned owner = is_create(s) ? (unsigned)i : (unsigned)i + 2;
        uint16_t handle = w->o[HANDLES+owner] ? (uint16_t)w->o[HANDLES+owner] :
                                              (uint16_t)(40+i);
        if (action == INPUT || action == OLD_INPUT) {
            if (own_input(w->l, link, handle, now) != OWN_OK) return 0;
            if (!w->o[ACCEPTED+owner] || !w->o[CONFIRMED+owner] ||
                w->o[TERMINALS+owner] != 1) *error = "I5: usable input from unowned link";
            unsigned other = owner == 0 ? 1 : owner == 1 ? 0 : owner == 2 ? 3 : 2;
            if (w->o[CONFIRMED+other] && w->o[TERMINALS+other] == 1 &&
                w->o[HANDLES+other] == handle) *error = "I10: reused handle became usable";
            w->o[INPUT_SEEN+i] = 1; w->o[LAST_INPUT+i] = now;
            return 1;
        }
        if (action == DETACH) return own_detach_link(w->l, link) == OWN_OK;
        w->o[LIVE+i] = 0;
        own_disconnected(w->l, link, handle, now);
        return 1;
    }
    if (action == LOCAL_OK || action == LOCAL_FAIL || action == LOCAL_STOP) {
        if (own_next_local(w->l, now, &token) != OWN_OK ||
            own_commit(w->l, &token, now, &e) != OWN_OK) return 0;
        *error = effect(w, s, &e, 1, action != LOCAL_FAIL, action == LOCAL_STOP);
        own_backend_local_result(w->l, &token, action != LOCAL_FAIL);
        if (e.kind == OWN_EFFECT_FINALIZE)
            own_finalized(w->l, action == LOCAL_FAIL ? CLEANUP_FAILED : CLEANUP_CONFIRMED);
        return 1;
    }
    if (action == IDENTITY || action == UNPROVEN_ID) {
        for (i = 0; i < 2; i++) if (w->o[ADDS+i] && !w->o[IDENTIFIED+i]) break;
        if (i == 2) return 0;
        own_backend_identity(w->l, link_at(w, (unsigned)i), 100 + (unsigned)i,
                             action == IDENTITY);
        if (action == IDENTITY) w->o[IDENTIFIED+i] = 1;
        else if (w->o[NOISE] & 1) return 0;
        else w->o[NOISE] |= 1;
        return 1;
    }
    if (action == STOP || action == REST || action == NATIVE)
        return own_release(w->l, action == STOP ? SESSION_STOP :
                           action == REST ? SESSION_REST : SESSION_NATIVE, now);
    if (action == FAILURE) {
        if (snap.runtime.state == SESSION_FAILED || snap.runtime.reason != SESSION_NONE) return 0;
        own_fail(w->l, now, 1);
        return 1;
    }
    if (action == CLOSE) {
        if (snap.transport_closed || !snap.runtime.cleanup_pending) return 0;
        own_transport_closed(w->l, now);
        w->o[BOOK_WAIT] = 0;
        return 1;
    }
    if (action == RETRY) return own_retry(w->l);
    if (action == TIMEOUT) {
        if ((!w->o[PENDING_I] && !w->o[HELD] && !w->o[REF_S]) ||
            w->o[TIMEOUT_SEEN] || now >= 1000) return 0;
        w->o[NOW] = 1000;
        w->o[TIMEOUT_SEEN] = 1;
        own_tick(w->l, 1000);
        return 1;
    }
    if (action == CUTOFF_MINUS_ONE || action == CUTOFF || action == DEADLINE) {
        if (!snap.runtime.cleanup_pending) return 0;
        uint64_t target = action == CUTOFF_MINUS_ONE ? snap.runtime.deadline - 251 :
                          action == CUTOFF ? snap.runtime.deadline - 250 :
                          snap.runtime.deadline + 1;
        if (now >= target) return 0;
        w->o[NOW] = target;
        own_tick(w->l, target);
        return 1;
    }
    if (action == FOREIGN_WRITE) {
        if (!is_setting(s) || !w->o[REF_S] || snap.runtime.reason != SESSION_NONE) return 0;
        own_setting_changed(w->l, command(w, s, 0).opcode, now);
        w->o[SETTING_VALUE] = w->o[SETTING_OWNER] = 7;
        w->o[ACCEPTED] = w->o[ACCEPTED+1] = 0; /* actual foreign value 7 */
        return 1;
    }
    if (action == READBACK || action == READBACK_FOREIGN) {
        unsigned bit = action == READBACK ? 2 : 8;
        if (!is_setting(s) || !w->o[REF_S] || w->o[NOISE] & bit) return 0;
        c = command(w, s, w->o[REF_S+1] ? 1 : 0);
        if (action == READBACK_FOREIGN)
            w->o[SETTING_VALUE] = w->o[SETTING_OWNER] = 7;
        c.params[0] = (uint8_t)w->o[SETTING_VALUE];
        own_readback(w->l, c.opcode, c.params, c.plen);
        w->o[NOISE] |= bit;
        return 1;
    }
    if (action == OVERFLOW || action == MALFORMED || action == WRONG_EVENT) {
        if (snap.runtime.reason != SESSION_NONE || w->o[NOISE] & 4) return 0;
        w->o[NOISE] |= 4;
        wire(w, (unsigned)(w->o[PENDING_OP] ? w->o[PENDING_OP] : 0x1009),
             w->o[PENDING_EVENT] == OWN_CC ? OWN_CS : OWN_CC, 0, 0, 0,
             action == MALFORMED, action == OVERFLOW);
        return 1;
    }
    return 0;
}

static world initial(own_policy policy, enum scenario s)
{
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "invalid explorer root: %s\n", #x); exit(2); } } while (0)
    world w = {0};
    w.o[POLICY_META] = policy;
    w.l = own_new(policy, 1);
    if (!w.l) exit(2);
    REQUIRE(own_activate(w.l));
    if (is_setting(s)) {
        /* A real, settled refusal from before this two-record window supplies
         * the old negative packet; it is not an invented causal tag. */
        own_command c = command(&w, s, 0);
        own_ref historical;
        own_token t;
        own_effect e;
        REQUIRE(own_prepare(w.l, &c, &historical) == OWN_OK);
        REQUIRE(own_next_hci(w.l, 0, &t) == OWN_OK);
        REQUIRE(own_commit(w.l, &t, 0, &e) == OWN_OK);
        own_submitted(w.l, &t, OWN_SENT, 0);
        REQUIRE(wire(&w, c.opcode, OWN_CC, 0x0c, 0, 0, 0, 0) == OWN_RECEIPT_SEQUENCED);
        REQUIRE(own_reclaim(w.l, historical) == OWN_OK);
        w.o[OLD_REFUSAL_OP] = c.opcode;
        w.o[OLD_REFUSAL_EVENT] = OWN_CC;
    }
    unsigned seeds = s == VIRTUAL ? 2 : s == AUTH || s == ENCRYPT || s == DHKEY ? 1 : 0;
    for (unsigned g = 0; g < seeds; g++) {
        own_peer p = peer(g, 0);
        link_ref link;
        own_ref ref;
        REQUIRE(own_reserve_link(w.l, &p, &link) == OWN_OK);
        w.o[LINK_A+g] = link.attempt; w.o[LINK_G+g] = link.generation; w.o[LINK_SLOT+g] = link.slot;
        own_command c = command(&w, CLASSIC, g);
        REQUIRE(own_prepare(w.l, &c, &ref) == OWN_OK);
        w.o[REF_A+2+g] = ref.attempt; w.o[REF_S+2+g] = ref.serial;
        own_token t; own_effect e;
        REQUIRE(own_next_hci(w.l, 0, &t) == OWN_OK);
        REQUIRE(own_commit(w.l, &t, 0, &e) == OWN_OK && e.opcode == 0x0405);
        own_submitted(w.l, &t, OWN_SENT, 0);
        REQUIRE(wire(&w, 0x0405, OWN_CS, 0, 0, 0, 0, 0) == OWN_RECEIPT_SEQUENCED);
        REQUIRE(own_next_hci(w.l, 0, &t) == OWN_OK);
        REQUIRE(own_commit(w.l, &t, 0, &e) == OWN_OK && e.opcode == 0x2018);
        own_submitted(w.l, &t, OWN_SENT, 0);
        REQUIRE(wire(&w, 0x2018, OWN_CC, 0, 8, ++w.o[RAND_COUNTER], 0, 0) == OWN_RECEIPT_SEQUENCED);
        own_terminal_event term = { .kind = OWN_TERM_CONNECTION, .peer = p,
                                   .handle = (uint16_t)(40+g) };
        own_terminal(w.l, ref, &term, 0);
        own_ledger *probe = own_clone(w.l);
        REQUIRE(probe != NULL);
        REQUIRE(own_input(probe, link, term.handle, 0) == OWN_OK);
        own_free(probe);
        w.o[ACCEPTED+2+g] = w.o[CONFIRMED+2+g] = w.o[TERMINALS+2+g] = 1;
        w.o[HANDLES+2+g] = 40+g;
        w.o[LIVE+g] = 1;
        w.o[OLD_RANDOM] = w.o[RAND_COUNTER];
    }
    REQUIRE(own_invariant(w.l) == NULL);
#undef REQUIRE
    return w;
}

static size_t key_of(const world *w, uint8_t out[KEY_MAX])
{
    size_t n = own_canonical(w->l, out, KEY_MAX);
    if (n + 10*O_COUNT + sizeof w->held > KEY_MAX) {
        fputs("canonical key exceeds bound\n", stderr); exit(2);
    }
    for (unsigned i = 0; i < O_COUNT; i++) {
        uint64_t v = w->o[i];
        do { out[n++] = (uint8_t)(v & 127) | (v > 127 ? 128 : 0); v >>= 7; } while (v);
    }
    if (w->o[HELD]) { memcpy(out+n, &w->held, sizeof w->held); n += sizeof w->held; }
    return n;
}
static uint64_t hash_key(const uint8_t *key, size_t n)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < n; i++) { hash ^= key[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static void rehash(search *q)
{
    free(q->slots);
    q->slot_capacity = q->slot_capacity ? q->slot_capacity * 2 : 8192;
    q->slots = calloc(q->slot_capacity, sizeof *q->slots);
    if (!q->slots) exit(2);
    for (size_t i = 0; i < q->count; i++) {
        size_t pos = q->nodes[i].hash & (q->slot_capacity - 1);
        while (q->slots[pos]) pos = (pos + 1) & (q->slot_capacity - 1);
        q->slots[pos] = (uint32_t)i + 1;
    }
}
static void insert(search *q, const world *w, const node *parent, unsigned action)
{
    uint8_t key[KEY_MAX];
    size_t n = key_of(w, key);
    uint64_t hash = hash_key(key, n);
    if (!q->slot_capacity || q->count * 2 >= q->slot_capacity) rehash(q);
    size_t pos = hash & (q->slot_capacity - 1);
    while (q->slots[pos]) {
        const node *old = &q->nodes[q->slots[pos] - 1];
        if (old->hash == hash && old->len == n && !memcmp(old->key, key, n)) return;
        pos = (pos + 1) & (q->slot_capacity - 1);
    }
    node next = { .hash = hash, .len = (uint16_t)n };
    if (parent) {
        next.depth = parent->depth + 1;
        memcpy(next.path, parent->path, parent->depth);
        next.path[parent->depth] = (uint8_t)action;
    }
    next.key = malloc(n);
    if (!next.key) exit(2);
    memcpy(next.key, key, n);
    if (q->count == q->capacity) {
        q->capacity = q->capacity ? q->capacity * 2 : 4096;
        node *nodes = realloc(q->nodes, q->capacity * sizeof *nodes);
        if (!nodes) exit(2);
        q->nodes = nodes;
    }
    q->nodes[q->count] = next;
    q->slots[pos] = (uint32_t)++q->count;
    q->saw_g |= !!w->o[G_REACHED]; q->saw_h |= !!w->o[H_REACHED];
}

static void trace(enum scenario s, own_policy policy, const node *parent,
                  unsigned action, const char *error)
{
    printf("FAIL explorer policy=%d class=%s: %s\nshortest failing trace (%u events):",
           policy, scenario_names[s], error, parent->depth + 1);
    for (unsigned i = 0; i < parent->depth; i++) printf(" %s", action_names[parent->path[i]]);
    printf(" %s\n", action_names[action]);
}

static size_t explore(own_policy policy, enum scenario s, int *saw_g, int *saw_h)
{
    search q = {0};
    world root = initial(policy, s);
    insert(&q, &root, NULL, 0);
    uint64_t edges = 0;
    for (size_t head = 0; head < q.count; head++) {
        node current = q.nodes[head];
        if (current.depth == DEPTH) continue;
        world base = root;
        base.l = own_clone(root.l);
        if (!base.l) exit(2);
        const char *error;
        for (unsigned k = 0; k < current.depth; k++) {
            if (!apply(&base, s, (enum action)current.path[k], &error) || error) exit(2);
        }
        uint8_t replay_key[KEY_MAX];
        size_t replay_len = key_of(&base, replay_key);
        if (replay_len != current.len || memcmp(replay_key, current.key, replay_len)) {
            fputs("nondeterministic replay\n", stderr); exit(2);
        }
        for (unsigned a = 0; a < ACTIONS; a++) {
            world next = base;
            next.l = own_clone(base.l);
            if (!next.l) exit(2);
            if (apply(&next, s, (enum action)a, &error)) {
                edges++;
                if (!error) error = own_invariant(next.l);
                if (error) { trace(s, policy, &current, a, error); exit(1); }
                insert(&q, &next, &current, a);
            }
            own_free(next.l);
        }
        own_free(base.l);
    }
    printf("class=%s policy=%d states=%zu edges=%llu\n", scenario_names[s], policy,
           q.count, (unsigned long long)edges);
    *saw_g |= q.saw_g; *saw_h |= q.saw_h;
    size_t count = q.count;
    for (size_t i = 0; i < q.count; i++) free(q.nodes[i].key);
    free(q.nodes); free(q.slots); own_free(root.l);
    return count;
}

typedef struct { size_t states; int g, h; } case_result;

static int policy_cases(own_policy policy, size_t *states, int *g, int *h)
{
#ifdef OWN_MUTATION
    /* Fault controls keep deterministic first-failure ordering. */
    for (enum scenario s = SCAN; s < SCENARIOS; s++) *states += explore(policy, s, g, h);
    return 0;
#else
    enum { WORKERS = OWN_EXPLORE_WORKERS };
    _Static_assert(WORKERS > 0 && WORKERS <= SCENARIOS, "bounded graph workers");
    pid_t pid[WORKERS] = {0};
    int fd[WORKERS] = {0};
    unsigned next = 0, active = 0;
    int result = 0;
    while ((next < SCENARIOS || active) && !result) {
        for (unsigned slot = 0; slot < WORKERS && next < SCENARIOS; slot++) {
            if (pid[slot]) continue;
            int channel[2];
            if (pipe(channel) != 0) { result = 2; break; }
            fflush(NULL);
            pid_t child = fork();
            if (child < 0) {
                close(channel[0]); close(channel[1]);
                result = 2; break;
            }
            if (!child) {
                close(channel[0]);
                case_result r = {0};
                r.states = explore(policy, (enum scenario)next, &r.g, &r.h);
                ssize_t n;
                do { n = write(channel[1], &r, sizeof r); } while (n < 0 && errno == EINTR);
                close(channel[1]);
                exit(n == (ssize_t)sizeof r ? 0 : 2);
            }
            close(channel[1]);
            pid[slot] = child;
            fd[slot] = channel[0];
            next++; active++;
        }
        if (result) break;
        int status;
        pid_t done;
        do { done = waitpid(-1, &status, 0); } while (done < 0 && errno == EINTR);
        if (done < 0) { result = 2; break; }
        unsigned slot = 0;
        while (slot < WORKERS && pid[slot] != done) slot++;
        if (slot == WORKERS) { result = 2; break; }
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            result = WIFEXITED(status) ? WEXITSTATUS(status) : 2;
        } else {
            case_result r;
            size_t got = 0;
            while (got < sizeof r) {
                ssize_t n = read(fd[slot], (char *)&r + got, sizeof r - got);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                got += (size_t)n;
            }
            if (got != sizeof r) result = 2;
            else { *states += r.states; *g |= r.g; *h |= r.h; }
        }
        close(fd[slot]);
        pid[slot] = 0;
        active--;
    }
    /* A failing worker cannot leave other graph processes behind. */
    for (unsigned slot = 0; slot < WORKERS; slot++)
        if (pid[slot]) kill(pid[slot], SIGTERM);
    for (unsigned slot = 0; slot < WORKERS; slot++) {
        if (!pid[slot]) continue;
        while (waitpid(pid[slot], NULL, 0) < 0 && errno == EINTR) {}
        close(fd[slot]);
    }
    return result;
#endif
}

int main(void)
{
    for (own_policy policy = OWN_POLICY_NO_RESTORE; policy <= OWN_POLICY_SINGLE_PRODUCER; policy++) {
        size_t count = 0;
        int g = 0, h = 0;
        int result = policy_cases(policy, &count, &g, &h);
        if (result) return result;
        if (!g || !h) { printf("FAIL explorer unreachable G=%d H=%d\n", g, h); return 1; }
        printf("policy=%s depth=12 states=%zu violations=0 G=reachable H=reachable\n",
               policy == OWN_POLICY_NO_RESTORE ? "NO_RESTORE" : "SINGLE_PRODUCER", count);
    }
    return 0;
}
