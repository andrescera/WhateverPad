#include "ownership.h"
#include <stdlib.h>
#include <string.h>

#define STRING_(x) #x
#define STRING(x) STRING_(x)
static int mutation(const char *s) { return strcmp(STRING(OWN_MUTATION), s) == 0; }

enum command_class { QUERY, SETTING, CREATE, PROCEDURE, REPLY, CRYPTO, DISCONNECT, BARRIER_ONLY };
typedef struct {
    uint16_t opcode;
    uint8_t class, ack, terminal, plen;
} descriptor;

/* Default deny, including Reset, every Cancel, and irreversible pairing mode.
 * LE Rand is barrier-only, so all local Rand answers enter the freshness table.
 */
static const descriptor commands[] = {
    {0x0401, PROCEDURE, OWN_CS, OWN_TERM_INQUIRY, 5},
    {0x0405, CREATE, OWN_CS, OWN_TERM_CONNECTION, 13},
    {0x0406, DISCONNECT, OWN_CS, OWN_TERM_DISCONNECT, 3},
    {0x0409, CREATE, OWN_CS, OWN_TERM_CONNECTION, 7},
    {0x040a, REPLY, OWN_CS, OWN_TERM_CONNECTION, 7},
    {0x040b, REPLY, OWN_CC, OWN_TERM_COMMAND, 22},
    {0x040c, REPLY, OWN_CC, OWN_TERM_COMMAND, 6},
    {0x040d, REPLY, OWN_CC, OWN_TERM_COMMAND, 23},
    {0x0411, PROCEDURE, OWN_CS, OWN_TERM_AUTH, 2},
    {0x0413, PROCEDURE, OWN_CS, OWN_TERM_ENCRYPTION, 3},
    {0x042b, REPLY, OWN_CC, OWN_TERM_COMMAND, 9},
    {0x042c, REPLY, OWN_CC, OWN_TERM_COMMAND, 6},
    {0x0c01, SETTING, OWN_CC, OWN_TERM_COMMAND, 8},
    {0x0c1a, SETTING, OWN_CC, OWN_TERM_COMMAND, 1},
    {0x0c1c, SETTING, OWN_CC, OWN_TERM_COMMAND, 4},
    {0x2001, SETTING, OWN_CC, OWN_TERM_COMMAND, 8},
    {0x200b, SETTING, OWN_CC, OWN_TERM_COMMAND, 7},
    {0x200c, SETTING, OWN_CC, OWN_TERM_COMMAND, 2},
    {0x2002, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x200d, CREATE, OWN_CS, OWN_TERM_CONNECTION, 25},
    {0x2013, PROCEDURE, OWN_CS, OWN_TERM_UPDATE, 14},
    {0x2018, BARRIER_ONLY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x2019, PROCEDURE, OWN_CS, OWN_TERM_ENCRYPTION, 28},
    {0x2025, CRYPTO, OWN_CS, OWN_TERM_P256, 0},
    {0x2026, CRYPTO, OWN_CS, OWN_TERM_DHKEY, 64},
    {0x080e, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c14, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c15, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c17, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c19, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c1b, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c1d, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c23, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c39, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c42, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c44, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c46, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c48, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c51, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c55, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c58, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x0c6c, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x1001, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x1002, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x1003, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x1004, QUERY, OWN_CC, OWN_TERM_COMMAND, 1},
    {0x1005, QUERY, OWN_CC, OWN_TERM_COMMAND, 0},
    {0x1009, QUERY, OWN_CC, OWN_TERM_COMMAND, 0}
};
_Static_assert(sizeof commands / sizeof *commands <= 64, "setup history width");

enum link_state { LINK_FREE, RESERVED, CONNECTING, UP, READY, RETIRING, RETIRED, LINK_QUARANTINED };
enum virtual_state { V_FREE, V_QUEUED, V_ADDING, V_PENDING_ID, V_IDENTIFIED, V_BINDING, V_READY, V_FAILED, V_DELETED };

typedef struct {
    own_ref ref, previous;
    uint64_t revision, forward_receipt;
    own_state state;
    own_result result;
    own_command c;
    unsigned desc;
    uint8_t sent, acked, terminal_seen, terminal_failed, lease, barrier_done;
    uint8_t barrier_pending, undo_consumed, undo_ack, undo_terminal;
    uint8_t submit_pending, submitted_phase;
    uint8_t forward_status, undo_status, undo_terminal_status;
    uint8_t proof_veto, binding_revoked;
    uint64_t phase_ack_by, phase_terminal_by;
    uint64_t setup_by;
    uint8_t setup_sends, retry_pending;
    own_terminal_event terminal;
    own_token committed;
} own_record;

typedef struct {
    link_ref ref;
    own_peer peer;
    own_ref owner;
    uint64_t revision, device_id, last_input;
    uint16_t handle;
    uint8_t state, vstate, has_handle, usable, input_pending;
    uint8_t connected, retired, delete_claimed, delete_ok, add_ok;
    uint8_t local_pending;
    own_token committed;
} own_link;

typedef struct {
    uint64_t sequence, attempt, serial, phase, provenance, event, opcode, status;
    uint64_t random, random_len, credit;
} receipt_data;
_Static_assert(sizeof(receipt_data) <= sizeof(own_receipt), "receipt storage");

struct own_ledger {
    session_t session;
    own_policy policy;
    own_record records[OWN_MAX];
    own_link links[OWN_LINK_MAX];
    uint64_t serial, generation, epoch, receipt_issued, receipt_seen, cleanup_deadline, now;
    uint64_t barrier_values[OWN_BARRIER_MAX];
    unsigned barrier_count;
    uint8_t handles_seen[512];
    own_ref waiter;
    own_phase waiting_phase;
    own_receipt issued_receipt;
    unsigned ambiguities, credits;
    unsigned radio_left;
    uint8_t rand_supported, transport_closed, finalize_claimed, finalized, dirty_local;
    uint64_t setup_retired;
    uint8_t rand_resent;
    uint16_t failed_opcode;
    uint8_t reply_timeout;
};

static int same_ref(own_ref a, own_ref b)
{
    return a.attempt == b.attempt && a.serial == b.serial;
}

static void wipe(void *storage, size_t n)
{
    volatile uint8_t *p = storage;
    while (n--) *p++ = 0;
}

static int same_link(link_ref a, link_ref b)
{
    return a.attempt == b.attempt &&
           (mutation("drop_link_generation") || a.generation == b.generation) &&
           a.slot == b.slot;
}

static int same_peer(const own_peer *a, const own_peer *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static own_record *record(own_ledger *l, own_ref ref)
{
    if (!ref.serial) return NULL;
    for (unsigned i = 0; i < OWN_MAX; i++)
        if (l->records[i].state != OWN_FREE && same_ref(l->records[i].ref, ref))
            return &l->records[i];
    return NULL;
}

static const own_record *const_record(const own_ledger *l, own_ref ref)
{
    return record((own_ledger *)l, ref);
}

static own_link *link_at(own_ledger *l, link_ref ref)
{
    if (!ref.generation || ref.slot >= OWN_LINK_MAX) return NULL;
    own_link *p = &l->links[ref.slot];
    return p->state != LINK_FREE && same_link(p->ref, ref) ? p : NULL;
}

static int admits(const own_ledger *l)
{
    return !l->transport_closed && !l->session.cleanup_pending &&
           l->session.reason == SESSION_NONE &&
           (l->session.state == SESSION_STARTING || l->session.state == SESSION_ACTIVE);
}

static int before_cutoff(const own_ledger *l, uint64_t now)
{
    return !l->transport_closed && session_work_remaining(&l->session, now, 1) > 0;
}

static uint64_t bounded_add(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

own_ledger *own_new(own_policy policy, int le_rand_supported)
{
    /* Initialization reserves a process-unique attempt namespace. Transitions
     * and explorer clones use only their ledger state, never this allocator. */
    static uint64_t instance;
    if (instance == UINT32_MAX) return NULL;
    own_ledger *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    session_init(&l->session);
    l->session.attempt = (++instance << 32) | 1;
    l->policy = policy == OWN_POLICY_SINGLE_PRODUCER ? policy : OWN_POLICY_NO_RESTORE;
    l->rand_supported = !!le_rand_supported;
    l->epoch = 1;
    return l;
}

void own_free(own_ledger *l)
{
    if (!l) return;
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        if (r->c.opcode == 0x040b || r->c.opcode == 0x040d || r->c.opcode == 0x2019)
            wipe(r->c.params, sizeof r->c.params);
    }
    free(l);
}

own_ledger *own_clone(const own_ledger *l)
{
    own_ledger *out = malloc(sizeof *out);
    if (out) memcpy(out, l, sizeof *out);
    return out;
}

/* Model-only value copy; no native pointers/resources exist in the ledger.
 * Kept out of the production header and source-gated to tests. */
void own_model_copy(own_ledger *out, const own_ledger *in)
{
    memcpy(out, in, sizeof *out);
}

int own_activate(own_ledger *l)
{
    if (l->rand_resent && !l->waiter.serial) { own_detector_trip(l, l->now); return 0; }
    if (!admits(l)) return 0;
    for (unsigned i = 0; i < OWN_MAX; i++)
        if (l->records[i].state == OWN_PREPARED || l->records[i].state == OWN_SUBMITTED ||
            l->records[i].state == OWN_QUARANTINED || l->records[i].barrier_pending)
            return 0;
    return session_activate(&l->session);
}

static void freeze(own_ledger *l)
{
    l->epoch++;
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        r->retry_pending = 0;
        if (r->state == OWN_PREPARED && !mutation("deadline_is_teardown")) {
            r->state = OWN_CLOSED;
            r->result = OWN_STOPPED;
            r->revision++;
        }
        /* Stop before the barrier answer means no lease, even if it is in flight. */
        if (r->barrier_pending) r->lease = 0;
    }
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        own_link *p = &l->links[i];
        if (p->state == LINK_FREE) continue;
        p->state = RETIRING;
        p->input_pending = 0;
        if (p->vstate == V_QUEUED) p->vstate = V_FREE;
        p->revision++;
    }
}

int own_release(own_ledger *l, enum session_reason reason, uint64_t now)
{
    if (now > l->now) l->now = now;
    int was_pending = l->session.cleanup_pending;
    if (!session_request(&l->session, reason, now)) return 0;
    if (!was_pending) l->cleanup_deadline = l->session.deadline;
    freeze(l);
    return 1;
}

void own_fail(own_ledger *l, uint64_t now, int recoverable)
{
    if (now > l->now) l->now = now;
    if (l->session.reason != SESSION_NONE) return;
    int was_pending = l->session.cleanup_pending;
    session_fail(&l->session, now, recoverable);
    if (!was_pending) {
        l->cleanup_deadline = l->session.deadline;
        freeze(l);
    }
}

void own_detector_trip(own_ledger *l, uint64_t now)
{
    if (!l->ambiguities) {
        own_record *r = record(l, l->waiter);
        if (r) l->failed_opcode = l->waiting_phase == OWN_BARRIER ? 0x2018 :
            l->waiting_phase == OWN_UNDO && commands[r->desc].class == CREATE ?
            0x0406 : r->c.opcode;
    }
    l->ambiguities++;
    l->session.unsafe = 1;
    own_release(l, SESSION_AMBIGUOUS, now);
    l->waiter = (own_ref){0};
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        r->lease = 0;
        r->barrier_pending = 0;
        if (r->state != OWN_FREE && r->state != OWN_CLOSED) {
            r->state = OWN_QUARANTINED;
            r->result = OWN_UNCERTAIN;
            r->revision++;
        }
    }
}

int own_retry(own_ledger *l)
{
    if (!l->finalized || !l->transport_closed || l->dirty_local) return 0;
    if ((l->session.attempt & UINT32_MAX) == UINT32_MAX) {
        /* This guard runs only after known local finalization. */
        own_release(l, SESSION_AMBIGUOUS, l->now);
        session_cleanup_complete(&l->session, CLEANUP_CONFIRMED);
        return 0;
    }
    for (unsigned i = 0; i < OWN_MAX; i++)
        if (l->records[i].state != OWN_FREE && l->records[i].state != OWN_CLOSED)
            return 0;
    for (unsigned i = 0; i < OWN_LINK_MAX; i++)
        if (l->links[i].state != LINK_FREE) return 0;
    if (!session_retry(&l->session)) return 0;
    memset(l->records, 0, sizeof l->records);
    l->setup_retired = l->rand_resent = 0;
    l->failed_opcode = l->reply_timeout = 0;
    l->transport_closed = l->finalize_claimed = l->finalized = 0;
    l->cleanup_deadline = 0;
    l->epoch++;
    return 1;
}

static own_result target_valid(own_ledger *l, const own_command *c, const descriptor *d)
{
    own_link *p = link_at(l, c->link);
    if (c->link.generation && !p) return OWN_INVALID;
    if (p && (p->state == RETIRING || p->state == RETIRED || p->state == LINK_QUARANTINED))
        return OWN_NO_PROOF;
    if (d->class == CREATE &&
        (!p || p->state != RESERVED || !same_peer(&p->peer, &c->peer))) return OWN_NO_PROOF;
    if (d->class == CREATE) {
        unsigned address_offset = c->opcode == 0x200d ? 6 : 0;
        if (memcmp(c->params + address_offset, c->peer.wire, 6)) return OWN_INVALID;
        if (c->opcode == 0x200d &&
            (c->params[4] != 0 || c->params[5] != c->peer.address_type))
            return OWN_INVALID; /* Only direct, expected-peer initiation is modeled. */
    }
    if ((d->class == REPLY || c->opcode == 0x0411 || c->opcode == 0x0413 ||
         c->opcode == 0x2013 || c->opcode == 0x2019 || c->opcode == 0x2026) && !p)
        return OWN_NO_PROOF;
    if (d->class == REPLY &&
        (!same_peer(&p->peer, &c->peer) || memcmp(c->params, p->peer.wire, 6) ||
         (c->opcode == 0x040a && p->state != RESERVED))) return OWN_NO_PROOF;
    if (d->class == REPLY && !mutation("reply_without_owned_peer")) {
        own_record *owner = record(l, p->owner);
        if (!owner || owner->state != OWN_ACKNOWLEDGED || !owner->lease ||
            !p->has_handle || (p->state != UP && p->state != READY))
            return OWN_NO_PROOF;
    }
    if (c->opcode == 0x0411 || c->opcode == 0x0413 || c->opcode == 0x2013 ||
        c->opcode == 0x2019 || c->opcode == 0x2026) {
        own_record *owner = record(l, p->owner);
        if (!owner || owner->state != OWN_ACKNOWLEDGED || !owner->lease || !p->has_handle ||
            (p->state != UP && p->state != READY) || !same_peer(&p->peer, &c->peer))
            return OWN_NO_PROOF;
        if (c->opcode != 0x2026 &&
            ((uint16_t)c->params[0] | ((uint16_t)c->params[1] << 8)) != p->handle)
            return OWN_NO_PROOF;
    }
    if (d->class == SETTING && c->link.generation) return OWN_INVALID;
    return OWN_OK;
}

own_result own_prepare(own_ledger *l, const own_command *c, own_ref *out)
{
    if (!admits(l)) return OWN_STOPPED;
    unsigned d;
    for (d = 0; d < sizeof commands / sizeof *commands; d++)
        if (commands[d].opcode == c->opcode) break;
    if (d == sizeof commands / sizeof *commands || commands[d].class == DISCONNECT ||
        commands[d].class == BARRIER_ONLY ||
        c->plen != commands[d].plen || !c->ack_by || c->terminal_by < c->ack_by ||
        (c->original_known && c->original_len != c->plen)) return OWN_INVALID;
    if (c->opcode == 0x1004 && c->params[0] != 1) return OWN_INVALID;
    /* HCI has no invocation ID: surplus replies are safe only if reuse is denied. */
    if ((l->setup_retired & (UINT64_C(1) << d)) && !c->idle_probe) return OWN_NO_PROOF;
    if (c->idle_probe && (c->opcode != 0x1005 || c->setup_retry ||
        l->session.state != SESSION_ACTIVE || c->link.generation ||
        c->original_known || c->original_len)) return OWN_INVALID;
    if (c->setup_retry && (l->session.state != SESSION_STARTING || l->generation ||
        (commands[d].class != QUERY && commands[d].class != SETTING))) return OWN_INVALID;
    own_result target = target_valid(l, c, &commands[d]);
    if (target != OWN_OK) return target;
    own_link *p = link_at(l, c->link);
    own_record *previous = NULL, *free_record = NULL;
    int unchanged = 0;
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        if (r->state == OWN_FREE) { if (!free_record) free_record = r; continue; }
        if (r->state == OWN_CLOSED) continue;
        unsigned cls = commands[r->desc].class;
        if (commands[d].terminal != OWN_TERM_COMMAND &&
            commands[d].terminal == commands[r->desc].terminal &&
            same_link(c->link, r->c.link)) return OWN_BUSY;
        if ((commands[d].class == CREATE && cls == CREATE &&
             (!r->terminal_seen || !r->barrier_done)) ||
            (commands[d].class == CRYPTO && cls == CRYPTO) ||
            (c->opcode == 0x0401 && r->c.opcode == 0x0401)) return OWN_BUSY;
        if (commands[d].class == SETTING && c->opcode == r->c.opcode) {
            if (r->state != OWN_ACKNOWLEDGED || r->barrier_pending || r->undo_consumed)
                return OWN_BUSY;
            if (r->c.plen == c->plen && memcmp(r->c.params, c->params, c->plen) == 0)
                unchanged = 1;
            previous = r;
        }
    }
    if (unchanged) return OWN_UNCHANGED;
    if (!free_record) return OWN_NO_SPACE;
    if (l->serial == UINT64_MAX || l->epoch == UINT64_MAX) {
        own_detector_trip(l, l->now);
        return OWN_STOPPED;
    }
    own_record *r = free_record;
    memset(r, 0, sizeof *r);
    r->ref = (own_ref){ l->session.attempt, ++l->serial };
    r->state = OWN_PREPARED;
    r->result = OWN_OK;
    r->revision = 1;
    r->desc = d;
    r->c = *c;
    /* Setup replies may cross opens; they cannot authorize a restore. */
    r->proof_veto = c->setup_retry && l->policy == OWN_POLICY_SINGLE_PRODUCER;
    /* Only actual bytes are retained; callers' unused buffers are not evidence. */
    memset(r->c.params + c->plen, 0, sizeof r->c.params - c->plen);
    memset(r->c.original + c->original_len, 0, sizeof r->c.original - c->original_len);
    if (previous) {
        r->previous = previous->ref;
        r->c.original_known = previous->c.original_known;
        r->c.original_len = previous->c.original_len;
        memcpy(r->c.original, previous->c.original, sizeof r->c.original);
    }
    if (p && commands[d].class == CREATE) p->owner = r->ref;
    *out = r->ref;
    return OWN_OK;
}

own_result own_status(const own_ledger *l, own_ref ref, own_state *out)
{
    const own_record *r = const_record(l, ref);
    if (!r) return OWN_INVALID;
    if (out) *out = r->state;
    return r->result;
}

uint64_t own_wait_deadline(const own_ledger *l, own_ref ref)
{
    const own_record *r = const_record(l, ref);
    return r ? (r->setup_by ? r->setup_by : r->c.ack_by) : 0;
}

own_result own_reclaim(own_ledger *l, own_ref ref)
{
    own_record *r = record(l, ref);
    if (!r) return OWN_INVALID;
    if (r->state != OWN_CLOSED || r->submit_pending || same_ref(l->waiter, ref))
        return OWN_BUSY;
    for (unsigned i = 0; i < OWN_MAX; i++)
        if (same_ref(l->records[i].previous, ref)) return OWN_BUSY;
    for (unsigned i = 0; i < OWN_LINK_MAX; i++)
        if (l->links[i].state != LINK_FREE && same_ref(l->links[i].owner, ref))
            return OWN_BUSY;
    wipe(r, sizeof *r);
    return OWN_OK;
}

static own_token token_for(const own_ledger *l, const own_record *r,
                           const own_link *p, own_effect_kind kind, own_phase phase)
{
    own_token t = {{0}};
    t._opaque[0] = l->epoch;
    t._opaque[1] = r ? r->ref.attempt : p ? p->ref.attempt : l->session.attempt;
    t._opaque[2] = r ? r->ref.serial : 0;
    t._opaque[3] = r ? r->revision : p ? p->revision : l->session.revision;
    t._opaque[4] = kind;
    t._opaque[5] = phase;
    t._opaque[6] = p ? p->ref.generation : 0;
    t._opaque[7] = p ? p->ref.slot : 0;
    return t;
}

static int link_fresh(const own_ledger *l, const own_record *r, uint64_t now)
{
    own_link *p = link_at((own_ledger *)l, r->c.link);
    return p && p->has_handle && p->usable && now >= p->last_input &&
           now - p->last_input <= OWN_FRESH_MS && same_ref(p->owner, r->ref) &&
           p->state != LINK_QUARANTINED;
}

static int undo_eligible(const own_ledger *l, const own_record *r, uint64_t now)
{
    if (l->setup_retired & (UINT64_C(1) << r->desc)) return 0;
    if (l->policy != OWN_POLICY_SINGLE_PRODUCER || l->ambiguities ||
        !before_cutoff(l, now) || r->state != OWN_ACKNOWLEDGED ||
        !r->acked || !r->lease || r->proof_veto || r->undo_consumed) return 0;
    if (commands[r->desc].class == SETTING) {
        /* The predecessor holds the one continuing lease. A replacement
         * suspends its use, not its veto history; never copy that authority. */
        for (unsigned i = 0; i < OWN_MAX; i++)
            if (same_ref(l->records[i].previous, r->ref) &&
                l->records[i].state != OWN_FREE && l->records[i].state != OWN_CLOSED)
                return 0;
        return r->c.original_known && r->c.original_len == r->c.plen;
    }
    return commands[r->desc].class == CREATE && r->terminal_seen &&
           !r->terminal_failed && r->barrier_done && link_fresh(l, r, now);
}

own_result own_request_undo(own_ledger *l, own_ref ref, uint64_t now, own_token *out)
{
    own_tick(l, now);
    own_record *r = record(l, ref);
    if (!r) return OWN_INVALID;
    if (r->c.setup_retry && l->policy == OWN_POLICY_SINGLE_PRODUCER) { own_detector_trip(l, now); return OWN_NO_PROOF; }
    if (l->waiter.serial) return OWN_BUSY;
    if (admits(l))
        for (unsigned i = 0; i < OWN_MAX; i++)
            if (l->records[i].barrier_pending) return OWN_BUSY;
    if (!undo_eligible(l, r, now)) return OWN_NO_PROOF;
    *out = token_for(l, r, NULL, OWN_EFFECT_HCI, OWN_UNDO);
    return OWN_OK;
}

own_result own_next_hci(own_ledger *l, uint64_t now, own_token *out)
{
    own_tick(l, now);
    if (!before_cutoff(l, now)) return OWN_STOPPED;
    if (l->waiter.serial) {
        own_record *r = record(l, l->waiter);
        if (!r->retry_pending) return OWN_BUSY;
        *out = token_for(l, r, NULL, OWN_EFFECT_HCI, l->waiting_phase);
        return OWN_OK;
    }
    own_record *normal = NULL, *restore = NULL;
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        if (r->barrier_pending && admits(l) && l->rand_supported) {
            *out = token_for(l, r, NULL, OWN_EFFECT_HCI, OWN_BARRIER);
            return OWN_OK;
        }
        if (r->state == OWN_PREPARED && (!normal || r->ref.serial < normal->ref.serial))
            normal = r;
        if (l->session.cleanup_pending && undo_eligible(l, r, now)) {
            if (commands[r->desc].class == CREATE) {
                *out = token_for(l, r, NULL, OWN_EFFECT_HCI, OWN_UNDO);
                return OWN_OK;
            }
            if (!restore || r->ref.serial > restore->ref.serial) restore = r;
        }
    }
    if (restore) {
        *out = token_for(l, restore, NULL, OWN_EFFECT_HCI, OWN_UNDO);
        return OWN_OK;
    }
    if (!normal || !admits(l)) return OWN_STOPPED;
    *out = token_for(l, normal, NULL, OWN_EFFECT_HCI, OWN_FORWARD);
    return OWN_OK;
}

static int local_kind(const own_ledger *l, const own_link *p)
{
    if (p->state == LINK_FREE) return -1;
    if (p->state == RETIRING || p->state == LINK_QUARANTINED || p->state == RETIRED) {
        if (p->connected && !p->retired) return OWN_EFFECT_RETIRE;
        if (p->device_id && p->add_ok && !p->delete_claimed &&
            (p->vstate == V_IDENTIFIED || p->vstate == V_READY || p->vstate == V_FAILED))
            return OWN_EFFECT_DELETE;
        return -1;
    }
    if (!session_accepts(&l->session) || !p->usable) return -1;
    if (!p->connected) return OWN_EFFECT_CONNECT;
    if (p->vstate == V_QUEUED) return OWN_EFFECT_ADD;
    if (p->vstate == V_IDENTIFIED) return OWN_EFFECT_BIND;
    if (p->vstate == V_READY && p->input_pending) return OWN_EFFECT_INPUT;
    return -1;
}

static int bounded_retry(const own_ledger *l, const own_record *r)
{
    return (r->c.idle_probe && l->session.state == SESSION_ACTIVE) ||
           (r->c.setup_retry && l->session.state == SESSION_STARTING && !l->generation);
}

own_result own_next_local(own_ledger *l, uint64_t now, own_token *out)
{
    if (l->finalized) return OWN_STOPPED;
    own_tick(l, now);
    /* The cutoff stops optional draining/HCI, never mandatory local closure.
     * The platform must bound these calls; a late caller does not renew D. */
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        own_link *p = &l->links[i];
        int kind = local_kind(l, p);
        if (kind >= 0) {
            *out = token_for(l, NULL, p, (own_effect_kind)kind, OWN_FORWARD);
            return OWN_OK;
        }
    }
    if (l->transport_closed && l->session.cleanup_pending && !l->finalize_claimed) {
        *out = token_for(l, NULL, NULL, OWN_EFFECT_FINALIZE, OWN_FORWARD);
        return OWN_OK;
    }
    return OWN_STOPPED;
}

own_result own_commit(own_ledger *l, const own_token *t, uint64_t now, own_effect *out)
{
    if (l->finalized) return OWN_STOPPED;
    own_tick(l, now);
    if ((t->_opaque[0] != l->epoch &&
         !(mutation("deadline_is_teardown") && l->session.deadline)) ||
        t->_opaque[4] > OWN_EFFECT_FINALIZE)
        return OWN_INVALID;
    own_effect e = {0};
    e.kind = (own_effect_kind)t->_opaque[4];
    if (e.kind != OWN_EFFECT_HCI) {
        if (e.kind == OWN_EFFECT_FINALIZE) {
            own_token expected = token_for(l, NULL, NULL, e.kind, OWN_FORWARD);
            if (!l->transport_closed || !l->session.cleanup_pending || l->finalize_claimed ||
                memcmp(t, &expected, sizeof *t)) return OWN_INVALID;
            l->finalize_claimed = 1;
            *out = e;
            return OWN_OK;
        }
        link_ref ref = {t->_opaque[1], t->_opaque[6], (uint8_t)t->_opaque[7]};
        own_link *p = link_at(l, ref);
        if (!p || p->revision != t->_opaque[3] || local_kind(l, p) != (int)e.kind)
            return OWN_INVALID;
        e.link = p->ref;
        e.owner = p->owner;
        e.handle = p->handle;
        e.device_id = p->device_id;
        p->revision++;
        p->committed = *t;
        p->local_pending = (uint8_t)e.kind + 1;
        switch (e.kind) {
        case OWN_EFFECT_CONNECT: p->connected = 1; p->vstate = V_QUEUED; break;
        case OWN_EFFECT_RETIRE:
            if (!mutation("claims_after_callback")) p->retired = 1;
            break;
        case OWN_EFFECT_ADD: p->vstate = V_ADDING; break;
        case OWN_EFFECT_BIND: p->vstate = V_BINDING; break;
        case OWN_EFFECT_INPUT: p->input_pending = 0; break;
        case OWN_EFFECT_DELETE:
            if (!mutation("claims_after_callback")) p->delete_claimed = 1;
            break;
        default: return OWN_INVALID;
        }
        *out = e;
        return OWN_OK;
    }
    own_ref ref = {t->_opaque[1], t->_opaque[2]};
    own_record *r = record(l, ref);
    if (!r || r->revision != t->_opaque[3] || t->_opaque[5] > OWN_UNDO)
        return OWN_INVALID;
    int retry = r->retry_pending && same_ref(l->waiter, ref) &&
                t->_opaque[5] == l->waiting_phase;
    if ((l->waiter.serial && !retry) || !before_cutoff(l, now)) return OWN_BUSY;
    e.owner = r->ref;
    e.link = r->c.link;
    e.phase = (own_phase)t->_opaque[5];
    const descriptor *d = &commands[r->desc];
    if (e.phase == OWN_FORWARD) {
        int admitted = admits(l);
        if ((r->state != OWN_PREPARED && !retry) ||
            (!admitted && !(mutation("deadline_is_teardown") && l->session.deadline)))
            return OWN_STOPPED;
        if ((l->setup_retired & (UINT64_C(1) << r->desc)) && !retry && !r->c.idle_probe)
            return OWN_NO_PROOF;
        own_result target = target_valid(l, &r->c, d);
        if (target != OWN_OK) {
            r->state = OWN_CLOSED;
            r->result = OWN_STOPPED;
            r->revision++;
            return target;
        }
        /* A barrier must be the next command, including when this token predates it. */
        for (unsigned i = 0; i < OWN_MAX; i++)
            if (l->records[i].barrier_pending) return OWN_BUSY;
        own_record *prev = record(l, r->previous);
        if (prev) {
            if (prev->state != OWN_ACKNOWLEDGED || prev->undo_consumed) return OWN_BUSY;
            prev->revision++;
        }
        e.opcode = r->c.opcode;
        e.plen = r->c.plen;
        memcpy(e.params, r->c.params, e.plen);
        r->state = OWN_SUBMITTED;
        r->phase_ack_by = r->c.ack_by;
        r->phase_terminal_by = r->c.terminal_by;
        own_link *p = link_at(l, r->c.link);
        if (d->class == CREATE && p) { p->state = CONNECTING; p->revision++; }
    } else if (e.phase == OWN_BARRIER) {
        /* A repeated Rand can leave a different, unseen value behind. */
        if (l->rand_resent && !retry) { own_detector_trip(l, now); return OWN_NO_PROOF; }
        if (!admits(l) || !l->rand_supported || !r->barrier_pending ||
            (r->state != OWN_ACKNOWLEDGED &&
             !(r->state == OWN_SUBMITTED && r->result == OWN_REFUSED &&
               r->forward_status))) return OWN_STOPPED;
        e.opcode = 0x2018;
        r->phase_ack_by = bounded_add(now, 1000);
    } else {
        if (admits(l))
            for (unsigned i = 0; i < OWN_MAX; i++)
                if (l->records[i].barrier_pending) return OWN_BUSY;
        if (!undo_eligible(l, r, now)) return OWN_NO_PROOF;
        r->undo_consumed = 1; /* Claim before returning bytes to any external call. */
        r->state = OWN_UNDO_SUBMITTED;
        r->phase_ack_by = bounded_add(now, 1000);
        r->phase_terminal_by = bounded_add(now, 2000);
        if (d->class == CREATE) {
            own_link *p = link_at(l, r->c.link);
            e.opcode = 0x0406;
            e.handle = p->handle;
            e.plen = 3;
            e.params[0] = (uint8_t)p->handle;
            e.params[1] = (uint8_t)(p->handle >> 8);
            e.params[2] = 0x13;
            p->state = RETIRING;
            p->input_pending = 0;
            if (p->vstate == V_QUEUED) p->vstate = V_FREE;
            p->revision++;
        } else {
            e.opcode = r->c.opcode;
            e.plen = r->c.original_len;
            memcpy(e.params, r->c.original, e.plen);
        }
    }
    if (bounded_retry(l, r) && e.phase != OWN_UNDO) {
        if (!retry) {
            r->setup_sends = 0;
            r->setup_by = bounded_add(now, OWN_SETUP_MS);
            if (r->c.terminal_by < r->setup_by)
                r->setup_by = r->c.terminal_by;
        }
        e.resend = r->setup_sends++;
        r->phase_ack_by = bounded_add(now, r->setup_sends == 3 ? 3000 : 1000);
        if (r->phase_ack_by > r->setup_by) r->phase_ack_by = r->setup_by;
        /* Probe replies can prove only traffic, never later buffer-size data. */
        if (retry || r->c.idle_probe) {
            if (e.phase == OWN_BARRIER) l->rand_resent = 1;
            else l->setup_retired |= UINT64_C(1) << r->desc;
        }
        r->retry_pending = 0;
    }
    r->revision++;
    r->committed = *t;
    r->submit_pending = 1;
    r->submitted_phase = (uint8_t)e.phase;
    l->waiter = r->ref;
    l->waiting_phase = e.phase;
    *out = e;
    return OWN_OK;
}

static void previous_settled(own_ledger *l, own_record *r, int success)
{
    own_record *previous = record(l, r->previous);
    if (previous && previous->state == OWN_ACKNOWLEDGED) {
        if (success) {
            previous->state = OWN_CLOSED;
            previous->result = OWN_TRANSFERRED;
            previous->lease = 0;
        }
        previous->revision++;
    }
    r->previous = (own_ref){0};
}

void own_submitted(own_ledger *l, const own_token *t, own_submit_result result, uint64_t now)
{
    own_ref ref = {t->_opaque[1], t->_opaque[2]};
    own_record *r = record(l, ref);
    if (!r || !r->submit_pending || memcmp(t, &r->committed, sizeof *t)) return;
    r->submit_pending = 0;
    if (result == OWN_SEND_UNCERTAIN) { own_detector_trip(l, now); return; }
    if (result == OWN_NOT_SENT) {
        if (r->setup_sends > 1) { own_detector_trip(l, now); return; }
        l->waiter = (own_ref){0};
        if (r->submitted_phase == OWN_FORWARD) {
            previous_settled(l, r, 0);
            r->state = OWN_CLOSED;
            r->result = OWN_REFUSED;
            own_link *p = link_at(l, r->c.link);
            if (p && commands[r->desc].class == CREATE) {
                memset(p, 0, sizeof *p); /* No wire/API acquisition to retire. */
            }
        } else if (r->submitted_phase == OWN_BARRIER) {
            /* Without the fence, the accepted forward ACK may be an old copy.
             * Do not open another invocation over its still-pending real answer. */
            own_detector_trip(l, now);
            return;
        } else {
            r->lease = 0;
            r->state = OWN_QUARANTINED;
            r->result = OWN_UNCERTAIN;
            l->session.unsafe = 1;
        }
        r->revision++;
        return;
    }
    if (r->submitted_phase == OWN_FORWARD) {
        r->sent = 1;
        if (mutation("ownership_on_sent")) {
            r->acked = r->lease = r->barrier_done = 1;
            r->state = OWN_ACKNOWLEDGED;
        }
    }
}

own_provenance own_backend_receipt(own_ledger *l, const own_ack *a, uint64_t now,
                                  own_receipt *out)
{
    receipt_data p = {0};
    if (l->receipt_issued == UINT64_MAX) own_detector_trip(l, now);
    else p.sequence = ++l->receipt_issued;
    p.event = a->event;
    p.opcode = a->opcode;
    p.status = a->status;
    p.random_len = a->random_len;
    for (unsigned i = 0; i < 8; i++) p.random |= (uint64_t)a->random[i] << (8*i);
    p.credit = a->opcode == 0 && !a->malformed && !a->overflow &&
               (a->event == OWN_CC || (a->event == OWN_CS && a->status == 0));
    own_record *r = record(l, l->waiter);
    if (r && !l->ambiguities && !l->transport_closed && !a->malformed && !a->overflow &&
        !r->submit_pending && (now < r->phase_ack_by ||
                              (r->retry_pending && now < r->setup_by))) {
        unsigned opcode = l->waiting_phase == OWN_BARRIER ? 0x2018 :
                          l->waiting_phase == OWN_UNDO &&
                          commands[r->desc].class == CREATE ? 0x0406 : r->c.opcode;
        unsigned event = l->waiting_phase == OWN_BARRIER ? OWN_CC :
                         l->waiting_phase == OWN_UNDO &&
                         commands[r->desc].class == CREATE ? OWN_CS : commands[r->desc].ack;
        if (a->opcode == opcode && a->event == event) {
            p.provenance = OWN_RECEIPT_SEQUENCED;
            p.attempt = r->ref.attempt;
            p.serial = r->ref.serial;
            p.phase = l->waiting_phase;
        }
    }
    if (!p.provenance && !a->malformed && !a->overflow && !a->status && a->event == OWN_CC) {
        for (unsigned d = 0; d < sizeof commands / sizeof *commands; d++)
            if (a->opcode == commands[d].opcode &&
                ((l->setup_retired & (UINT64_C(1) << d)) ||
                 (a->opcode == 0x2018 && l->rand_resent)))
                p.provenance = OWN_RECEIPT_RETIRED;
    }
    memset(out, 0, sizeof *out);
    memcpy(out, &p, sizeof p);
    l->issued_receipt = *out;
    return (own_provenance)p.provenance;
}

static void bind_link(own_ledger *l, own_record *r, uint64_t now)
{
    if (commands[r->desc].class != CREATE || !r->acked || !r->barrier_done ||
        !r->terminal_seen || r->terminal_failed || r->state == OWN_QUARANTINED ||
        r->state == OWN_CLOSED) return;
    if (r->binding_revoked) {
        own_detector_trip(l, now);
        return;
    }
    own_link *p = link_at(l, r->c.link);
    if (!p || p->has_handle) return;
    uint16_t h = r->terminal.handle;
    if (h >= 0x0f00) {
        own_detector_trip(l, now);
        if (p) p->state = LINK_QUARANTINED;
        return;
    }
    p->handle = h;
    p->has_handle = 1;
    p->state = admits(l) ? UP : RETIRING;
    p->revision++;
    r->lease = 1;
}

void own_receive(own_ledger *l, const own_receipt *receipt, uint64_t now)
{
    receipt_data p;
    memcpy(&p, receipt, sizeof p);
    if (p.sequence && p.sequence <= l->receipt_seen) {
        if (mutation("rearm_undo_duplicate")) {
            own_record *r = record(l, (own_ref){p.attempt, p.serial});
            if (r && r->undo_consumed) {
                r->undo_consumed = 0;
                r->state = OWN_ACKNOWLEDGED;
            }
        }
        return;
    }
    if (l->transport_closed) return;
    if (!p.sequence || memcmp(receipt, &l->issued_receipt, sizeof *receipt)) {
        own_detector_trip(l, now);
        return;
    }
    l->receipt_seen = p.sequence;
    own_tick(l, now);
    if (p.credit) { l->credits++; return; }
    if (p.provenance == OWN_RECEIPT_RETIRED) return;
    own_ref ref = {p.attempt, p.serial};
    own_record *r = record(l, ref);
    if (p.provenance == OWN_RECEIPT_SEQUENCED && r &&
        same_ref(l->waiter, ref) && p.phase == l->waiting_phase &&
        p.phase == OWN_UNDO && r->state == OWN_QUARANTINED) {
        /* A terminal refusal may precede CS. Settle its ACK diagnostically;
         * quarantine and consumed authority remain unchanged. */
        l->waiter = (own_ref){0};
        r->undo_ack = 1;
        r->undo_status = (uint8_t)p.status;
        r->revision++;
        return;
    }
    if (p.provenance != OWN_RECEIPT_SEQUENCED || !r ||
        !same_ref(l->waiter, ref) || p.phase != l->waiting_phase ||
        r->state == OWN_QUARANTINED || r->state == OWN_CLOSED) {
        own_detector_trip(l, now);
        return;
    }
    l->waiter = (own_ref){0};
    r->revision++;
    if (p.phase == OWN_UNDO) {
        r->undo_ack = 1;
        r->undo_status = (uint8_t)p.status;
        if (p.status) {
            r->state = OWN_QUARANTINED;
            r->lease = 0;
            r->result = OWN_UNCERTAIN;
            l->session.unsafe = 1;
        } else {
            if (commands[r->desc].class == SETTING || r->undo_terminal) {
                r->state = OWN_CLOSED;
                r->lease = 0;
            }
        }
        return;
    }
    if (p.phase == OWN_BARRIER) {
        r->barrier_pending = 0;
        if (p.status || p.random_len != 8) { own_detector_trip(l, now); return; }
        for (unsigned i = 0; i < l->barrier_count; i++)
            if (p.random == l->barrier_values[i]) { own_detector_trip(l, now); return; }
        int space = l->barrier_count < OWN_BARRIER_MAX;
        if (space) l->barrier_values[l->barrier_count++] = p.random;
        if (r->forward_status) {
            /* A copied old refusal must not unsuspend a predecessor lease.
             * The real answer must settle before this fence, just as for success. */
            own_record *previous = record(l, r->previous);
            if (previous && (!space || !admits(l) || l->ambiguities))
                previous->lease = 0;
            previous_settled(l, r, 0);
            r->state = OWN_CLOSED;
            return;
        }
        if (space && admits(l) && !l->ambiguities) {
            r->barrier_done = 1;
            own_record *previous = record(l, r->previous);
            if (commands[r->desc].class == SETTING &&
                l->policy == OWN_POLICY_SINGLE_PRODUCER && r->c.original_known &&
                !r->proof_veto &&
                (!r->previous.serial ||
                 (previous && previous->state == OWN_ACKNOWLEDGED && previous->lease)))
                r->lease = 1;
            bind_link(l, r, now);
        }
        previous_settled(l, r, 1);
        if (r->terminal_failed) r->state = OWN_CLOSED;
        return;
    }
    r->forward_status = (uint8_t)p.status;
    r->forward_receipt = p.sequence;
    if (p.status) {
        if (r->setup_sends > 1 && commands[r->desc].class == SETTING) {
            own_detector_trip(l, now); return;
        }
        own_record *previous = record(l, r->previous);
        r->result = OWN_REFUSED;
        if (previous && previous->lease && commands[r->desc].class == SETTING) {
            if (l->rand_supported && admits(l)) {
                r->barrier_pending = 1;
                r->terminal_seen = r->terminal_failed = 1;
                /* Forward acquisition was refused; only continuity is pending. */
                r->state = OWN_SUBMITTED;
                return;
            }
            previous->lease = 0;
        }
        previous_settled(l, r, 0);
        r->state = OWN_CLOSED;
        r->result = OWN_REFUSED;
        r->lease = 0;
        own_link *link = link_at(l, r->c.link);
        if (commands[r->desc].class == CREATE && link) {
            link->state = RETIRED;
            link->revision++;
        }
        return;
    }
    r->acked = 1;
    r->state = OWN_ACKNOWLEDGED;
    if (commands[r->desc].terminal == OWN_TERM_COMMAND) r->terminal_seen = 1;
    if (commands[r->desc].class == SETTING || commands[r->desc].class == CREATE) {
        r->barrier_pending = l->rand_supported && admits(l) && !r->terminal_failed;
        if (mutation("ack_by_opcode")) {
            r->lease = r->barrier_done = 1;
            r->barrier_pending = 0;
            previous_settled(l, r, 1);
            bind_link(l, r, now);
        }
        if (!r->barrier_pending) previous_settled(l, r, 1);
    } else if (r->terminal_seen) r->state = OWN_CLOSED;
    if (r->terminal_failed) r->state = OWN_CLOSED;
}

static void terminal_received(own_ledger *l, own_ref ref,
                              const own_terminal_event *event, uint64_t now)
{
    own_tick(l, now);
    own_record *r = record(l, ref);
    if (!r || l->transport_closed || r->state == OWN_QUARANTINED ||
        r->state == OWN_CLOSED || r->state == OWN_PREPARED) return;
    if (event->kind == OWN_TERM_DISCONNECT && commands[r->desc].class == CREATE) {
        own_link *p = link_at(l, r->c.link);
        if (event->status) {
            if (p && p->has_handle && p->handle == event->handle &&
                r->state == OWN_UNDO_SUBMITTED) {
                r->state = OWN_QUARANTINED;
                r->lease = 0;
                r->undo_terminal = 1;
                r->undo_terminal_status = event->status;
                r->result = OWN_UNCERTAIN;
                r->revision++;
                l->session.unsafe = 1;
            }
            return;
        }
        own_disconnected(l, r->c.link, event->handle, now);
        return;
    }
    if (event->kind != commands[r->desc].terminal || event->kind == OWN_TERM_COMMAND)
        return;
    if (event->kind == OWN_TERM_CONNECTION && !same_peer(&event->peer, &r->c.peer))
        return;
    if (r->terminal_seen) {
        if (r->terminal.status != event->status || r->terminal.handle != event->handle)
            own_detector_trip(l, now);
        return;
    }
    r->terminal = *event;
    r->terminal_seen = 1;
    r->terminal_failed = event->status != 0;
    if (event->status) {
        r->result = OWN_REFUSED;
        own_link *p = link_at(l, r->c.link);
        if (p && commands[r->desc].class == CREATE) {
            p->state = RETIRING;
            p->revision++;
        }
    }
    r->revision++;
    if (!r->acked) return;
    if (event->status || commands[r->desc].class != CREATE) {
        r->lease = 0;
        own_link *p = link_at(l, r->c.link);
        if (p && commands[r->desc].class == CREATE) {
            p->state = RETIRING;
            p->revision++;
        }
        if (!r->barrier_pending && !same_ref(l->waiter, r->ref)) r->state = OWN_CLOSED;
    } else bind_link(l, r, now);
}

void own_terminal(own_ledger *l, own_ref ref, const own_terminal_event *event, uint64_t now)
{
    /* Host dispatch consumes successes; only backend ingress may establish one. */
    if (event->kind == OWN_TERM_CONNECTION && !event->status) return;
    terminal_received(l, ref, event, now);
}

void own_setting_changed(own_ledger *l, uint16_t opcode, uint64_t now)
{
    for (unsigned i = 0; i < OWN_MAX; i++)
        if (l->records[i].state != OWN_FREE && l->records[i].c.opcode == opcode &&
            commands[l->records[i].desc].class == SETTING) {
            own_detector_trip(l, now);
            return;
        }
}

void own_readback(own_ledger *l, uint16_t opcode, const uint8_t *bytes, size_t n)
{
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        if (r->state == OWN_FREE || r->state == OWN_CLOSED || r->c.opcode != opcode ||
            commands[r->desc].class != SETTING) continue;
        if (n != r->c.plen || memcmp(bytes, r->c.params, n)) {
            r->lease = 0;
            r->proof_veto = 1;
            r->revision++;
        } else if (mutation("readback_is_lease")) {
            r->acked = r->lease = r->barrier_done = 1;
            r->state = OWN_ACKNOWLEDGED;
        }
    }
}

void own_tick(own_ledger *l, uint64_t now)
{
    if (now > l->now) l->now = now;
    if (l->transport_closed) return;
    own_record *r = record(l, l->waiter);
    if (r && (now >= r->phase_ack_by || !before_cutoff(l, now))) {
        if (bounded_retry(l, r) &&
            admits(l) && l->waiting_phase != OWN_UNDO &&
            r->setup_sends < 3 && now < r->setup_by) {
            if (!r->retry_pending) { r->retry_pending = 1; r->revision++; }
            return;
        }
        if (mutation("resend_missing_ack")) {
            r->state = OWN_PREPARED;
            r->c.ack_by = bounded_add(now, 1000);
            r->c.terminal_by = bounded_add(now, 2000);
            l->waiter = (own_ref){0};
        } else {
            if (!l->ambiguities) l->reply_timeout = 1;
            own_detector_trip(l, now);
        }
        return;
    }
    for (unsigned i = 0; i < OWN_MAX; i++) {
        r = &l->records[i];
        if (r->state == OWN_PREPARED && now >= r->c.ack_by) {
            r->state = OWN_CLOSED;
            r->result = OWN_REFUSED;
            r->previous = (own_ref){0}; /* Its predecessor was never suspended. */
            r->revision++;
            own_link *p = link_at(l, r->c.link);
            if (p && p->state == RESERVED && same_ref(p->owner, r->ref))
                memset(p, 0, sizeof *p);
        }
        if (r->state == OWN_ACKNOWLEDGED && !r->terminal_seen &&
            now >= r->c.terminal_by) { own_detector_trip(l, now); return; }
        if (r->state == OWN_UNDO_SUBMITTED && r->undo_ack && !r->undo_terminal &&
            commands[r->desc].class == CREATE && now >= r->phase_terminal_by) {
            r->state = OWN_QUARANTINED;
            r->lease = 0;
            r->result = OWN_UNCERTAIN;
            l->session.unsafe = 1;
        }
    }
}

int own_drain_allowed(const own_ledger *l, uint64_t now, unsigned completed)
{
    if (completed > 64) return 0;
    if (mutation("deadline_only_idle") && completed) return 1;
    return before_cutoff(l, now);
}

own_result own_reserve_link(own_ledger *l, const own_peer *peer, link_ref *out)
{
    if (!admits(l)) return OWN_STOPPED;
    for (unsigned i = 0; i < OWN_LINK_MAX; i++)
        if (l->links[i].state != LINK_FREE && same_peer(&l->links[i].peer, peer))
            return OWN_BUSY;
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        own_link *p = &l->links[i];
        if (p->state != LINK_FREE) continue;
        if (l->generation == UINT64_MAX) {
            own_detector_trip(l, l->now);
            return OWN_STOPPED;
        }
        memset(p, 0, sizeof *p);
        p->ref = (link_ref){ l->session.attempt, ++l->generation, (uint8_t)i };
        p->revision = 1;
        p->peer = *peer;
        p->state = RESERVED;
        *out = p->ref;
        return OWN_OK;
    }
    return OWN_NO_SPACE;
}

int own_link_current(const own_ledger *l, link_ref ref)
{
    const own_link *p = link_at((own_ledger *)l, ref);
    const own_record *r = p ? const_record(l, p->owner) : NULL;
    return p && (!r || !r->binding_revoked);
}

int own_link_retained(const own_ledger *l, link_ref ref)
{
    return link_at((own_ledger *)l, ref) != NULL;
}

own_result own_input(own_ledger *l, link_ref ref, uint16_t handle, uint64_t now)
{
    own_tick(l, now);
    if (!session_accepts(&l->session)) return OWN_STOPPED;
    own_link *p = link_at(l, ref);
    if (!p || (p->state != UP && p->state != READY) || !p->has_handle ||
        p->handle != handle) return OWN_NO_PROOF;
    own_record *r = record(l, p->owner);
    if (!r || !r->lease) return OWN_NO_PROOF;
    p->usable = p->input_pending = 1;
    p->last_input = now;
    p->revision++;
    return OWN_OK;
}

void own_backend_local_result(own_ledger *l, const own_token *t, int success)
{
    if (l->finalized) return;
    if (t->_opaque[4] == OWN_EFFECT_FINALIZE) return;
    link_ref ref = {t->_opaque[1], t->_opaque[6], (uint8_t)t->_opaque[7]};
    own_link *p = link_at(l, ref);
    if (!p || !p->local_pending || memcmp(t, &p->committed, sizeof *t)) return;
    p->local_pending = 0;
    p->revision++;
    switch ((own_effect_kind)t->_opaque[4]) {
    case OWN_EFFECT_RETIRE: p->retired = 1; break;
    case OWN_EFFECT_ADD:
        p->add_ok = !!success;
        p->vstate = success ? V_PENDING_ID : V_FAILED;
        break;
    case OWN_EFFECT_BIND:
        p->vstate = success ? V_READY : V_FAILED;
        if (success && session_accepts(&l->session) && p->state == UP) p->state = READY;
        else if (p->state != RETIRING) p->state = RETIRING;
        break;
    case OWN_EFFECT_DELETE:
        p->delete_claimed = 1;
        p->delete_ok = !!success;
        p->vstate = V_DELETED;
        if (!success) l->dirty_local = 1;
        break;
    default: break;
    }
}

void own_backend_identity(own_ledger *l, link_ref ref, uint64_t device_id, int proven)
{
    if (l->finalized) return;
    own_link *p = link_at(l, ref);
    if (!p || p->vstate != V_PENDING_ID || !p->add_ok || !device_id || !proven) return;
    p->device_id = device_id;
    p->vstate = V_IDENTIFIED;
    p->revision++;
}

void own_disconnected(own_ledger *l, link_ref ref, uint16_t handle, uint64_t now)
{
    (void)now;
    own_link *p = link_at(l, ref);
    if (!p) return;
    own_record *r = record(l, p->owner);
    if (!p->has_handle) {
        if (!r || !r->terminal_seen || r->terminal.handle != handle ||
            r->terminal_failed) return;
        /* The controller can disconnect before CS/the barrier publishes the link. */
        r->terminal_failed = 1;
        if (handle < 0x0f00)
            l->handles_seen[handle/8] |= (uint8_t)(1u << (handle%8));
    } else if (p->handle != handle) return;
    p->has_handle = 0;
    p->state = RETIRING;
    p->input_pending = 0;
    if (p->vstate == V_QUEUED) p->vstate = V_FREE;
    p->revision++;
    if (r) {
        r->lease = 0;
        r->undo_terminal = 1;
        if (r->state == OWN_ACKNOWLEDGED) {
            if (!same_ref(l->waiter, r->ref)) {
                r->barrier_pending = 0;
                r->state = OWN_CLOSED;
            }
        } else if (r->state == OWN_UNDO_SUBMITTED && r->undo_ack) {
            r->state = OWN_CLOSED;
        }
        r->revision++;
    }
}

own_result own_detach_link(own_ledger *l, link_ref ref)
{
    own_link *p = link_at(l, ref);
    if (!p) return OWN_INVALID;
    if ((p->has_handle && !l->transport_closed) ||
        (p->connected && !p->retired) || p->local_pending ||
        (p->vstate != V_FREE && p->vstate != V_FAILED && p->vstate != V_DELETED) ||
        (p->device_id && !p->delete_claimed)) return OWN_BUSY;
    if (p->state != RETIRING && p->state != RETIRED && p->state != LINK_QUARANTINED)
        return OWN_BUSY;
    own_record *r = record(l, p->owner);
    if (p->has_handle || (r && r->terminal_seen && !r->terminal_failed &&
                         !r->undo_terminal)) l->radio_left++;
    memset(p, 0, sizeof *p);
    return OWN_OK;
}

void own_transport_closed(own_ledger *l, uint64_t now)
{
    if (l->transport_closed) return;
    if (!l->session.cleanup_pending) own_fail(l, now, 1);
    l->transport_closed = 1;
    l->epoch++;
    l->waiter = (own_ref){0};
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        if (r->state == OWN_FREE) continue;
        if (r->state == OWN_SUBMITTED || r->state == OWN_UNDO_SUBMITTED ||
            r->state == OWN_QUARANTINED ||
            (r->state == OWN_ACKNOWLEDGED &&
             (!r->terminal_seen ||
              ((commands[r->desc].class == CREATE || commands[r->desc].class == SETTING) &&
               !r->barrier_done)))) {
            r->result = OWN_UNCERTAIN;
            l->session.unsafe = 1;
        }
        r->state = OWN_CLOSED;
        r->lease = r->barrier_pending = r->submit_pending = 0;
        r->previous = (own_ref){0};
        r->revision++;
    }
    for (unsigned i = 0; i < OWN_LINK_MAX; i++)
        if (l->links[i].state != LINK_FREE) {
            l->links[i].state = RETIRING;
            l->links[i].input_pending = 0;
            l->links[i].revision++;
        }
}

void own_finalized(own_ledger *l, enum cleanup_result result)
{
    if (!l->finalize_claimed || l->finalized) return;
    l->finalized = 1;
    for (unsigned i = 0; i < OWN_LINK_MAX; i++)
        if (l->links[i].state != LINK_FREE &&
            own_detach_link(l, l->links[i].ref) != OWN_OK) l->dirty_local = 1;
    if (l->dirty_local && result == CLEANUP_CONFIRMED) result = CLEANUP_UNCERTAIN;
    session_cleanup_complete(&l->session, result);
}

void own_snapshot(const own_ledger *l, own_summary *out)
{
    *out = (own_summary){ .runtime = l->session, .barriers = l->barrier_count,
        .ambiguity_count = l->ambiguities, .credit_nops = l->credits,
        .transport_closed = l->transport_closed, .finalized = l->finalized,
        .radio_left = l->radio_left, .failed_opcode = l->failed_opcode,
        .reply_timeout = l->reply_timeout };
    for (unsigned i = 0; i < OWN_MAX; i++) {
        out->records += l->records[i].state != OWN_FREE;
        out->quarantined += l->records[i].state == OWN_QUARANTINED;
    }
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        const own_link *p = &l->links[i];
        const own_record *r = const_record(l, p->owner);
        out->links += p->state != LINK_FREE;
        out->radio_left += l->session.cleanup_pending &&
                          (p->has_handle || (r && r->terminal_seen &&
                                            !r->terminal_failed && !r->undo_terminal));
        out->unidentified += p->add_ok && !p->device_id;
    }
}

const char *own_invariant(const own_ledger *l)
{
    if (l->session.cleanup_pending && l->cleanup_deadline != l->session.deadline)
        return "I11: cleanup deadline changed";
    if (l->session.reason != SESSION_NONE &&
        l->session.state != SESSION_QUIESCING) return "I14: terminal runtime reopened";
    if (l->barrier_count > OWN_BARRIER_MAX) return "A8: barrier table overflow";
    if (l->waiter.serial && !const_record(l, l->waiter)) return "I7: waiter was reclaimed";
    if (l->waiter.serial && l->waiting_phase == OWN_UNDO &&
        !const_record(l, l->waiter)->undo_consumed) return "I2: in-flight undo unclaimed";
    for (unsigned i = 0; i < OWN_MAX; i++) {
        const own_record *r = &l->records[i];
        if (r->state == OWN_FREE) continue;
        if (!r->ref.serial || !r->ref.attempt) return "I6: absent record identity";
        if (r->setup_sends > 3 || (r->retry_pending && !r->c.setup_retry && !r->c.idle_probe))
            return "setup retry escaped its budget";
        if (r->c.idle_probe && (r->c.opcode != 0x1005 || r->c.link.generation ||
            r->lease || r->barrier_pending || r->undo_consumed))
            return "idle probe acquired authority";
        if (r->acked && (!r->sent || !r->forward_receipt))
            return "I1: acknowledgement without own sequenced receipt";
        if (r->state == OWN_QUARANTINED && r->lease) return "I7: quarantined lease";
        if (r->lease && (!r->acked || !r->barrier_done || l->ambiguities))
            return "I2: lease without barrier-confirmed acknowledgement";
        if (r->lease && r->proof_veto) return "I15: vetoed proof regained a lease";
        if (r->lease && r->binding_revoked) return "I10: revoked binding regained a lease";
        if (r->lease && r->result == OWN_REFUSED) return "I4: refusal acquired authority";
        if (r->lease && commands[r->desc].class == SETTING &&
            (!r->c.original_known || l->policy == OWN_POLICY_NO_RESTORE))
            return "I15: restoration without original/policy";
        if (r->undo_consumed && !r->acked) return "I2: undo without acquisition";
        if (r->state == OWN_UNDO_SUBMITTED && !r->undo_consumed)
            return "I2: unclaimed undo";
        for (unsigned j = i + 1; j < OWN_MAX; j++)
            if (l->records[j].state != OWN_FREE && same_ref(r->ref, l->records[j].ref))
                return "I6: reused record identity";
    }
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        const own_link *p = &l->links[i];
        if (p->state == LINK_FREE) continue;
        if (p->retired && !p->connected) return "I8: retirement without connect";
        if (p->delete_claimed && (!p->add_ok || !p->device_id))
            return "I9: deletion without acquired identity";
        if (p->has_handle && !l->transport_closed) {
            const own_record *r = const_record(l, p->owner);
            if (!r || !r->acked || !r->terminal_seen || !r->barrier_done ||
                r->binding_revoked)
                return "I10: local generation substituted for wire authority";
        }
        if (p->vstate == V_READY && (!p->usable || !p->device_id || !p->add_ok))
            return "I5: public ready before input and virtual readiness";
    }
    return NULL;
}

/* Variable-width, explicit little-endian encoding avoids padding, pointers and
 * physical free-table order. Identity and provenance are deliberately preserved.
 */
typedef struct { uint8_t *out; size_t n, capacity; } canonical_writer;
static void byte(canonical_writer *w, uint8_t v)
{
    if (w->n < w->capacity) w->out[w->n] = v;
    w->n++;
}
static void number(canonical_writer *w, uint64_t v)
{
    do { byte(w, (uint8_t)(v & 127) | (v > 127 ? 128 : 0)); v >>= 7; } while (v);
}
static void bytes(canonical_writer *w, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) byte(w, p[i]);
}
static void reference(canonical_writer *w, own_ref r)
{
    number(w, r.attempt); number(w, r.serial);
}
static void link_reference(canonical_writer *w, link_ref r)
{
    number(w, r.attempt); number(w, r.generation); number(w, r.slot);
}
static void token_bytes(canonical_writer *w, const own_token *t)
{
    for (unsigned i = 0; i < 8; i++) number(w, t->_opaque[i]);
}

size_t own_canonical(const own_ledger *l, uint8_t *out, size_t capacity)
{
    canonical_writer w = {out, 0, capacity};
#define N(x) number(&w, (uint64_t)(x))
    N(l->policy); N(l->session.state); N(l->session.reason);
    N(l->session.attempt); N(l->session.revision); N(l->session.deadline);
    N(l->session.recoverable); N(l->session.cleanup_pending); N(l->session.unsafe);
    N(l->serial); N(l->generation); N(l->epoch); N(l->cleanup_deadline); N(l->now);
    N(l->receipt_issued); N(l->receipt_seen); N(l->ambiguities); N(l->credits);
    N(l->rand_supported); N(l->transport_closed); N(l->finalize_claimed);
    N(l->finalized); N(l->dirty_local); N(l->radio_left);
    N(l->setup_retired); N(l->rand_resent);
    N(l->failed_opcode); N(l->reply_timeout);
    reference(&w, l->waiter);
    if (l->waiter.serial) N(l->waiting_phase);
    if (l->receipt_issued != l->receipt_seen) {
        receipt_data receipt;
        memcpy(&receipt, &l->issued_receipt, sizeof receipt);
        N(receipt.sequence); N(receipt.attempt); N(receipt.serial); N(receipt.phase);
        N(receipt.provenance); N(receipt.event); N(receipt.opcode); N(receipt.status);
        N(receipt.random); N(receipt.random_len); N(receipt.credit);
    }
    N(l->barrier_count);
    /* Barrier order has no meaning; sort a bounded copy for canonical equality. */
    uint64_t values[OWN_BARRIER_MAX];
    memcpy(values, l->barrier_values, l->barrier_count * sizeof *values);
    for (unsigned i = 1; i < l->barrier_count; i++) {
        uint64_t v = values[i]; unsigned j = i;
        while (j && values[j-1] > v) { values[j] = values[j-1]; j--; }
        values[j] = v;
    }
    for (unsigned i = 0; i < l->barrier_count; i++) N(values[i]);
    unsigned used = 0;
    for (unsigned i = 0; i < OWN_MAX; i++) used += l->records[i].state != OWN_FREE;
    N(used);
    uint64_t after = 0;
    for (unsigned ordinal = 0; ordinal < used; ordinal++) {
        const own_record *r = NULL;
        for (unsigned i = 0; i < OWN_MAX; i++)
            if (l->records[i].state != OWN_FREE && l->records[i].ref.serial > after &&
                (!r || l->records[i].ref.serial < r->ref.serial)) r = &l->records[i];
        if (!r) break;
        after = r->ref.serial;
        reference(&w, r->ref); reference(&w, r->previous);
        N(r->revision); N(r->forward_receipt); N(r->state); N(r->result);
        N(r->c.opcode); N(r->c.plen); bytes(&w, r->c.params, r->c.plen);
        N(r->c.original_known);
        if (r->c.original_known) {
            N(r->c.original_len); bytes(&w, r->c.original, r->c.original_len);
        }
        bytes(&w, (const uint8_t *)&r->c.peer, sizeof r->c.peer);
        link_reference(&w, r->c.link); N(r->c.ack_by); N(r->c.terminal_by);
        N(r->c.setup_retry); N(r->c.idle_probe); N(r->setup_by); N(r->setup_sends); N(r->retry_pending);
        N(r->sent); N(r->acked); N(r->terminal_seen); N(r->terminal_failed);
        N(r->lease); N(r->barrier_done); N(r->barrier_pending);
        N(r->undo_consumed); N(r->undo_ack); N(r->undo_terminal);
        N(r->submit_pending); N(r->submitted_phase);
        N(r->forward_status); N(r->undo_status); N(r->undo_terminal_status);
        N(r->proof_veto); N(r->binding_revoked);
        N(r->phase_ack_by); N(r->phase_terminal_by);
        if (r->terminal_seen && commands[r->desc].terminal != OWN_TERM_COMMAND) {
            N(r->terminal.kind); N(r->terminal.status); N(r->terminal.handle);
            bytes(&w, (const uint8_t *)&r->terminal.peer, sizeof r->terminal.peer);
        }
        if (r->submit_pending) token_bytes(&w, &r->committed);
    }
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        const own_link *p = &l->links[i];
        N(p->state);
        if (p->state == LINK_FREE) continue;
        link_reference(&w, p->ref); reference(&w, p->owner);
        bytes(&w, (const uint8_t *)&p->peer, sizeof p->peer);
        N(p->revision); N(p->device_id); N(p->last_input); N(p->handle);
        N(p->vstate); N(p->has_handle); N(p->usable); N(p->input_pending);
        N(p->connected); N(p->retired); N(p->delete_claimed); N(p->delete_ok); N(p->add_ok);
        N(p->local_pending);
        if (p->local_pending) token_bytes(&w, &p->committed);
    }
    used = 0;
    for (unsigned b = 0; b < 0x0f00/8; b++) {
        uint8_t v = l->handles_seen[b];
        while (v) { used += v & 1; v >>= 1; }
    }
    N(used);
    for (unsigned b = 0; b < 0x0f00/8; b++)
        if (l->handles_seen[b])
            for (unsigned bit = 0; bit < 8; bit++)
                if (l->handles_seen[b] & (1u << bit)) N(8*b + bit);
#undef N
    return w.n;
}

/* ACL capabilities are separate from the frozen HCI/local transition encoding.
 * One lane retains one packet; issuing or completing output grants no new lease.
 */
struct own_acl_lane {
    own_ledger *ledger;
    uint64_t instance, serial, epoch, attempt;
    link_ref link;
    own_ref owner;
    own_acl_token token;
    uint8_t packet[OWN_ACL_MAX];
    uint16_t len;
    uint8_t prepared, submit_pending;
    own_submit_result result;
};

own_acl_lane *own_acl_new(own_ledger *l)
{
    static uint64_t instance;
    if (!l || instance == UINT64_MAX) return NULL;
    own_acl_lane *lane = calloc(1, sizeof *lane);
    if (lane) {
        lane->ledger = l;
        lane->instance = ++instance;
    }
    return lane;
}

void own_acl_free(own_acl_lane *lane)
{
    if (!lane) return;
    wipe(lane, sizeof *lane);
    free(lane);
}

static own_result acl_allowed(own_ledger *l, link_ref ref, uint64_t now)
{
    own_tick(l, now);
    if (!session_accepts(&l->session) || !admits(l)) return OWN_STOPPED;
    own_link *p = link_at(l, ref);
    if (!p || (p->state != UP && p->state != READY) || !p->has_handle)
        return OWN_NO_PROOF;
    own_record *r = record(l, p->owner);
    if (!r || !r->lease || !r->acked || !r->barrier_done || !r->terminal_seen ||
        r->terminal_failed || r->proof_veto || l->ambiguities)
        return OWN_NO_PROOF;
    return OWN_OK;
}

own_result own_acl_prepare(own_acl_lane *lane, link_ref link, const uint8_t *packet,
                           size_t len, uint64_t now, own_acl_token *out)
{
    if (!lane || !out || !packet || len < 4 || len > OWN_ACL_MAX ||
        len != 4 + ((size_t)packet[2] | (size_t)packet[3] << 8))
        return OWN_INVALID;
    own_ledger *l = lane->ledger;
    own_result result = acl_allowed(l, link, now);
    if (result != OWN_OK) return result;
    own_link *p = link_at(l, link);
    unsigned handle = (packet[0] | (unsigned)packet[1] << 8) & 0x0fff;
    if (handle != p->handle) return OWN_NO_PROOF;
    if (lane->prepared || lane->submit_pending) return OWN_BUSY;
    if (lane->serial == UINT64_MAX) {
        own_detector_trip(l, now);
        return OWN_STOPPED;
    }
    lane->token = (own_acl_token){{lane->instance, ++lane->serial,
                                  l->session.attempt, l->epoch}};
    lane->epoch = l->epoch;
    lane->attempt = l->session.attempt;
    lane->link = link;
    lane->owner = p->owner;
    lane->len = (uint16_t)len;
    memcpy(lane->packet, packet, len);
    lane->prepared = 1;
    *out = lane->token;
    return OWN_OK;
}

own_result own_acl_commit(own_ledger *l, own_acl_lane *lane, const own_acl_token *token,
                          uint64_t now, own_acl_effect *out)
{
    if (!l || !lane || !token || !out || lane->ledger != l || !lane->prepared ||
        memcmp(token, &lane->token, sizeof *token)) return OWN_INVALID;
    own_result result = acl_allowed(l, lane->link, now);
    own_link *p = link_at(l, lane->link);
    if (result == OWN_OK &&
        (lane->epoch != l->epoch || lane->attempt != l->session.attempt ||
         !same_ref(lane->owner, p->owner))) result = OWN_INVALID;
    lane->prepared = 0;
    if (result == OWN_OK) {
        *out = (own_acl_effect){.link = lane->link, .handle = p->handle, .len = lane->len};
        memcpy(out->packet, lane->packet, lane->len);
        lane->submit_pending = 1; /* Consume before any external call. */
    }
    wipe(lane->packet, sizeof lane->packet);
    lane->len = 0;
    return result;
}

void own_acl_submitted(own_acl_lane *lane, const own_acl_token *token,
                       own_submit_result result, uint64_t now)
{
    if (!lane || !token || !lane->submit_pending ||
        memcmp(token, &lane->token, sizeof *token)) return;
    lane->submit_pending = 0;
    lane->result = result;
    if (result == OWN_SEND_UNCERTAIN) own_detector_trip(lane->ledger, now);
}

own_result own_commit_hci(own_ledger *l, const own_token *token, uint64_t now,
                          own_effect *out, uint64_t *ack_by)
{
    if (!l || !token || !out || !ack_by || token->_opaque[4] != OWN_EFFECT_HCI)
        return OWN_INVALID;
    own_result result = own_commit(l, token, now, out);
    if (result == OWN_OK) {
        own_record *r = record(l, out->owner);
        *ack_by = bounded_retry(l, r) && out->phase != OWN_UNDO ?
                  r->setup_by : r->phase_ack_by;
    }
    return result;
}

void own_observe_disconnect(own_ledger *l, uint16_t handle, uint64_t now)
{
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        own_link *p = &l->links[i];
        own_record *r = record(l, p->owner);
        if ((p->has_handle && p->handle == handle) ||
            (r && !r->undo_terminal && r->terminal_seen && !r->terminal_failed &&
             r->terminal.kind == OWN_TERM_CONNECTION && r->terminal.handle == handle))
            own_disconnected(l, p->ref, handle, now);
    }
}

void own_observe_connection(own_ledger *l, uint8_t le, uint8_t address_type,
                            const uint8_t address[6], uint16_t handle, uint8_t status,
                            uint64_t now)
{
    if (status || handle >= 0x0f00 || le > 1 ||
        (!le && address_type) || (le && address_type > 3)) return;
    /* Revoke by handle BEFORE expected-peer matching, including unpublished
     * terminals. This tombstone cannot be undone by a late ACK or fence. */
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_record *r = &l->records[i];
        if (r->state == OWN_FREE || commands[r->desc].class != CREATE ||
            !r->terminal_seen || r->terminal.status ||
            r->terminal.handle != handle || r->binding_revoked) continue;
        if (r->c.peer.le == le && r->c.peer.address_type == address_type &&
            !memcmp(r->c.peer.wire, address, 6)) continue;
        if (mutation("no_binding_revocation")) continue;
        r->binding_revoked = 1;
        r->lease = 0;
        r->revision++;
        own_link *p = link_at(l, r->c.link);
        if (p && same_ref(p->owner, r->ref)) {
            p->has_handle = 0;
            p->state = RETIRING;
            p->input_pending = 0;
            if (p->vstate == V_QUEUED) p->vstate = V_FREE;
            p->revision++;
        }
    }
    int seen = l->handles_seen[handle / 8] & (1u << (handle % 8));
    for (unsigned i = 0; i < OWN_LINK_MAX; i++) {
        own_link *p = &l->links[i];
        own_record *r = record(l, p->owner);
        if (!r || !r->sent || r->binding_revoked || commands[r->desc].class != CREATE ||
            !same_link(p->ref, r->c.link) || r->c.peer.le != le ||
            r->c.peer.address_type != address_type ||
            memcmp(r->c.peer.wire, address, 6)) continue;
        own_terminal_event terminal = {
            .kind = OWN_TERM_CONNECTION, .peer = r->c.peer, .handle = handle, .status = status
        };
        if (seen && !r->terminal_seen) {
            r->binding_revoked = 1;
        }
        terminal_received(l, r->ref, &terminal, now);
    }
    /* Observation, not publication, consumes H even if its Create later fails. */
    l->handles_seen[handle / 8] |= (uint8_t)(1u << (handle % 8));
}
