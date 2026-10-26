#include "host_int.h"
#include "attrib.h"
#include "probe.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const pad_output k_player_colours[HOST_MAX_PADS] = {
    { 0, 0, 0x00, 0x00, 0x40, 1 },      /* blue */
    { 0, 0, 0x40, 0x00, 0x00, 2 },      /* red */
    { 0, 0, 0x00, 0x40, 0x00, 3 },      /* green */
    { 0, 0, 0x20, 0x00, 0x20, 4 },      /* pink */
};
static unsigned closed_radio_left;


/* ---- helpers -------------------------------------------------------------- */

const char *addr_str(const unsigned char *a)
{
    static char buf[4][18];
    static int which;
    char *s = buf[which++ & 3];

    snprintf(s, 18, "**:**:**:**:%02X:%02X", a[1], a[0]);
    return s;
}

/* mask_conn is off by default: leaving Connection Request out is not proven
 * to hand the call to the system, and could keep a DualSense from reconnecting
 * (external audit, 9 Oct). It stays as a dialog-mode experiment. */
tune_t g_tune = { 1, PS_PAIR_INTERVAL, PS_PAIR_WINDOW, 0, 8, 0, HOST_EV_READS, 16 };

static void log_hex(const char *what, unsigned op, const unsigned char *p, int len)
{
    (void)p;
    /* Raw commands/events can contain keys, PINs and addresses. */
    log_line("%s %04x: len %d", what, op, len);
}

/* With fewer event reads kept while idle (dialog mode), a command opens them
 * all again for a while, so that its reply is read. */
static void reads_for(host_t *h, int n)
{
    if (n == h->reads_now || !h->hci.ops->event_reads) return;
    h->hci.ops->event_reads(h->hci.ctx, n);
    log_line("usb: %d event reads kept (%s)", n, n >= HOST_EV_READS ? "all" : "idle: the rest left to the system");
    h->reads_now = n;
}

static void pump(host_t *, int);

_Static_assert(WHATEVERPAD_ATTRIB_POLICY == OWN_POLICY_NO_RESTORE ||
               WHATEVERPAD_ATTRIB_POLICY == OWN_POLICY_SINGLE_PRODUCER,
               "unsupported attribution policy");

static unsigned long ambiguities(void)
{
    const attrib_counts *c = attrib_get();
    unsigned long n = c->overflow + c->timeout + c->untracked;
    for (unsigned i = ATTRIB_DUPLICATE; i < ATTRIB_CLASS_COUNT; i++) n += c->classes[i];
    return n;
}

static void attribution_check(host_t *h)
{
    unsigned long seen = ambiguities();
    if (seen != h->ambiguity_seen) {
        h->ambiguity_seen = seen;
        own_detector_trip(h->ownership, (uint64_t)now_ms());
    }
}

static int same_link(link_ref a, link_ref b)
{
    return a.attempt == b.attempt && a.generation == b.generation && a.slot == b.slot;
}

static int same_ref(own_ref a, own_ref b)
{
    return a.attempt == b.attempt && a.serial == b.serial;
}

int host_admits(const host_t *h)
{
    own_summary s;
    if (!h || h->dead) return 0;
    own_snapshot(h->ownership, &s);
    return !s.transport_closed && !s.runtime.cleanup_pending &&
           s.runtime.reason == SESSION_NONE &&
           (s.runtime.state == SESSION_STARTING || s.runtime.state == SESSION_ACTIVE);
}

void host_flush(host_t *h)
{
    own_token token;
    if (h->submitting || h->dead) return;
    if (own_next_hci(h->ownership, (uint64_t)now_ms(), &token) != OWN_OK) return;
    if (h->reads_now < HOST_EV_READS) {
        h->full_reads_until = now_ms() + 2000;
        reads_for(h, HOST_EV_READS);
    }
    h->submitting = 1;
    hci_submission submission = {.kind = HCI_SUBMIT_COMMAND, .token.command = &token};
    own_submit_result result = h->hci.ops->op_submit(h->hci.ctx, &submission);
    h->submitting = 0;
#if WHATEVERPAD_PROBE
    log_line("probe command_result attempts=%u submitted=%lu sent=%d mono_ms=%ld",
             h->probe_commands, attrib_get()->submitted, result == OWN_SENT, now_ms());
#endif
    if (result != OWN_SENT) {
        /* A backend refusal cannot be converted into another wire attempt. */
        own_fail(h->ownership, (uint64_t)now_ms(), 0);
        log_line("command submission unconfirmed; normal admission closed");
    }
}

static int command_prepare(host_t *h, link_t *l, unsigned op, const void *p, int len,
                           const void *original, int original_len, own_ref *out)
{
    if (!host_admits(h) || !h->hci.ops->op_submit || op > 0xffff ||
        len < 0 || len > 255 || (len && !p)) return 0;
#if WHATEVERPAD_PROBE
    if (op != OP_READ_BUFFER_SIZE || h->probe_commands) return 0;
    probe_deadline_ms = now_ms() + 1000;
#endif
    own_command c = {.opcode = (uint16_t)op, .plen = (uint8_t)len,
                     .ack_by = (uint64_t)now_ms() + (h->setup_retry ? 5000 : 2000),
                     .setup_retry = (uint8_t)h->setup_retry,
                     .terminal_by = (uint64_t)now_ms() + T_SETUP_TIMEOUT};
#if WHATEVERPAD_PROBE
    c.ack_by = (uint64_t)probe_deadline_ms;
#endif
    if (len) memcpy(c.params, p, (size_t)len);
    if (original && original_len == len) {
        c.original_known = 1;
        c.original_len = (uint8_t)original_len;
        memcpy(c.original, original, (size_t)original_len);
    }
    if (l) { c.link = l->ref; c.peer = l->peer; }
    unsigned slot;
    for (slot = 0; slot < OWN_MAX; slot++) {
        if (h->commands[slot].ref.serial) {
            own_state state;
            own_status(h->ownership, h->commands[slot].ref, &state);
            int retained = same_ref(h->commands[slot].ref, h->sync) ||
                           same_ref(h->commands[slot].ref, h->p256);
            for (unsigned i = 0; i < HOST_MAX_PADS; i++)
                retained |= h->links[i].used && !h->links[i].create.serial &&
                            same_ref(h->commands[slot].ref, h->links[i].scan_off);
            if (state == OWN_CLOSED &&
                !retained &&
                own_reclaim(h->ownership, h->commands[slot].ref) == OWN_OK)
                memset(&h->commands[slot], 0, sizeof h->commands[slot]);
        }
        if (!h->commands[slot].ref.serial) break;
    }
    if (slot == OWN_MAX) return 0;
    own_result prepared = own_prepare(h->ownership, &c, out);
    if (prepared == OWN_UNCHANGED) return 2;
    if (prepared != OWN_OK) return 0;
#if WHATEVERPAD_PROBE
    h->probe_commands++;
    log_line("probe submission opcode=0x%04x attempts=%u mono_ms=%ld deadline_ms=%ld",
             op, h->probe_commands, now_ms(), probe_deadline_ms);
#endif
    h->commands[slot].ref = *out;
    h->commands[slot].link = c.link;
    h->commands[slot].opcode = op;
    if (op == OP_INQUIRY) h->inquiry = *out;
    if (l && (op == OP_CREATE_CONNECTION || op == OP_ACCEPT_CONNECTION || op == 0x200d))
        l->create = *out;
    if (op == 0x2025) h->p256 = *out;
    if (op == 0x2026) h->dhkey = *out;
    if (!h->dispatching) host_flush(h);
    return host_admits(h);
}

int send_link_cmd(host_t *h, link_t *l, unsigned op, const void *p, int len)
{
    own_ref ref;
    if ((op == OP_CREATE_CONNECTION || op == OP_ACCEPT_CONNECTION || op == 0x200d) && h->le_ok) {
        if (!l) return 0;
        if (l->scan_off.serial) {
            own_state state = OWN_FREE;
            own_result result = own_status(h->ownership, l->scan_off, &state);
            if (result != OWN_OK) {
                own_fail(h->ownership, (uint64_t)now_ms(), 0);
                return 0;
            }
            if (state != OWN_ACKNOWLEDGED) return 0;
        } else {
            static const unsigned char off[2] = {0, 0};
            int prepared = command_prepare(h, NULL, 0x200c, off, 2, NULL, 0, &ref);
            if (!prepared) return 0;
            h->le_scan_want = 0;
            if (prepared == 1) { l->scan_off = ref; return 0; }
        }
    }
    int prepared = command_prepare(h, l, op, p, len, NULL, 0, &ref);
    if (prepared && l &&
        (op == OP_CREATE_CONNECTION || op == OP_ACCEPT_CONNECTION || op == 0x200d))
        l->scan_off = (own_ref){0};
    return prepared;
}

int send_cmd(host_t *h, unsigned op, const void *p, int len)
{
    link_t *l = NULL;
    const unsigned char *bytes = p;
    switch (op) {
    case OP_CREATE_CONNECTION: case OP_ACCEPT_CONNECTION: case OP_REJECT_CONNECTION:
    case OP_LINK_KEY_REPLY: case OP_LINK_KEY_NEG_REPLY: case OP_PIN_CODE_REPLY:
    case OP_IO_CAP_REPLY: case OP_USER_CONFIRM_REPLY:
        if (len < 6) return 0;
        l = link_by_peer(h, bytes, 0, 0);
        if (!l) return 0;
        break;
    case 0x200d:
        if (len != 25) return 0;
        l = link_by_peer(h, bytes + 6, 1, bytes[5]);
        if (!l || !l->le) return 0;
        break;
    case OP_AUTH_REQUESTED: case OP_SET_ENCRYPTION: case 0x2013: case 0x2019:
        if (len < 2) return 0;
        for (int i = 0; i < HOST_MAX_PADS; i++)
            if (h->links[i].used && h->links[i].connected &&
                h->links[i].handle == le16(bytes)) l = &h->links[i];
        if (!l) return 0;
        break;
    default: break; /* The engine's descriptor table is the final default-deny. */
    }
    return send_link_cmd(h, l, op, p, len);
}

void host_terminal(host_t *h, own_terminal_kind kind, link_t *l, unsigned handle, int status)
{
    own_terminal_event event = {.kind = kind, .handle = (uint16_t)handle,
                                .status = (uint8_t)status};
    if (l) event.peer = l->peer;
    for (unsigned i = 0; i < OWN_MAX; i++) {
        unsigned op = h->commands[i].opcode;
        if (!h->commands[i].ref.serial ||
            (l && !same_link(l->ref, h->commands[i].link))) continue;
        if ((kind == OWN_TERM_CONNECTION && (op == 0x0405 || op == 0x0409 || op == 0x200d)) ||
            (kind == OWN_TERM_INQUIRY && op == OP_INQUIRY) ||
            (kind == OWN_TERM_AUTH && op == OP_AUTH_REQUESTED) ||
            (kind == OWN_TERM_ENCRYPTION && (op == OP_SET_ENCRYPTION || op == 0x2019)) ||
            (kind == OWN_TERM_UPDATE && op == 0x2013) ||
            (kind == OWN_TERM_P256 && op == 0x2025) ||
            (kind == OWN_TERM_DHKEY && op == 0x2026))
            own_terminal(h->ownership, h->commands[i].ref, &event, (uint64_t)now_ms());
    }
}

link_t *host_command_link(host_t *h, own_ref ref)
{
    for (unsigned i = 0; i < OWN_MAX; i++) {
        link_ref link = h->commands[i].link;
        if (h->commands[i].ref.serial != ref.serial ||
            h->commands[i].ref.attempt != ref.attempt || !link.generation ||
            link.slot >= HOST_MAX_PADS) continue;
        link_t *l = &h->links[link.slot];
        if (l->used && same_link(l->ref, link) && own_link_current(h->ownership, link)) return l;
    }
    return NULL;
}

int slot_of(host_t *h, const link_t *l) { return (int)(l - h->links); }

link_t *link_by_peer(host_t *h, const unsigned char *a, int le, unsigned type)
{
    int i;

    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && h->links[i].peer.le == le &&
            h->links[i].peer.address_type == type &&
            memcmp(h->links[i].peer.wire, a, 6) == 0) return &h->links[i];
    return NULL;
}

int host_connection_observed(host_t *h, int le, unsigned type,
                             const unsigned char *address, unsigned handle)
{
    if (handle >= 0x0f00) {
        own_detector_trip(h->ownership, (uint64_t)now_ms());
        return 0;
    }
    for (unsigned i = 0; i < HOST_MAX_PADS; i++) {
        link_t *l = &h->links[i];
        if (l->used && l->connected && l->handle == handle &&
            (l->peer.le != le || l->peer.address_type != type ||
             memcmp(l->peer.wire, address, 6))) {
            own_detector_trip(h->ownership, (uint64_t)now_ms());
            return 0;
        }
    }
    return 1;
}

static link_t *link_by_handle(host_t *h, unsigned handle)
{
    int i;

    handle &= 0x0FFF;
    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && h->links[i].connected && h->links[i].handle == handle)
            return &h->links[i];
    return NULL;
}

static void chan_init(chan *c, unsigned psm, unsigned scid)
{
    memset(c, 0, sizeof *c);
    c->psm = psm;
    c->scid = scid;
}

link_t *link_new(host_t *h, const unsigned char *addr, int le, unsigned type)
{
    own_peer peer = {.le = (uint8_t)le, .address_type = (uint8_t)type};
    link_ref ref;
    memcpy(peer.adapter, h->bdaddr, 6);
    memcpy(peer.identity, addr, 6);
    memcpy(peer.wire, addr, 6);
    if (own_reserve_link(h->ownership, &peer, &ref) == OWN_OK) {
        link_t *l = &h->links[ref.slot];
        memset(l, 0, sizeof *l);
        l->ref = ref;
        l->peer = peer;
        l->used = 1;
        l->le = le;
        memcpy(l->addr, addr, 6);
        l->t_create = now_ms();
        chan_init(&l->sdp, PSM_SDP, CID_SDP);
        chan_init(&l->ctrl, PSM_HID_CTRL, CID_CTRL);
        chan_init(&l->intr, PSM_HID_INTR, CID_INTR);
        pad_state_reset(&l->st);
        l->out = k_player_colours[ref.slot];
        return l;
    }
    return NULL;
}

void link_free(host_t *h, link_t *l)
{
    link_ref ref = l->ref;
    int announced = l->announced;
    l->announced = 0;
    l->disconnecting = 1;
    if (announced && h->ev.on_disconnect) h->ev.on_disconnect(h->ev.ud, ref);
    if (!same_link(l->ref, ref)) return;
    if (!own_link_retained(h->ownership, ref) ||
        own_detach_link(h->ownership, ref) == OWN_OK) memset(l, 0, sizeof *l);
}

void link_disconnect(host_t *h, link_t *l, const char *why)
{
    if (!l->connected) {
        log_line("pad %s: dropped (%s)", addr_str(l->addr), why);
        link_free(h, l);
        return;
    }
    if (l->disconnecting) return;
    log_line("pad %s: disconnecting (%s)", addr_str(l->addr), why);
    l->disconnecting = 1;
    own_token token;
    own_result undo = own_request_undo(h->ownership, l->create, (uint64_t)now_ms(), &token);
    if (undo == OWN_OK) {
        hci_submission submission = {.kind = HCI_SUBMIT_COMMAND, .token.command = &token};
        h->hci.ops->op_submit(h->hci.ctx, &submission);
    } else {
        log_line("pad: radio link left connected; turn this controller off");
        link_free(h, l);
    }
}

/* ---- paired-pad store ----------------------------------------------------- */

db_rec *db_find(host_t *h, const unsigned char *addr)
{
    db_rec *r = bond_store_find(&h->store, 0, 0, addr);
    return r && r->complete && !r->repair_needed ? r : NULL;
}

bond_result db_save(host_t *h)
{
    bond_result result = h->store_error;
    if (result == BOND_COMMITTED) result = bond_store_save(h->db_path, &h->store);
    if (result == BOND_COMMITTED) {
        h->committed = h->store;
    } else {
        h->store = h->committed;
        if (result == BOND_UNCERTAIN) h->store_error = result;
        log_line("store: save failed: %s", bond_store_result_name(result));
    }
    return result;
}

db_rec *db_put(host_t *h, const unsigned char *addr, uint8_t le, uint8_t type)
{
    db_rec *d;
    db_rec empty = { .le = le, .addr_type = type };
    bond_result result;
    if (h->store_error != BOND_COMMITTED) return NULL;
    d = bond_store_find(&h->store, le, type, addr);
    if (!d) {
        memcpy(empty.addr, addr, 6);
        result = bond_store_put(&h->store, &empty);
        if (result != BOND_COMMITTED) {
            log_line("store: cannot remember pad: %s", bond_store_result_name(result));
            return NULL;
        }
        d = bond_store_find(&h->store, le, type, addr);
    }
    return d;
}

bond_result db_forget(host_t *h, const unsigned char *addr, uint8_t le, uint8_t type)
{
    db_rec *d = bond_store_find(&h->store, le, type, addr);
    if (!d) return BOND_MISSING;
    memmove(d, d + 1, sizeof *d * (size_t)(h->store.count - (d - h->store.records) - 1));
    h->store.count--;
    memset(&h->store.records[h->store.count], 0, sizeof *d);
    return db_save(h);
}

/* ---- ACL out ---------------------------------------------------------------- */

static void pool_expire(acl_pool *p)
{
    long now = now_ms();

    while (p->count && now - p->inflight[p->head] > INFLIGHT_MS) {
        p->head = (p->head + 1) % INFLIGHT_MAX;
        p->count--;
        if (p->credits < p->max) p->credits++;
    }
}

/* Packets the controller reports sent: their buffers are free again. */
static void pool_done(acl_pool *p, int n)
{
    int i;

    for (i = 0; i < n && p->count; i++) {
        p->head = (p->head + 1) % INFLIGHT_MAX;
        p->count--;
    }
    p->credits += n;
    if (p->credits > p->max) p->credits = p->max;
}

static acl_pool *pool_of(host_t *h, const link_t *l)
{
    return l->le && h->le_separate ? &h->lep : &h->br;
}

/* Sends one L2CAP frame, unfragmented. With no controller buffer free the
 * frame is dropped; everything sent here is retried or refreshed. */
int l2_send(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len)
{
    unsigned char pkt[HCI_PKT_MAX];
    acl_pool *pool = pool_of(h, l);
    unsigned mtu = l->le && h->le_separate ? h->le_mtu : h->acl_mtu;

    if (!l->connected) return 0;
    if (8 + len > (int)sizeof pkt || (mtu && 4 + len > (int)mtu)) {
        log_line("l2cap: %d-byte frame too large", len);
        return 0;
    }
    pool_expire(pool);
    if (pool->credits <= 0) {
        log_line("l2cap: no controller buffer free, frame dropped");
        return 0;
    }
    /* Start of a frame: auto-flushable on Classic, non-flushable on LE. */
    put16(pkt, (l->handle & 0x0FFF) | (l->le ? 0x0000 : 0x2000));
    put16(pkt + 2, (unsigned)(4 + len));
    put16(pkt + 4, (unsigned)len);
    put16(pkt + 6, cid);
    memcpy(pkt + 8, d, (size_t)len);
    own_acl_token token;
    if (own_acl_prepare(h->output, l->ref, pkt, (size_t)(8 + len),
                        (uint64_t)now_ms(), &token) != OWN_OK) return 0;
    hci_submission submission = {.kind = HCI_SUBMIT_ACL,
        .token.acl = {.lane = h->output, .token = &token}};
    if (h->hci.ops->op_submit(h->hci.ctx, &submission) != OWN_SENT) return 0;

    pool->credits--;
    if (pool->count == INFLIGHT_MAX) {
        pool->head = (pool->head + 1) % INFLIGHT_MAX;
        pool->count--;
    }
    pool->inflight[(pool->head + pool->count) % INFLIGHT_MAX] = now_ms();
    pool->count++;
    return 1;
}

static int sig_send(host_t *h, link_t *l, unsigned char code, unsigned char id,
                    const unsigned char *d, int len)
{
    unsigned char p[64];

    if (len > (int)sizeof p - 4) return 0;
    p[0] = code;
    p[1] = id;
    put16(p + 2, (unsigned)len);
    if (len) memcpy(p + 4, d, (size_t)len);
    return l2_send(h, l, CID_SIGNALING, p, 4 + len);
}

static unsigned char next_sig_id(host_t *h)
{
    if (++h->sig_id == 0) h->sig_id = 1;
    return h->sig_id;
}

/* ---- L2CAP channels ------------------------------------------------------- */

static chan *chan_by_scid(link_t *l, unsigned cid)
{
    if (l->sdp.st != CH_CLOSED && l->sdp.scid == cid) return &l->sdp;
    if (l->ctrl.st != CH_CLOSED && l->ctrl.scid == cid) return &l->ctrl;
    if (l->intr.st != CH_CLOSED && l->intr.scid == cid) return &l->intr;
    return NULL;
}

static chan *chan_by_psm(link_t *l, unsigned psm)
{
    if (psm == PSM_HID_CTRL) return &l->ctrl;
    if (psm == PSM_HID_INTR) return &l->intr;
    return NULL;            /* SDP is only ever opened by us */
}

static void chan_set(chan *c, chan_state st)
{
    if (st == CH_CLOSED || st == CH_CONNECTING) c->early_cfg = 0;
    c->st = st;
    c->t_state = now_ms();
    if (st != CH_OPEN) c->cfg_ours_ok = c->cfg_theirs_ok = 0;
    if (st == CH_CONFIG) c->cfg_tries = 0;
}

static void chan_send_cfg(host_t *h, link_t *l, chan *c)
{
    unsigned char r[4];

    put16(r, c->dcid);
    put16(r + 2, 0);                    /* flags; no options: defaults */
    sig_send(h, l, SIG_CFG_REQ, next_sig_id(h), r, 4);
    c->t_cfg = now_ms();
    c->cfg_tries++;
}

static void chan_check_open(link_t *l, chan *c)
{
    if (c->st == CH_CONFIG && c->cfg_ours_ok && c->cfg_theirs_ok) {
        chan_set(c, CH_OPEN);
        c->refused = 0;
        c->retry_at = 0;
        log_line("pad %s: channel %#x open", addr_str(l->addr), c->psm);
    }
}

static void chan_connect(host_t *h, link_t *l, chan *c)
{
    unsigned char r[4];

    if (now_ms() < c->retry_at) return;
    chan_set(c, CH_CONNECTING);
    put16(r, c->psm);
    put16(r + 2, c->scid);
    sig_send(h, l, SIG_CONN_REQ, next_sig_id(h), r, 4);
}

static void chan_close(host_t *h, link_t *l, chan *c)
{
    unsigned char r[4];

    if (c->st == CH_OPEN || c->st == CH_CONFIG) {
        put16(r, c->dcid);
        put16(r + 2, c->scid);
        sig_send(h, l, SIG_DISC_REQ, next_sig_id(h), r, 4);
    }
    chan_set(c, CH_CLOSED);
}

/* The pad closed a channel, or it never finished: without both HID
 * channels the pad is of no use. */
static void chan_lost(host_t *h, link_t *l, chan *c)
{
    chan_set(c, CH_CLOSED);
    if (c != &l->sdp && l->ready) link_disconnect(h, l, "HID channel closed");
}

static void chan_tick(host_t *h, link_t *l, chan *c, long now)
{
    if (c->st == CH_CONNECTING && now - c->t_state > T_CHAN_CONNECT) {
        log_line("pad %s: channel %#x: no connection response", addr_str(l->addr), c->psm);
        chan_set(c, CH_CLOSED);         /* opened again by the link's step */
    } else if (c->st == CH_CONFIG && now - c->t_state > T_CFG_TIMEOUT) {
        /* Only the pad can repeat its configuration request; if it went to
         * the system's driver, start the channel over. */
        log_line("pad %s: channel %#x: configuration stuck, reopening (ours %d theirs %d tries %d dcid %#x)", addr_str(l->addr), c->psm, c->cfg_ours_ok, c->cfg_theirs_ok, c->cfg_tries, c->dcid);
        chan_close(h, l, c);
    } else if (c->st == CH_CONFIG && !c->cfg_ours_ok && now - c->t_cfg >= T_CFG_RESEND) {
        if (c->cfg_tries >= CFG_TRIES) {
            log_line("pad %s: channel %#x: configuration never answered", addr_str(l->addr), c->psm);
            chan_close(h, l, c);
        } else {
            chan_send_cfg(h, l, c);
        }
    }
}

static void on_signaling(host_t *h, link_t *l, const unsigned char *d, int len)
{
    while (len >= 4) {
        unsigned char code = d[0], id = d[1], r[16];
        int clen = (int)le16(d + 2);
        const unsigned char *c = d + 4;
        chan *ch;

        if (4 + clen > len) break;

        switch (code) {
        case SIG_CONN_REQ:      /* psm, source cid */
            if (clen < 4) break;
            ch = chan_by_psm(l, le16(c));
            put16(r + 2, le16(c + 2));
            put16(r + 6, 0);
            if (!ch) {
                put16(r, 0);
                put16(r + 4, 0x0002);   /* PSM not supported */
                sig_send(h, l, SIG_CONN_RSP, id, r, 8);
                break;
            }
            /* The pad opens its own channel: take it, even over one of
             * ours still being set up. */
            ch->early_cfg = 0;
            chan_set(ch, CH_CONFIG);
            ch->dcid = le16(c + 2);
            put16(r, ch->scid);
            put16(r + 4, 0);            /* success */
            sig_send(h, l, SIG_CONN_RSP, id, r, 8);
            chan_send_cfg(h, l, ch);
            log_line("pad %s: accepted channel %#x", addr_str(l->addr), ch->psm);
            break;

        case SIG_CONN_RSP:      /* dcid, scid, result, status */
            if (clen < 8) break;
            ch = chan_by_scid(l, le16(c + 2));
            if (!ch || ch->st != CH_CONNECTING) break;
            if (le16(c + 4) == 1) break;            /* pending */
            if (le16(c + 4) != 0) {
                log_line("pad %s: channel %#x refused (%u)", addr_str(l->addr), ch->psm, le16(c + 4));
                long delay = 250L << (ch->refused < 4 ? ch->refused : 4);
                ch->refused++;
                chan_set(ch, CH_CLOSED);
                ch->retry_at = now_ms() + delay;
                /* "No resources" for the first channel (SDP) again and again on
                 * a link just made: the pad is busy with something of its own
                 * (the 8BitDo refused 8 times on its first link, twice on the
                 * console, with and without a call of its own turned down, and
                 * a new link worked at once both times). A fresh link is made
                 * instead of waiting out the eight refusals: at most
                 * FRESH_LINK_TRIES per search, then the refusals run out as before. */
                if (le16(c + 4) == 4 && l->pairing && ch->psm == 0x01 && ch->refused >= 3 &&
                    h->fresh_left > 0 && memcmp(h->last_call, l->addr, 6) == 0) {
                    h->fresh_left--;                    /* the search window is not stretched */
                    h->repage_at = now_ms() + T_FRESH_LINK;
                    link_disconnect(h, l, "no resources for a channel: a fresh link is made");
                    break;
                }
                if (ch->refused >= 8)
                    link_disconnect(h, l, "channel refused eight times");
                break;
            }
            ch->dcid = le16(c);
            chan_set(ch, CH_CONFIG);
            chan_send_cfg(h, l, ch);
            if (ch->early_cfg) {
                put16(r, ch->dcid);
                put16(r + 2, 0); put16(r + 4, 0);
                sig_send(h, l, SIG_CFG_RSP, ch->early_cfg_id, r, 6);
                ch->early_cfg = 0;
                ch->cfg_theirs_ok = 1;
                chan_check_open(l, ch);
                log_line("pad %s: early default config answered on PSM %#x", addr_str(l->addr), ch->psm);
            }
            break;

        case SIG_CFG_REQ:       /* dcid (ours), flags, options */
            if (clen < 4) break;
            ch = chan_by_scid(l, le16(c));
            if (!ch) break;
            if (ch->st == CH_CONNECTING) {
                /* Only the default, complete request needs no option parsing.
                 * Keep its identifier until our peer CID is known. Requests
                 * with options/continuation retain the previous retry path. */
                if (clen == 4 && le16(c + 2) == 0) {
                    ch->early_cfg_id = id;
                    ch->early_cfg = 1;
                }
                break;
            }
            put16(r, ch->dcid);
            put16(r + 2, 0);
            put16(r + 4, 0);            /* success, options as asked */
            sig_send(h, l, SIG_CFG_RSP, id, r, 6);
            ch->cfg_theirs_ok = 1;
            chan_check_open(l, ch);
            break;

        case SIG_CFG_RSP:       /* scid (ours), flags, result */
            if (clen < 6) break;
            ch = chan_by_scid(l, le16(c));
            if (!ch) break;
            if (le16(c + 4) == 0) {
                ch->cfg_ours_ok = 1;
                chan_check_open(l, ch);
            } else {
                log_line("pad %s: channel %#x configuration result %u",
                         addr_str(l->addr), ch->psm, le16(c + 4));
            }
            break;

        case SIG_DISC_REQ:      /* dcid (ours), scid */
            if (clen < 4) break;
            sig_send(h, l, SIG_DISC_RSP, id, c, 4);
            ch = chan_by_scid(l, le16(c));
            if (ch) chan_lost(h, l, ch);
            break;

        case SIG_DISC_RSP:
            break;

        case SIG_ECHO_REQ:
            sig_send(h, l, SIG_ECHO_RSP, id, c, clen < 32 ? clen : 32);
            break;

        case SIG_INFO_REQ:
            if (clen < 2) break;
            memset(r, 0, sizeof r);
            put16(r, le16(c));
            if (le16(c) == 2) {                 /* extended features: none */
                sig_send(h, l, SIG_INFO_RSP, id, r, 8);
            } else if (le16(c) == 3) {          /* fixed channels: signaling */
                r[4] = 0x02;
                sig_send(h, l, SIG_INFO_RSP, id, r, 12);
            } else {
                put16(r + 2, 1);                /* not supported */
                sig_send(h, l, SIG_INFO_RSP, id, r, 4);
            }
            break;

        case SIG_REJECT:
            log_line("pad %s: command rejected (reason %u)", addr_str(l->addr),
                     clen >= 2 ? le16(c) : 0);
            break;

        default:
            break;
        }
        d += 4 + clen;
        len -= 4 + clen;
    }
}

/* ---- SDP client: the Device ID record ------------------------------------- */

/* One data element header: sets type and content length, returns the
 * header length or 0 if it does not fit. */
static int de_header(const unsigned char *p, int avail, int *type, int *clen)
{
    static const int fixed[5] = { 1, 2, 4, 8, 16 };
    int size;

    if (avail < 1) return 0;
    *type = p[0] >> 3;
    size = p[0] & 7;
    if (*type == 0) { *clen = 0; return 1; }
    if (size < 5) { *clen = fixed[size]; return 1 + *clen <= avail ? 1 : 0; }
    if (size == 5) { if (avail < 2) return 0; *clen = p[1]; return 2 + *clen <= avail ? 2 : 0; }
    if (size == 6) { if (avail < 3) return 0; *clen = (int)be16(p + 1); return 3 + *clen <= avail ? 3 : 0; }
    if (avail < 5) return 0;
    {   /* 32-bit length: compared unsigned, as a huge one is no int */
        uint32_t big = be32(p + 1);
        if (big > (uint32_t)(avail - 5)) return 0;
        *clen = (int)big;
    }
    return 5;
}

/* Walks an attribute list depth-first, flattening the sequences: the
 * Device ID's vendor (0x0201) and product (0x0202) are each an attribute
 * id followed by a 16-bit value. Returns 1 if both were found. */
int sdp_find_ids(const unsigned char *p, int n, uint16_t *vid, uint16_t *pid)
{
    int i = 0, want = 0, got = 0;

    while (i < n) {
        int type, clen, hl = de_header(p + i, n - i, &type, &clen);

        if (!hl) return 0;
        if (type == 6 || type == 7) {   /* sequence: step inside */
            i += hl;
            continue;
        }
        if (type == 1 && clen == 2) {
            unsigned v = be16(p + i + hl);
            if (want == 1) { *vid = (uint16_t)v; got |= 1; want = 0; }
            else if (want == 2) { *pid = (uint16_t)v; got |= 2; want = 0; }
            else if (v == 0x0201) want = 1;
            else if (v == 0x0202) want = 2;
        } else {
            want = 0;
        }
        i += hl + clen;
    }
    return got == 3;
}

/* The HID descriptor list: {{0x22, descriptor bytes}}. Finds the bytes
 * that follow a one-byte 0x22 (report descriptor type). */
static int sdp_find_descriptor(const unsigned char *p, int n, const unsigned char **desc, int *dlen)
{
    int i = 0, after_type = 0;

    while (i < n) {
        int type, clen, hl = de_header(p + i, n - i, &type, &clen);

        if (!hl) return 0;
        if (type == 6 || type == 7) {
            i += hl;
            continue;
        }
        if (type == 4 && after_type) {          /* text string: the descriptor */
            *desc = p + i + hl;
            *dlen = clen;
            return 1;
        }
        after_type = type == 1 && clen == 1 && p[i + hl] == 0x22;
        i += hl + clen;
    }
    return 0;
}

static void sdp_send_request(host_t *h, link_t *l)
{
    /* Service Search Attribute requests, each answer up to 256 bytes:
     *   ids:  {UUID 0x1200 PnP Information}, attributes 0x0200-0x0205
     *   desc: {UUID 0x1124 HID}, attribute 0x0206 HIDDescriptorList */
    static const unsigned char ids[] = {
        0x35, 0x03, 0x19, 0x12, 0x00,  0x01, 0x00,
        0x35, 0x05, 0x0A, 0x02, 0x00, 0x02, 0x05,
    };
    static const unsigned char desc[] = {
        0x35, 0x03, 0x19, 0x11, 0x24,  0x01, 0x00,
        0x35, 0x03, 0x09, 0x02, 0x06,
    };
    const unsigned char *body = l->sdp_query == SDP_Q_IDS ? ids : desc;
    int blen = l->sdp_query == SDP_Q_IDS ? (int)sizeof ids : (int)sizeof desc;
    unsigned char req[64], two[24];
    unsigned char pdu = 0x06;           /* Service Search Attribute */
    int n = 5;

    if (l->sdp_two_step && !l->sdp_have_records) {
        /* Service Search: the same pattern, up to SDP_RECORDS handles */
        memcpy(two, body, 5);
        two[5] = 0x00;
        two[6] = SDP_RECORDS;
        body = two;
        blen = 7;
        pdu = 0x02;
    } else if (l->sdp_two_step) {
        /* Service Attribute: one record's handle, then the same byte limit
         * and attribute list as in the one-step request */
        uint32_t rec = l->sdp_rec[l->sdp_irec];
        two[0] = (unsigned char)(rec >> 24);
        two[1] = (unsigned char)(rec >> 16);
        two[2] = (unsigned char)(rec >> 8);
        two[3] = (unsigned char)rec;
        memcpy(two + 4, body + 5, (size_t)(blen - 5));
        body = two;
        blen = 4 + blen - 5;
        pdu = 0x04;
    }
    memcpy(req + n, body, (size_t)blen);
    n += blen;
    req[n++] = (unsigned char)l->sdp_cont_len;
    memcpy(req + n, l->sdp_cont, (size_t)l->sdp_cont_len);
    n += l->sdp_cont_len;

    l->sdp_tid = (l->sdp_tid + 1) & 0xFFFF;
    req[0] = pdu;
    req[1] = (unsigned char)(l->sdp_tid >> 8);
    req[2] = (unsigned char)l->sdp_tid;
    req[3] = (unsigned char)((n - 5) >> 8);
    req[4] = (unsigned char)(n - 5);
    l2_send(h, l, l->sdp.dcid, req, n);
    l->sdp_sent = 1;
    l->sdp_tries++;
    l->t_sdp = now_ms();
}

static void sdp_start(link_t *l, int query)
{
    l->sdp_query = query;
    l->sdp_sent = l->sdp_tries = 0;
    l->sdp_len = l->sdp_cont_len = 0;
    l->sdp_have_records = l->sdp_nrec = l->sdp_irec = 0;
}

/* A query is over: use what it brought, or what failing means. A pad that
 * did not answer the one-step query is asked again in two steps first. */
static int ids_from_name(const char *name, uint16_t *vid, uint16_t *pid);

static void sdp_finish(host_t *h, link_t *l, int ok)
{
    int query = l->sdp_query;

    if (!ok && !l->sdp_two_step && query) {
        l->sdp_two_step = 1;
        log_line("pad %s: no answer to the one-step SDP query; asking in two steps", addr_str(l->addr));
        sdp_start(l, query);
        return;
    }
    l->sdp_query = 0;
    if (query == SDP_Q_IDS) {
        uint16_t vid = 0, pid = 0;
        if (ok && sdp_find_ids(l->sdp_buf, l->sdp_len, &vid, &pid)) {
            db_rec *r = db_put(h, l->addr, 0, 0);
            if (r) {
                r->vid = vid;
                r->pid = pid;
                db_save(h);
            }
            log_line("pad %s: vendor %04x product %04x", addr_str(l->addr), vid, pid);
        } else {
            log_line("pad %s: no Device ID record, treating it as generic", addr_str(l->addr));
        }
        if (!vid && !pid && ids_from_name(l->name, &vid, &pid))
            log_line("pad %s: announced model read as %04x:%04x", addr_str(l->addr), vid, pid);
        l->vid = vid;
        l->pid = pid;
        l->ids_known = 1;
    } else if (query == SDP_Q_DESC) {
        const unsigned char *desc;
        int dlen;
        if (ok && sdp_find_descriptor(l->sdp_buf, l->sdp_len, &desc, &dlen) &&
            generic_setup(l->pctx.bytes, desc, dlen, l->vid, l->pid, h->map_dir)) {
            l->prof = &generic_profile;
        } else {
            link_disconnect(h, l, "no usable HID descriptor");
        }
    }
}

/* Two steps, first answer: the matching records' handles. */
static void sdp_on_records(host_t *h, link_t *l, const unsigned char *d, int len)
{
    int count, end, cl, i;

    if (len < 10) { sdp_finish(h, l, 0); return; }
    count = (int)be16(d + 7);
    end = 9 + 4 * count;
    if (end >= len) { sdp_finish(h, l, 0); return; }
    for (i = 0; i < count && l->sdp_nrec < SDP_RECORDS; i++)
        l->sdp_rec[l->sdp_nrec++] = be32(d + 9 + 4 * i);
    cl = d[end];
    if (cl > (int)sizeof l->sdp_cont || end + 1 + cl > len) { sdp_finish(h, l, 0); return; }
    if (cl && l->sdp_nrec < SDP_RECORDS) {          /* more handles to come */
        memcpy(l->sdp_cont, d + end + 1, (size_t)cl);
        l->sdp_cont_len = cl;
        l->sdp_tries = 0;
        sdp_send_request(h, l);
        return;
    }
    if (!l->sdp_nrec) {                             /* no such record: like an empty answer */
        sdp_finish(h, l, 1);
        return;
    }
    l->sdp_have_records = 1;
    l->sdp_irec = 0;
    l->sdp_cont_len = l->sdp_len = 0;
    l->sdp_tries = 0;
    sdp_send_request(h, l);
}

/* Two steps: one record read; was what we look for in it? */
static int sdp_found(link_t *l)
{
    uint16_t vid, pid;
    const unsigned char *desc;
    int dlen;

    if (l->sdp_query == SDP_Q_IDS) return sdp_find_ids(l->sdp_buf, l->sdp_len, &vid, &pid);
    return sdp_find_descriptor(l->sdp_buf, l->sdp_len, &desc, &dlen);
}

static void on_sdp(host_t *h, link_t *l, const unsigned char *d, int len)
{
    int bytes, cl;
    unsigned char want = !l->sdp_two_step ? 0x07 : l->sdp_have_records ? 0x05 : 0x03;

    if (!l->sdp_query || !l->sdp_sent || len < 5) return;
    if (be16(d + 1) != l->sdp_tid) return;          /* an answer to an older try */
    if (d[0] != want || len < 8) {
        sdp_finish(h, l, 0);
        return;
    }
    if (want == 0x03) {
        sdp_on_records(h, l, d, len);
        return;
    }
    bytes = (int)be16(d + 5);
    if (8 + bytes > len || l->sdp_len + bytes > (int)sizeof l->sdp_buf) {
        sdp_finish(h, l, 0);
        return;
    }
    memcpy(l->sdp_buf + l->sdp_len, d + 7, (size_t)bytes);
    l->sdp_len += bytes;
    cl = d[7 + bytes];
    if (cl > (int)sizeof l->sdp_cont || 8 + bytes + cl > len) {
        sdp_finish(h, l, 0);
        return;
    }
    if (cl) {                                       /* more to come: ask for it */
        memcpy(l->sdp_cont, d + 8 + bytes, (size_t)cl);
        l->sdp_cont_len = cl;
        l->sdp_tries = 0;
        sdp_send_request(h, l);
        return;
    }
    if (want == 0x05 && !sdp_found(l) && l->sdp_irec + 1 < l->sdp_nrec) {
        l->sdp_irec++;                              /* not in this record: the next one */
        l->sdp_cont_len = l->sdp_len = 0;
        l->sdp_tries = 0;
        sdp_send_request(h, l);
        return;
    }
    sdp_finish(h, l, 1);
}

/* Moves an SDP query along: channel, request, retries. */
static void sdp_step(host_t *h, link_t *l, long now)
{
    chan_tick(h, l, &l->sdp, now);
    if (l->sdp.st == CH_CLOSED) {
        chan_connect(h, l, &l->sdp);
    } else if (l->sdp.st == CH_OPEN && (!l->sdp_sent || now - l->t_sdp > T_SDP_RESEND)) {
        if (l->sdp_tries >= SDP_TRIES) {
            log_line("pad %s: SDP not answered", addr_str(l->addr));
            sdp_finish(h, l, 0);
        } else {
            sdp_send_request(h, l);
        }
    }
}

/* ---- HID ------------------------------------------------------------------ */

/* Sends an output report: buf[1..n] is the report, id first; buf[0] is
 * room for the Classic HID header. */
static int send_report(host_t *h, link_t *l, unsigned char *buf, int n)
{
    if (l->le) return le_send_report(h, l, buf + 1, n);
    buf[0] = HID_DATA_OUTPUT;
    return l2_send(h, l, l->intr.dcid, buf, n + 1);
}

static int send_output(host_t *h, link_t *l)
{
    unsigned char buf[128];
    int n;

    if (!l->ready || !l->prof->build_output) return 0;
    n = l->prof->build_output(l->pctx.bytes, &l->out, buf + 1, (int)sizeof buf - 1);
    if (n <= 0) return 0;
    l->t_out = now_ms();
    return send_report(h, l, buf, n);
}

/* Tells a Classic pad to forget this host (HID "virtual cable unplug"):
 * otherwise it keeps calling this controller every time it is switched on,
 * even after being forgotten here, and the system answers in our place. */
static void send_unplug(host_t *h, link_t *l)
{
    unsigned char b[1] = { HID_CONTROL_UNPLUG };

    if (l->le || !l->connected || l->ctrl.st != CH_OPEN) return;
    if (l2_send(h, l, l->ctrl.dcid, b, 1))
        log_line("pad %s: told to forget this host", addr_str(l->addr));
}

static void on_hid(host_t *h, link_t *l, const chan *c, const unsigned char *d, int len)
{
    if (len < 1) return;
    if (c == &l->ctrl) {
        if (d[0] == HID_CONTROL_UNPLUG) link_disconnect(h, l, "virtual cable unplugged");
        return;                         /* handshakes and feature data unused */
    }
    if (d[0] != HID_DATA_INPUT || len < 2) return;
    link_input(h, l, d + 1, len - 1);
}

static void announce(host_t *h, link_t *l, int report_id, int len);
static void note_report(link_t *l, int id);

/* One input report, id first, from either transport. */
void link_input(host_t *h, link_t *l, const unsigned char *rep, int len)
{
    int r;

    if (!l->prof || len < 1) return;
    note_report(l, rep[0]);
    r = l->prof->parse(l->pctx.bytes, rep, len, &l->st);
    if (r == PARSE_REINIT) {
        /* It dropped its report mode (a Wii pad after a status report):
         * the wake-up sequence is sent again at once. */
        l->full = 0;
        l->init_done = 0;
        l->init_step = 0;
        l->init_rounds = 0;
        l->t_init = now_ms() - T_INIT_STEP;
        return;
    }
    if (r == PARSE_FULL) l->full = 1;
    if (r == PARSE_NONE || own_input(h->ownership, l->ref, (uint16_t)l->handle,
                                   (uint64_t)now_ms()) != OWN_OK) return;
    link_ref ref = l->ref;
    if (r != PARSE_NONE && l->ready && !l->announced) announce(h, l, rep[0], len);
    if (host_admits(h) && own_link_current(h->ownership, ref) &&
        same_link(l->ref, ref) && l->ready && !l->disconnecting && h->ev.on_state)
        h->ev.on_state(h->ev.ud, ref, &l->st);
}

void link_ready(host_t *h, link_t *l)
{
    pad_info info;

    l->ready = 1;
    l->ssp_stage = 0;
    if (memcmp(h->last_call, l->addr, 6) == 0) {    /* the call worked: no extra tries left */
        h->repage_left = 0;
        h->repage_at = 0;
    }
    memset(&info, 0, sizeof info);
    info.vid = l->vid;
    info.pid = l->pid;
    info.profile = l->prof->name;
    memcpy(info.addr, l->addr, 6);
    (void)info;
    log_line("pad %s: ready as %s in slot %d", addr_str(l->addr), l->prof->name, slot_of(h, l));
}

/* The pad's virtual DualSense is created at its first input, not when its
 * link is ready: a pad that links but stays silent (the 8BitDo coming back
 * from sleep) would otherwise leave a virtual pad that never moves, and
 * the console's user screen would wait on it and take no other pad. */
static void announce(host_t *h, link_t *l, int report_id, int len)
{
    pad_info info;

    l->announced = 1;
    memset(&info, 0, sizeof info);
    info.vid = l->vid;
    info.pid = l->pid;
    info.profile = l->prof->name;
    memcpy(info.addr, l->addr, 6);
    log_line("pad %s: first input (report %#04x, %d bytes): virtual DualSense created", addr_str(l->addr), report_id, len);
    if (h->ev.on_connect) h->ev.on_connect(h->ev.ud, l->ref, &info);
}

/* Which reports a pad sends, for the log when it does not wake up. */
static void note_report(link_t *l, int id)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (l->seen_n[i] && l->seen_id[i] == id) { l->seen_n[i]++; return; }
        if (!l->seen_n[i]) { l->seen_id[i] = (unsigned char)id; l->seen_n[i] = 1; return; }
    }
}

/* Asks for a feature report on the control channel (HID GET_REPORT). Some
 * DualShock 4 clones stay in their short report 0x01 until their
 * calibration (0x05) is read, as the PS4 itself does when one connects. */
static void ask_feature(host_t *h, link_t *l, unsigned char id)
{
    unsigned char b[2] = { 0x43, 0 };   /* GET_REPORT, feature */

    if (l->le || !l->connected || l->ctrl.st != CH_OPEN) return;
    b[1] = id;
    if (l2_send(h, l, l->ctrl.dcid, b, 2) && l->init_rounds == 1)
        log_line("pad %s: still in its short report; asking for its feature report %#04x", addr_str(l->addr), id);
}

/* Sends the pad's wake-up sequence one report at a time, and repeats it
 * while the pad has not switched to its full report. */
void init_tick(host_t *h, link_t *l, long now)
{
    unsigned char buf[128];
    int n;

    if (l->init_done || !l->prof->init_report) return;
    if (l->init_step < 0) {                     /* between rounds */
        if (l->full) l->init_done = 1;
        else if (now - l->t_init >= (l->init_rounds >= INIT_ROUNDS ? T_INIT_SLOW : T_INIT_RETRY)) {
            l->init_step = 0;
            if (l->prof->wake_feature) ask_feature(h, l, l->prof->wake_feature);
        }
        return;
    }
    if (now - l->t_init < T_INIT_STEP) return;

    n = l->prof->init_report(l->pctx.bytes, l->init_step, &l->out, buf + 1, (int)sizeof buf - 1);
    if (n > 0) {
        send_report(h, l, buf, n);
        l->init_step++;
        l->t_init = now;
        return;
    }
    l->init_rounds++;
    if (!l->prof->needs_full || l->full) {
        l->init_done = 1;
    } else {
        /* A pad left in its simple report sends buttons and sticks in another
         * layout (it "goes mad" until it switches): never give up, keep
         * asking every few seconds once the quick rounds are spent. */
        if (l->init_rounds == INIT_ROUNDS) {
            char seen[64];
            int i, pos = 0;
            seen[0] = 0;
            for (i = 0; i < 4 && l->seen_n[i]; i++)
                pos += snprintf(seen + pos, sizeof seen - (size_t)pos, " %#04x x%d", l->seen_id[i], l->seen_n[i]);
            log_line("pad %s: not in its full report yet; asking again every %d s (reports seen:%s)",
                     addr_str(l->addr), T_INIT_SLOW / 1000, pos ? seen : " none");
        }
        l->init_step = -1;
        l->t_init = now;
    }
}

/* ---- inbound ACL ---------------------------------------------------------- */

static void on_frame(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len)
{
    chan *c;

    if (l->le) {
        le_on_frame(h, l, cid, d, len);
        return;
    }
    if (cid == CID_SIGNALING) {
        on_signaling(h, l, d, len);
        return;
    }
    c = chan_by_scid(l, cid);
    if (!c) return;
    if (c == &l->sdp) on_sdp(h, l, d, len);
    else on_hid(h, l, c, d, len);
}

static void on_acl(host_t *h, const unsigned char *pkt, int len)
{
    unsigned hdr;
    int pb, dlen;
    link_t *l;

    if (len < 4) return;
    hdr = le16(pkt);
    pb = (int)(hdr >> 12) & 3;
    dlen = (int)le16(pkt + 2);
    l = link_by_handle(h, hdr);
    if (!l || l->disconnecting || 4 + dlen > len) return;

    if (pb == 1) {                      /* continuation */
        if (!l->rx_need || l->rx_len + dlen > (int)sizeof l->rx) {
            l->rx_need = 0;
            return;
        }
        memcpy(l->rx + l->rx_len, pkt + 4, (size_t)dlen);
        l->rx_len += dlen;
    } else {                            /* start: a lost tail is abandoned */
        if (dlen < 4 || dlen > (int)sizeof l->rx) return;
        memcpy(l->rx, pkt + 4, (size_t)dlen);
        l->rx_len = dlen;
        l->rx_need = 4 + (int)le16(pkt + 4);
    }
    if (l->rx_need && l->rx_len >= l->rx_need) {
        int need = l->rx_need;
        l->rx_need = 0;
        on_frame(h, l, le16(l->rx + 2), l->rx + 4, need - 4);
    }
}

/* ---- HCI events ----------------------------------------------------------- */

static int is_banned(const host_t *h, const unsigned char *addr);

static void send_io_cap_reply(host_t *h, const unsigned char *addr)
{
    unsigned char p[9];

    memcpy(p, addr, 6);
    p[6] = 0x03;                        /* NoInputNoOutput */
    p[7] = 0x00;                        /* no OOB data */
    p[8] = 0x04;                        /* general bonding, no MITM */
    send_cmd(h, OP_IO_CAP_REPLY, p, 9);
}

static void pair_with(host_t *h, const unsigned char *rec)
{
    unsigned char p[13];
    link_t *l;

    if (h->discovery_blocked || is_banned(h, rec) || link_by_peer(h, rec, 0, 0) ||
        (h->collision_blocked && memcmp(h->collision_addr, rec, 6) == 0)) return;
    if (db_find(h, rec)) {
        log_line("pad %s: in pairing mode again, forgetting its old key", addr_str(rec));
        db_forget(h, rec, 0, 0);
    }
    l = link_new(h, rec, 0, 0);
    if (!l) {
        log_line("pairing: no free slot");
        return;
    }
    l->pairing = 1;
    l->cod = (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16;
    memcpy(h->last_call, rec, sizeof h->last_call);
    if (!h->in_repage) h->repage_left = REPAGE_TRIES;      /* a new call gets its extra tries */
    h->inquiring = 0;

    memcpy(p, rec, 6);
    put16(p + 6, 0xCC18);               /* DM1..DH5 */
    p[8] = rec[6];                      /* page scan repetition mode */
    p[9] = 0;
    put16(p + 10, le16(rec + 11) | 0x8000);   /* clock offset, valid */
    p[12] = 1;                          /* allow role switch */
    l->connect_opcode = OP_CREATE_CONNECTION;
    memcpy(l->connect_params, p, sizeof p);
    send_cmd(h, OP_CREATE_CONNECTION, p, 13);
    log_line("pad %s: call data: page-scan mode %u, clock offset %04x", addr_str(rec),
             (unsigned)rec[6], (unsigned)le16(rec + 11));
    log_line("pad %s: connecting to pair", addr_str(rec));
}

/* ---- banned addresses ---------------------------------------------------------
 *
 * <data dir>/ban.txt lists Bluetooth addresses (one per line, "AA:BB:CC:DD:EE:FF"
 * or "AABBCCDDEEFF", '#' starts a comment) that the search must never look at:
 * the console's own DualSense controllers. A banned address is skipped as soon
 * as the search reports it, before anything else is done with it. With no
 * ban.txt nothing changes. */

static int is_banned(const host_t *h, const unsigned char *addr)
{
    int i;

    for (i = 0; i < h->n_ban; i++)
        if (memcmp(h->ban[i], addr, 6) == 0) return 1;
    return 0;
}

static void ban_load(host_t *h)
{
    char line[96];
    FILE *f = fopen(h->ban_path, "r");

    if (!f) return;
    while (h->n_ban < 16 && fgets(line, sizeof line, f)) {
        unsigned v[6];
        char *hash = strchr(line, '#');
        int i;

        if (hash) *hash = '\0';
        if (sscanf(line, " %2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
            sscanf(line, " %2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
            continue;
        for (i = 0; i < 6; i++) h->ban[h->n_ban][5 - i] = (unsigned char)v[i];     /* shown MSB first */
        h->n_ban++;
    }
    fclose(f);
    log_line("ban list: %d address(es) the search will never look at", h->n_ban);
}

/* An inquiry record: addr 6, page scan mode 1, reserved 1, class 3,
 * clock offset 2, RSSI 1. */
/* What a pad in range says about itself before any connection (its
 * extended inquiry response): name, services, maybe its model. Logged
 * during a search, for pads of the game pad class, to learn which models
 * can be told apart without connecting. */
static void log_eir(host_t *h, const unsigned char *rec, const unsigned char *eir, int n)
{
    uint32_t cod = (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16;
    char out[200];
    int pos = 0, i;

    if (now_ms() >= h->pair_until || !profile_class_is_gamepad(cod)) return;
    for (i = 0; i + 1 < n && eir[i] > 0 && i + 1 + eir[i] <= n; i += 1 + eir[i]) {
        int type = eir[i + 1], dlen = eir[i] - 1;
        if (pos > (int)sizeof out - 40) break;
        pos += snprintf(out + pos, sizeof out - (size_t)pos, " %02x:len=%d", type, dlen);
    }
    log_line("inquiry: %s class %06x eir%s", addr_str(rec), (unsigned)cod, pos ? out : " (empty)");
}

/* The model a pad announces about itself in its extended inquiry response
 * (the Device ID record, type 0x10: source, vendor, product, version).
 * Returns 1 and the ids when it is there. */
static int eir_device_id(const unsigned char *eir, int n, unsigned *vid, unsigned *pid)
{
    int i;

    if (!eir) return 0;
    for (i = 0; i + 1 < n && eir[i] > 0 && i + 1 + eir[i] <= n; i += 1 + eir[i]) {
        if (eir[i + 1] == 0x10 && eir[i] >= 9) {
            *vid = le16(eir + i + 4);
            *pid = le16(eir + i + 6);
            return 1;
        }
    }
    return 0;
}

/* Sony's own pads for the console (DualSense and DualSense Edge): the
 * system pairs and keeps them, and a second pairing with another host breaks
 * them. They are never touched, on any console. */
static int model_is_dualsense(unsigned vid, unsigned pid)
{
    return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2);
}

/* Whether an extended answer carries at least one field. */
/* The pad's name from its search answer (complete or short name), "" if none. */
static void eir_name(const unsigned char *eir, int n, char *out, int max)
{
    int i;

    out[0] = 0;
    if (!eir) return;
    for (i = 0; i + 1 < n && eir[i] > 0 && i + 1 + eir[i] <= n; i += 1 + eir[i]) {
        int type = eir[i + 1], len = eir[i] - 1;
        if ((type == 0x09 || type == 0x08) && len > 0) {
            if (len > max - 1) len = max - 1;
            memcpy(out, eir + i + 2, (size_t)len);
            out[len] = 0;
            if (type == 0x09) return;   /* the complete name wins */
        }
    }
}

/* Which PIN a pad asking for one gets: our address for Nintendo's Wii
 * pads (named "Nintendo RVL-...", or with the Wii Remote's class, or that
 * already refused "0000" on an earlier pairing), "0000" for the rest. */
static int pin_is_address(const host_t *h, const link_t *l)
{
    if (strncmp(l->name, "Nintendo RVL-", 13) == 0) return 1;
    if (l->cod == 0x002504) return 1;               /* peripheral, joystick: the Wii Remote */
    return h->pin_addr_set && memcmp(h->pin_addr, l->addr, 6) == 0;
}

/* Pads that give no vendor and product (some clones answer the Device ID
 * question empty) but say what they are in their name: read as the pad
 * they copy. */
static int ids_from_name(const char *name, uint16_t *vid, uint16_t *pid)
{
    static const struct { const char *name; uint16_t vid, pid; } k[] = {
        { "Wireless Controller",      0x054C, 0x05C4 },     /* DualShock 4 and its clones */
        { "Pro Controller",           0x057E, 0x2009 },     /* Switch Pro and pads emulating it */
        { "Xbox Wireless Controller", 0x045E, 0x02E0 },     /* Xbox One S, and clones in Xbox mode */
    };
    size_t i;

    for (i = 0; name[0] && i < sizeof k / sizeof k[0]; i++)
        if (strcmp(name, k[i].name) == 0) {
            *vid = k[i].vid;
            *pid = k[i].pid;
            return 1;
        }
    return 0;
}

static int eir_has_fields(const unsigned char *eir, int n)
{
    return n >= 2 && eir[0] > 0 && 1 + eir[0] <= n;
}

static void on_found(host_t *h, const unsigned char *rec, const unsigned char *eir, int eir_len)
{
    uint32_t cod = (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16;
    unsigned vid, pid;

    if (now_ms() >= h->pair_until) return;
    if (h->discovery_blocked || is_banned(h, rec)) return;          /* banned: not even looked at */
    if (!profile_class_is_gamepad(cod)) {
        log_line("inquiry: %s class %06x is not a gamepad, ignored", addr_str(rec), (unsigned)cod);
        return;
    }
    if (eir_device_id(eir, eir_len, &vid, &pid) && model_is_dualsense(vid, pid)) {
        static unsigned char last[6];
        static long last_at;
        if (memcmp(last, rec, 6) != 0 || now_ms() - last_at > 30000) {
            memcpy(last, rec, 6);
            last_at = now_ms();
            log_line("inquiry: %s is a DualSense (%04x:%04x), left alone", addr_str(rec), vid, pid);
        }
        /* Remember the identity for this host session, including later replies
         * without EIR. Never persist this learned list to ban.txt. */
        if (h->n_ban < 16) memcpy(h->ban[h->n_ban++], rec, 6);
        else {
            h->discovery_blocked = 1;
            log_line("inquiry: exclusion list full; new Classic pairing blocked for this session");
        }
        /* It may have answered cut short before (no data, so no model then):
         * that pending call is cancelled, never made. */
        if (h->defer_at && memcmp(h->defer_rec, rec, 6) == 0) {
            h->defer_at = 0;
            log_line("inquiry: pending call to %s cancelled: it is a DualSense", addr_str(rec));
        }
        return;
    }
    /* Empty EIR is not proof of USB truncation. Keep the existing bounded
     * wait heuristic, but validate framing separately before reaching here. */
    if (eir && !eir_has_fields(eir, eir_len)) {
        if (!h->defer_at) {
            memcpy(h->defer_rec, rec, sizeof h->defer_rec);
            h->defer_at = now_ms();
            log_line("inquiry: %s answered without data; waiting up to %d s for a complete answer before calling it",
                     addr_str(rec), T_EIR_WAIT / 1000);
        }
        return;
    }
    if (h->defer_at && memcmp(h->defer_rec, rec, 6) == 0) h->defer_at = 0;
    log_line("found gamepad %s (class %06x) after %ld ms of search", addr_str(rec), (unsigned)cod,
             now_ms() - h->search_started);
    pair_with(h, rec);
    {
        link_t *nl = link_by_peer(h, rec, 0, 0);
        if (nl) eir_name(eir, eir_len, nl->name, (int)sizeof nl->name);
    }
}

/* An unknown existing handle may belong to the system. Never sweep handles
 * to recover a refused connection: there is no proof of ownership here. */
static void report_existing_link(host_t *h)
{
    if (h->purge_at && now_ms() - h->purge_at < 60000) return;
    h->purge_at = now_ms();
    log_line("connection already exists: preserving unknown/system links; stop pairing and reconnect the target pad");
}

/* Diagnostics only: while a pad is linking (connected or being paged, not yet
 * ready), the events that matter for connecting and pairing are written to the
 * log, a limited number of them per run, to see where a pairing stalls. */
static void trace_linking(host_t *h, const unsigned char *ev, int len)
{
    static int budget = 300;
    int i, linking = 0;

    if (budget <= 0) return;
    switch (ev[0]) {
    case 0x03: case 0x05: case 0x06: case 0x08: case 0x0E: case 0x0F: case 0x17:
    case 0x18: case 0x31: case 0x32: case 0x33: case 0x36: break;
    default: return;
    }
    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && !h->links[i].ready) linking = 1;   /* pairing or reconnecting */
    if (!linking) return;
    budget--;
    log_line("link event: type %02x, len %d", ev[0], len);
}

/* Validate only discovery events. A plausible header is not proof of
 * framing, so never scan payload bytes for a guessed next HCI event. */
static int inquiry_record_valid(const unsigned char *rec)
{
    int i, any = 0, all_ff = 1;
    for (i = 0; i < 6; i++) {
        any |= rec[i];
        if (rec[i] != 0xff) all_ff = 0;
    }
    return any && !all_ff && rec[6] <= 2;
}

static int inquiry_valid(const unsigned char *ev, int len)
{
    int i, n;
    if (len < 3 || len != 2 + ev[1]) return 0;
    n = ev[2];
    if (ev[0] == 0x2f) {
        if (len != 257 || n != 1 || !inquiry_record_valid(ev + 3)) return 0;
        for (i = 17; i < len && ev[i]; i += 1 + ev[i])
            if (i + 1 + ev[i] > len) return 0;
        return 1;
    }
    if (n < 1 || len != 3 + 14 * n) return 0;
    for (i = 0; i < n; i++)
        if (!inquiry_record_valid(ev + 3 + 14 * i)) return 0;
    return 1;
}

static void on_event(host_t *h, const unsigned char *ev, int len)
{
    unsigned char p[32];
    link_t *l;
    int i;

    if (len < 2) return;
    if (!host_admits(h) && ev[0] != 0x0e && ev[0] != 0x0f &&
        ev[0] != 0x01 && ev[0] != 0x03 && ev[0] != 0x05 &&
        !(ev[0] == 0x3e && len >= 3 && (ev[2] == 1 || ev[2] == 0x0a))) return;
    if (g_tune.trace && ev[0] != 0x13) {
        log_hex("trace ev", ev[0], ev, len);
    }
    if ((ev[0] == 0x02 || ev[0] == 0x22 || ev[0] == 0x2f) && !inquiry_valid(ev, len)) {
        h->bad_inquiry++;
        if (h->bad_inquiry <= 5 || (h->bad_inquiry & (h->bad_inquiry - 1)) == 0)
            log_line("inquiry: malformed result ignored (event %#x, bytes %d, total %u)",
                     ev[0], len, h->bad_inquiry);
        return;
    }
    trace_linking(h, ev, len);
#if WHATEVERPAD_PROBE
    /* Observe metadata without answering requests or entering pairing protocols. */
    if (ev[0] != 0x0e && ev[0] != 0x0f) return;
#endif
    switch (ev[0]) {
    case 0x01:  /* Inquiry Complete */
        if (len != 3) break;            /* a damaged event: not an end of inquiry */
        log_line("inquiry: finished (status %#04x)", ev[2]);
        host_terminal(h, OWN_TERM_INQUIRY, NULL, 0, ev[2]);
        /* Something is always in range (at home, a TV at least): a whole
         * round with nothing heard means the radio is busy elsewhere. The
         * page scan is looked at again at once instead of in a few seconds. */
        if (h->inquiring && ev[2] == 0 && h->round_heard == 0 && now_ms() < h->pair_until) {
            log_line("inquiry: nothing at all heard in a whole round: radio busy, or nothing in range (page scan checked again)");
            h->ps_check_at = 0;
        }
        h->inquiring = 0;
        break;

    case 0x02:  /* Inquiry Result: addr, scan mode, 2 reserved, class, clock */
        h->round_heard++;
        for (i = 0; i < ev[2] && 3 + (i + 1) * 14 <= len; i++) {
            const unsigned char *r = ev + 3 + i * 14;
            unsigned char rec[14];
            memcpy(rec, r, 7);          /* to the RSSI layout on_found reads */
            rec[7] = 0;
            memcpy(rec + 8, r + 9, 5);
            rec[13] = 0;
            on_found(h, rec, NULL, 0);
        }
        break;

    case 0x22:  /* Inquiry Result with RSSI */
        h->round_heard++;
        for (i = 0; i < ev[2] && 3 + (i + 1) * 14 <= len; i++) on_found(h, ev + 3 + i * 14, NULL, 0);
        break;

    case 0x2F:  /* Extended Inquiry Result */
        h->round_heard++;
        if (len >= 17) {
            log_eir(h, ev + 3, ev + 17, len - 17);
            on_found(h, ev + 3, ev + 17, len - 17);
        }
        break;

    case 0x04:  /* Connection Request: addr, class, link type */
        if (len < 12 || ev[11] != 1) break;
        {
            db_rec *r = db_find(h, ev + 2);
            if (!r && !link_by_peer(h, ev + 2, 0, 0)) {
                /* Somebody that is not a paired pad is paging this controller:
                 * usually a DualSense reconnecting to the console, which uses
                 * this controller for that. It is the system's to answer, so it
                 * is left alone (turning it down left the DualSense blinking);
                 * only noted, once in a while. */
                static unsigned char last[6];
                static long last_at;
                if (memcmp(last, ev + 2, 6) != 0 || now_ms() - last_at > 30000) {
                    memcpy(last, ev + 2, 6);
                    last_at = now_ms();
                    log_line("incoming connection from %s (class %02x%02x%02x): not ours, left to the system",
                             addr_str(ev + 2), ev[10], ev[9], ev[8]);
                }
                /* This event came to us, so the system never saw the call:
                 * stand aside for a while, for the system to get the pad's
                 * next attempt (about 22 s later). Not while one of our pads
                 * is being linked; once the pause ends, the next call starts another. */
                /* Never while searching: the pause would blind the search for
                 * the pad being paired. */
                if (h->hci.ops->yield && now_ms() >= h->pair_until && !h->pair_pending_s &&
                    now_ms() - h->yield_at > T_YIELD) {
                    int k, linking = 0;
                    for (k = 0; k < HOST_MAX_PADS; k++)
                        if (h->links[k].used && !h->links[k].ready) linking = 1;
                    if (!linking) {
                        h->yield_at = now_ms();
                        h->hci.ops->yield(h->hci.ctx, T_YIELD);
                    }
                }
                for (i = 0; i < HOST_MAX_PADS; i++)
                    if (h->links[i].used) {
                        own_release(h->ownership, SESSION_NATIVE, (uint64_t)now_ms());
                        break;
                    }
                break;
            }
            if (!r || r->le) break;                                 /* not ours */
            l = link_by_peer(h, ev + 2, 0, 0);
            if (l) {
                /* The controller lets no one call over a link it still has:
                 * a paired pad calling while its link is open here means that
                 * link is gone and its end never reached us (some pads drop
                 * without a word, or the event went to the system). The old
                 * one is let go and the call taken; it would otherwise wait
                 * unanswered and the pad stay dead until switched off. */
                if (!l->connected || !l->ready || l->disconnecting) break;   /* still being set up: not this */
                log_line("pad: a new request arrived without the old link's terminal; radio link left connected, turn this controller off");
                own_detector_trip(h->ownership, (uint64_t)now_ms());
                link_free(h, l);
                break;
            }
        }
        l = link_new(h, ev + 2, 0, 0);
        memcpy(p, ev + 2, 6);
        if (!l) {
            log_line("incoming owned peer: no free ledger slot; request left unanswered");
            break;
        }
        l->cod = (uint32_t)ev[8] | (uint32_t)ev[9] << 8 | (uint32_t)ev[10] << 16;
        p[6] = 0x00;                    /* become central, as a host does */
        l->connect_opcode = OP_ACCEPT_CONNECTION;
        memcpy(l->connect_params, p, 7);
        send_cmd(h, OP_ACCEPT_CONNECTION, p, 7);
        log_line("pad %s: reconnecting", addr_str(ev + 2));
        break;

    case 0x03:  /* Connection Complete: status, handle, addr, type, enc */
        if (len != 13 || ev[11] != 1) break;
        if (!ev[2] && !host_connection_observed(h, 0, 0, ev + 5, le16(ev + 3))) break;
        if (!(l = link_by_peer(h, ev + 5, 0, 0)) || l->connected) break;
        host_terminal(h, OWN_TERM_CONNECTION, l, le16(ev + 3), ev[2]);
        if (ev[2] != 0) {
            log_line("pad %s: connection failed (status %#04x)", addr_str(l->addr), ev[2]);
            /* A pad found in this search that did not answer the page is not
             * given up on: it is called again, up to REPAGE_TRIES more times,
             * and the search is held open meanwhile. Found late and called
             * once, it would otherwise be lost for a whole new search. */
            if (ev[2] == 0x04 && l->pairing && !l->le && h->repage_left > 0 &&
                memcmp(h->last_call, l->addr, 6) == 0) {
                long hold = now_ms() + T_REPAGE_WAIT + T_CONN_TIMEOUT + 5000;
                h->repage_left--;
                h->repage_at = now_ms() + T_REPAGE_WAIT;
                if (h->pair_until < hold) h->pair_until = hold;
                log_line("pad %s: no answer to the call; calling it again (%d more after this)",
                         addr_str(l->addr), h->repage_left);
            }
            link_free(h, l);
            break;
        }
        if (!own_link_current(h->ownership, l->ref)) break;
        l->connected = 1;
        l->handle = le16(ev + 3) & 0x0FFF;
        l->t_conn = now_ms();
        log_line("pad %s: connected, handle %#05x", addr_str(l->addr), l->handle);
        break;

    case 0x05:  /* Disconnection Complete: status, handle, reason */
        if (len < 6 || ev[2] || !(l = link_by_handle(h, le16(ev + 3)))) break;
        own_disconnected(h->ownership, l->ref, (uint16_t)l->handle, (uint64_t)now_ms());
        l->connected = 0;
        log_line("pad %s: disconnected (reason %#04x)", addr_str(l->addr), ev[5]);
        link_free(h, l);
        break;

    case 0x06:  /* Authentication Complete: status, handle */
        if (len < 5 || !(l = link_by_handle(h, le16(ev + 3)))) break;
        host_terminal(h, OWN_TERM_AUTH, l, l->handle, ev[2]);
        l->ssp_stage = 0;
        if (ev[2] == 0) {
            l->auth_ok = 1;
            l->t_auth_ok = now_ms();
            break;
        }
        log_line("pad %s: authentication failed (status %#04x)", addr_str(l->addr), ev[2]);
        if (l->pin_zero) {
            /* Keep the PIN intent, but require a new manual pairing: setup
             * failure has no accepted input authorizing radio Disconnect. */
            memcpy(h->pin_addr, l->addr, 6);
            h->pin_addr_set = 1;
            host_pair(h, 0);
            log_line("pad %s: PIN 0000 refused; turn this controller off and pair it again to use this console's address", addr_str(l->addr));
        }
        if (!l->pairing) db_forget(h, l->addr, 0, 0);     /* the pad lost its key */
        link_disconnect(h, l, "authentication failed");
        break;

    case 0x08:  /* Encryption Change: status, handle, enabled */
        if (len < 6 || !(l = link_by_handle(h, le16(ev + 3)))) break;
        host_terminal(h, OWN_TERM_ENCRYPTION, l, l->handle, ev[2]);
        if (l->le) {
            le_on_encryption(h, l, ev[2], ev[5]);
            break;
        }
        if (ev[2] == 0 && ev[5]) {
            l->enc_on = 1;
            l->auth_ok = 1;             /* encryption implies it */
        }
        break;

    case 0x30:  /* Encryption Key Refresh Complete: status, handle */
        if (len < 5 || !(l = link_by_handle(h, le16(ev + 3))) || !l->le) break;
        le_on_encryption(h, l, ev[2], ev[2] == 0);
        break;

    case 0x3E:  /* LE Meta */
        if (len >= 3) le_on_meta(h, ev + 2, len - 2);
        break;

    case 0x0E:  /* Command Complete: credits, opcode, return parameters */
        if (len < 6) break;
        for (unsigned j = 0; j < OWN_MAX; j++) {
            if (!h->sync.serial || h->commands[j].ref.serial != h->sync.serial ||
                h->commands[j].ref.attempt != h->sync.attempt ||
                h->commands[j].opcode != le16(ev + 3) || h->sync_done) continue;
            own_state state = OWN_FREE;
            own_result result = own_status(h->ownership, h->sync, &state);
            h->cc_op = le16(ev + 3);
            h->cc_len = len < (int)sizeof h->cc ? len : (int)sizeof h->cc;
            memcpy(h->cc, ev, (size_t)h->cc_len);
            h->sync_done = 1;
            h->sync_ok = !ev[5] && result == OWN_OK &&
                         (state == OWN_ACKNOWLEDGED || state == OWN_CLOSED);
        }
        break;

    case 0x0F:  /* Command Status: status, credits, opcode */
        if (len >= 6 && ev[2] && le16(ev + 4) == 0x2026) {
            le_dhkey_refused(h);
            break;
        }
        if (len >= 6 && le16(ev + 4) == OP_INQUIRY) {
            log_line("inquiry: controller answered status %#04x", ev[2]);
            if (ev[2] == 0x0C) {
                /* Command disallowed: an inquiry is already running, probably one
                 * an earlier run left on the chip. Stop it and wait for its end
                 * instead of asking again at once. */
                h->inquiring = 1;
                h->inq_started = now_ms();
            } else if (ev[2] != 0) {
                h->inquiring = 0;
            }
        }
        if (len >= 6 && le16(ev + 4) == 0x200D) {     /* LE: its own handling */
            le_on_create_status(h, ev[2]);
            break;
        }
        if (len < 6 || ev[2] == 0) break;
        if (le16(ev + 4) == OP_CREATE_CONNECTION && ev[2] == 0x0B) report_existing_link(h);
        if (le16(ev + 4) == OP_CREATE_CONNECTION || le16(ev + 4) == OP_ACCEPT_CONNECTION ||
            le16(ev + 4) == 0x200D) {
            int want_le = le16(ev + 4) == 0x200D;
            for (i = 0; i < HOST_MAX_PADS; i++) {
                l = &h->links[i];
                if (l->used && !l->connected && l->le == want_le &&
                    (l->pairing || want_le || le16(ev + 4) == OP_ACCEPT_CONNECTION)) {
                    if (!want_le && ev[2] == 0x0b) {
                        memcpy(h->collision_addr, l->addr, 6);
                        h->collision_blocked = 1;
                    }
                    log_line("pad %s: connection refused (status %#04x)", addr_str(l->addr), ev[2]);
                    link_free(h, l);
                }
            }
        } else if (le16(ev + 4) == OP_REJECT_CONNECTION && h->reject_pending) {
            /* There was no call to turn down: a real link holds the pad. */
            h->reject_pending = 0;
            h->repage_at = 0;
            memcpy(h->collision_addr, h->reject_addr, 6);
            h->collision_blocked = 1;
            log_line("pad %s: no call of its own was pending; a link exists, left alone", addr_str(h->reject_addr));
        } else if (le16(ev + 4) == OP_AUTH_REQUESTED) {
            /* Usually a collision with the pad's own authentication:
             * wait and see, and ask again if nothing comes of it. */
            for (i = 0; i < HOST_MAX_PADS; i++) {
                l = &h->links[i];
                if (l->used && l->auth_sent && !l->auth_ok) {
                    l->auth_sent = 0;
                    l->t_conn = now_ms();
                }
            }
        }
        break;

    case 0x13:  /* Number Of Completed Packets: (handle, count) pairs */
        for (i = 0; i < ev[2] && 3 + i * 4 + 4 <= len; i++) {
            int done = (int)le16(ev + 3 + i * 4 + 2);
            l = link_by_handle(h, le16(ev + 3 + i * 4));
            if (l) pool_done(pool_of(h, l), done);
        }
        break;

    case 0x16:  /* PIN Code Request: legacy pairing */
        if (len < 8 || !(l = link_by_peer(h, ev + 2, 0, 0))) break;
        memset(p, 0, sizeof p);
        memcpy(p, ev + 2, 6);
        if (pin_is_address(h, l)) {
            /* Nintendo's Wii pads paired with their Sync button take the
             * host's address as their PIN, its 6 bytes as sent (wiibrew). */
            p[6] = 6;
            memcpy(p + 7, h->bdaddr, 6);
            l->pin_zero = 0;
            log_line("pad %s: PIN requested; answering with this console's address", addr_str(l->addr));
        } else {
            p[6] = 4;
            memcpy(p + 7, "0000", 4);
            l->pin_zero = 1;
        }
        send_cmd(h, OP_PIN_CODE_REPLY, p, 23);
        break;

    case 0x17:  /* Link Key Request */
        if (len < 8 || !link_by_peer(h, ev + 2, 0, 0)) break;
        {
            db_rec *r = db_find(h, ev + 2);
            if (r) {
                memcpy(p, ev + 2, 6);
                memcpy(p + 6, r->key, 16);
                send_cmd(h, OP_LINK_KEY_REPLY, p, 22);
            } else {
                send_cmd(h, OP_LINK_KEY_NEG_REPLY, ev + 2, 6);
                l = link_by_peer(h, ev + 2, 0, 0);
                if (l) { l->ssp_stage = 1; l->t_ssp = now_ms(); }
            }
        }
        break;

    case 0x18:  /* Link Key Notification: addr, key, type */
        if (len < 25 || !(l = link_by_peer(h, ev + 2, 0, 0))) break;
        {
            db_rec *r = db_put(h, ev + 2, 0, 0);
            if (!r) break;
            memcpy(r->key, ev + 8, 16);
            r->type = ev[24];
            r->complete = 1;
            if (db_save(h) != BOND_COMMITTED) break;
        }
        log_line("pad %s: paired, key stored", addr_str(ev + 2));
        break;

    case 0x31:  /* IO Capability Request */
        if (len < 8 || !(l = link_by_peer(h, ev + 2, 0, 0))) break;
        send_io_cap_reply(h, ev + 2);
        l->ssp_stage = 2;
        l->t_ssp = now_ms();
        break;

    case 0x33:  /* User Confirmation Request */
        if (len < 8 || !(l = link_by_peer(h, ev + 2, 0, 0))) break;
        send_cmd(h, OP_USER_CONFIRM_REPLY, ev + 2, 6);
        l->ssp_stage = 3;
        break;

    default:
        break;
    }
}

/* ---- per-link state machine ---------------------------------------------- */

/* Advances one link from whatever has happened so far. Each step waits
 * first for the pad to take it, then takes it itself, so it works for pads
 * that drive their own reconnection and for those that wait to be asked. */
static void link_step(host_t *h, link_t *l, long now)
{
    unsigned char p[3];

    if (l->le) {
        le_step(h, l, now);
        return;
    }
    if (l->disconnecting) return;
    if (!l->connected) {
        if (now - l->t_create > T_CONN_TIMEOUT) {
            if (!l->create.serial) own_fail(h->ownership, (uint64_t)now, 1);
            link_disconnect(h, l, "no connection");
        } else if (!l->create.serial && l->connect_opcode) {
            send_link_cmd(h, l, l->connect_opcode, l->connect_params,
                          l->connect_opcode == OP_ACCEPT_CONNECTION ? 7 : 13);
        }
        return;
    }
    /* A pairing event lost on the way (taken by the system's driver, which
     * reads the same pipe) would leave the pairing waiting for the whole
     * setup time. The answer to the step that did not come is given anyway:
     * if the controller never asked, it ignores it. */
    if (l->ssp_stage && l->ssp_stage < 3 && !l->auth_ok && now - l->t_ssp > T_SSP_STALL) {
        if (l->ssp_stage == 1) {
            log_line("pad %s: no capability request after the key was refused; answering it anyway", addr_str(l->addr));
            send_io_cap_reply(h, l->addr);
            l->ssp_stage = 2;
        } else {
            log_line("pad %s: no confirmation request after our capabilities; confirming anyway", addr_str(l->addr));
            send_cmd(h, OP_USER_CONFIRM_REPLY, l->addr, 6);
            l->ssp_stage = 3;
        }
        l->t_ssp = now;
    }
    if (!l->ready && now - l->t_conn > T_SETUP_TIMEOUT) {
        link_disconnect(h, l, "setup did not finish");
        return;
    }

    /* Authentication and encryption. */
    if (!l->auth_ok) {
        if (!l->auth_sent && now - l->t_conn >= (l->pairing ? 0 : T_SELF_AUTH)) {
            put16(p, l->handle);
            l->auth_sent = send_cmd(h, OP_AUTH_REQUESTED, p, 2);
        }
        return;
    }
    if (!l->enc_on) {
        if (!l->enc_sent && now - l->t_auth_ok >= (l->pairing ? 0 : T_SELF_ENCRYPT)) {
            put16(p, l->handle);
            p[2] = 1;
            l->enc_sent = send_cmd(h, OP_SET_ENCRYPTION, p, 3);
        }
        return;
    }
    if (!l->t_secure) {
        l->t_secure = now;
        log_line("pad %s: link encrypted", addr_str(l->addr));
    }

    /* Which pad is it: the store, the pad's Device ID record, and for pads
     * with no profile, the pad's own HID descriptor. */
    if (!l->ids_known && !l->sdp_query) {
        db_rec *r = db_find(h, l->addr);
        if (r && (r->vid || r->pid)) {
            l->vid = r->vid;
            l->pid = r->pid;
            l->ids_known = 1;
        } else {
            sdp_start(l, SDP_Q_IDS);
        }
    }
    if (l->ids_known && !l->prof && !l->sdp_query) {
        l->prof = profile_find(l->vid, l->pid);
        if (l->prof && l->prof->setup) l->prof->setup(l->pctx.bytes, l->vid, l->pid);
        if (!l->prof) {
            log_line("pad %s: %04x:%04x has no profile, reading its HID descriptor",
                     addr_str(l->addr), l->vid, l->pid);
            sdp_start(l, SDP_Q_DESC);
        }
    }
    if (l->sdp_query) {
        sdp_step(h, l, now);
        return;
    }
    if (l->disconnecting || !l->prof) return;
    if (l->sdp.st == CH_OPEN) chan_close(h, l, &l->sdp);

    /* HID channels: control, then interrupt. */
    chan_tick(h, l, &l->ctrl, now);
    chan_tick(h, l, &l->intr, now);
    if (l->ctrl.st == CH_CLOSED && now - l->t_secure >= T_SELF_CHANNELS)
        chan_connect(h, l, &l->ctrl);
    if (l->ctrl.st == CH_OPEN && l->intr.st == CH_CLOSED &&
        now - l->ctrl.t_state >= T_INTR_AFTER)
        chan_connect(h, l, &l->intr);

    if (!l->ready) {
        if (l->ctrl.st == CH_OPEN && l->intr.st == CH_OPEN) link_ready(h, l);
        return;
    }

    init_tick(h, l, now);
    if (l->prof->rumble_refresh_ms && (l->out.strong || l->out.weak) &&
        now - l->t_out >= l->prof->rumble_refresh_ms)
        send_output(h, l);
}

/* ---- pumping ---------------------------------------------------------------- */

static void pump(host_t *h, int timeout_ms)
{
    unsigned char buf[HCI_PKT_MAX];
    int n;

    if (h->hci.ops->pump(h->hci.ctx, timeout_ms) < 0) h->dead = 1;
    own_tick(h->ownership, (uint64_t)now_ms());
    attrib_poll(now_ms());
    attribution_check(h);
    if (h->ev.idle) h->ev.idle(h->ev.ud);
    while ((n = h->hci.ops->next_event(h->hci.ctx, buf, (int)sizeof buf)) > 0) {
        h->last_rx = now_ms();
        attribution_check(h);
        h->dispatching++;
        on_event(h, buf, n);
        h->dispatching--;
    }
    while ((n = h->hci.ops->next_acl(h->hci.ctx, buf, (int)sizeof buf)) > 0) {
        h->last_rx = now_ms();
        if (!WHATEVERPAD_PROBE && host_admits(h)) {
            h->dispatching++;
            on_acl(h, buf, n);
            h->dispatching--;
        }
    }
    host_flush(h);
}

static void start_inquiry(host_t *h)
{
    /* General inquiry, ~10 s, unlimited responses. */
    unsigned char inq[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };

    inq[3] = (unsigned char)g_tune.inq_len;

    if (send_cmd(h, OP_INQUIRY, inq, 5)) {
        h->round_heard = 0;
        h->inquiring = 1;
        h->inq_started = now_ms();
        log_line("inquiry: started");
    } else {
        static int fails;
        if (fails++ < 5) log_line("inquiry: the command could not be sent");
    }
}

/* The system can leave the controller listening for calls nearly all the
 * time: page scan of 334 slots every 668, interlaced (two windows back to
 * back), so the radio has almost no time left for anything else. Then a
 * search hears almost nothing and a call to a pad gets no answer. In the logs
 * of 46 runs (every version, 0.5.25 to 0.6.15): 0.34 devices heard per search
 * round and 16 calls with no answer in that mode, against 0.98 and none with
 * the system's other value (18 slots every 2160). What decided a good or a bad
 * run was this, not the version. While pairing, it is set to a light value
 * (18 slots every 1.28 s, the most the specification allows for R1, so a
 * DualSense still reconnects at once), and the system's value is put back
 * when pairing ends. */
/* The heavy page scan was seen on the MediaTek chip. On a Marvell/NXP one
 * the system's setting is left alone, as programs that work on it do. */
static int chip_is_marvell(const host_t *h)
{
    return h->hci.ops->chip_vendor && h->hci.ops->chip_vendor(h->hci.ctx) == 0x1286;
}

static void page_scan_for_pairing(host_t *h)
{
    unsigned char light[4];
    unsigned iv, win;

    if (!g_tune.ps_light || chip_is_marvell(h)) return;
    light[0] = (unsigned char)g_tune.ps_iv;
    light[1] = (unsigned char)(g_tune.ps_iv >> 8);
    light[2] = (unsigned char)g_tune.ps_win;
    light[3] = (unsigned char)(g_tune.ps_win >> 8);
    if (!hci_sync(h, OP_READ_PAGE_SCAN_ACT, NULL, 0) || h->cc_len < 10) return;
    iv = le16(h->cc + 6);
    win = le16(h->cc + 8);
    if (win * 8 <= iv) return;          /* light already: the radio is free enough */
    if (iv == g_tune.ps_iv && win == g_tune.ps_win) return;
    unsigned char original[4];
    put16(original, iv);
    put16(original + 2, win);
    own_readback(h->ownership, OP_WRITE_PAGE_SCAN_ACT, original, sizeof original);
    if (!host_setting(h, OP_WRITE_PAGE_SCAN_ACT, light, 4, original, 4)) return;
    h->ps_lightened++;
    if (h->ps_lightened <= 5 || (h->ps_lightened & (h->ps_lightened - 1)) == 0)
        log_line("radio: the system listens for calls %u slots of every %u; lightened to %u of %u while pairing (%d)",
                 win, iv, g_tune.ps_win, g_tune.ps_iv, h->ps_lightened);
}

/* Puts back the system's page scan, unless the system changed it meanwhile. */
static void page_scan_restore(host_t *h)
{
    for (unsigned i = 0; i < OWN_MAX; i++) {
        own_token token;
        if (h->commands[i].opcode == OP_WRITE_PAGE_SCAN_ACT &&
            own_request_undo(h->ownership, h->commands[i].ref,
                             (uint64_t)now_ms(), &token) == OWN_OK) {
            hci_submission submission = {.kind = HCI_SUBMIT_COMMAND, .token.command = &token};
            h->hci.ops->op_submit(h->hci.ctx, &submission);
            return;
        }
    }
}

/* Pairing in progress: a search window open, a call to repeat, or a pad
 * being linked. */
static int pairing_busy(const host_t *h, long now)
{
    int i;

    if (now < h->pair_until || h->repage_at || h->defer_at) return 1;
    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && h->links[i].pairing && !h->links[i].ready) return 1;
    return 0;
}

/* The radio is kept light while pairing and while pads of ours are connected:
 * with the system's heavy page scan, a connected pad gets few slots left and
 * lags (console, 9 Oct: the DS4 lagged badly until the page scan was made
 * light again, then went fine). */
static int radio_wanted(const host_t *h, long now)
{
    int i;

    if (pairing_busy(h, now)) return 1;
    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && h->links[i].ready) return 1;
    return 0;
}

/* Our event reads are stopped while the events are left to the system: a
 * command now would get no reply. */
static int yielding(const host_t *h, long now)
{
    return h->yield_at && now - h->yield_at < T_YIELD;
}

void host_poll(host_t *h, int timeout_ms)
{
    long now;
    int i, linking = 0;

    if (!h) return;                     /* no Bluetooth: nothing to do */

    pump(h, timeout_ms);
    if (!host_admits(h)) return;
#if WHATEVERPAD_PROBE
    reads_for(h, HOST_EV_READS);
    return; /* Observation only: no idle watchdog retransmits or protocol retries. */
#endif
    now = now_ms();
    /* Event reads: all of them while pairing or waiting for a reply; while
     * idle, as many as the dialog mode says (all, unless changed). */
    if (!h->dead) {
        int busy = pairing_busy(h, now) || now < h->full_reads_until;
        reads_for(h, busy ? HOST_EV_READS : g_tune.idle_reads);
    }
    /* First of all, so that what follows sees any event its replies brought. */
    if (!h->dead && !yielding(h, now)) {
        if (radio_wanted(h, now)) {
            if (now >= h->ps_check_at) {
                h->ps_check_at = now + (pairing_busy(h, now) ? T_PS_CHECK : T_PS_CHECK_IDLE);
                page_scan_for_pairing(h);
            }
        } else {
            h->ps_check_at = 0;
            page_scan_restore(h);
        }
        now = now_ms();
    }
    for (i = 0; i < HOST_MAX_PADS; i++) {
        if (!h->links[i].used) continue;
        link_step(h, &h->links[i], now);
        if (h->links[i].used && h->links[i].pairing && !h->links[i].ready &&
            !h->links[i].le) linking = 1;
    }
    le_poll(h, now);

    /* A controller that stopped answering (rest mode can leave the USB
     * device open but deaf) is probed, and given up if still silent. */
    /* While the events are left to the system on purpose (see yield), the
     * silence is ours: no probing, or the controller would be declared lost
     * and Bluetooth reopened in the middle of a search. */
    if (h->yield_at && now - h->yield_at < T_YIELD + T_PROBE_IDLE) {
        h->probes = 0;
    } else if (now - h->last_rx > T_PROBE_IDLE) {
        if (h->probes >= PROBE_TRIES && now - h->probe_at > T_PROBE_WAIT) {
            if (!h->dead) log_line("controller silent after %d probes: transport lost", h->probes);
            h->dead = 1;
        } else if (h->probes < PROBE_TRIES && now - h->probe_at > T_PROBE_WAIT) {
            send_cmd(h, OP_READ_BUFFER_SIZE, NULL, 0);     /* any reply will do */
            h->probe_at = now;
            h->probes++;
        }
    } else {
        h->probes = 0;
    }

    /* Keep searching through the pairing window, one pad at a time. */
    if (h->defer_at && now >= h->pair_until) {
        log_line("inquiry: pending candidate expired with the search window");
        h->defer_at = 0;
    } else if (h->defer_at && !linking &&
               (now >= h->pair_until - 1000 || now - h->defer_at >= T_EIR_WAIT)) {
        unsigned char rec[14];
        memcpy(rec, h->defer_rec, sizeof rec);
        h->defer_at = 0;
        log_line("found gamepad %s; no complete answer came in time, calling it anyway (%ld ms of search)",
                 addr_str(rec), now - h->search_started);
        pair_with(h, rec);
        /* pair_with may allocate a link: do not start another inquiry in
         * this same poll with the stale value of linking. */
        for (i = 0; i < HOST_MAX_PADS; i++)
            if (h->links[i].used && h->links[i].pairing && !h->links[i].ready &&
                !h->links[i].le) linking = 1;
    }
    if (h->repage_at && now >= h->repage_at) {
        unsigned char rec[14];
        memcpy(rec, h->last_call, sizeof rec);
        h->repage_at = 0;
        h->reject_pending = 0;
        h->in_repage = 1;
        pair_with(h, rec);
        h->in_repage = 0;
        for (i = 0; i < HOST_MAX_PADS; i++)
            if (h->links[i].used && h->links[i].pairing && !h->links[i].ready &&
                !h->links[i].le) linking = 1;
    }
    if (h->pair_pending_s && now >= h->pair_wait_until) {
        int s = h->pair_pending_s;
        h->pair_pending_s = 0;
        log_line("pairing: the pause for the system is over");
        host_pair(h, s);
    }
    if (now < h->pair_until && !h->inquiring && !linking && !h->repage_at) start_inquiry(h);
    if (now >= h->pair_until && h->inquiring) {
        h->inquiring = 0;
    }
}

int host_pair(host_t *h, int seconds)
{
    long now = now_ms();
    if (!host_admits(h)) return 0;

#if WHATEVERPAD_PROBE
    (void)h;
    (void)seconds;
    return 0;
#endif
    /* A fresh request or cancellation must not inherit an old candidate. */
    h->defer_at = 0;
    h->collision_blocked = 0;
    h->turned_down = 0;
    h->fresh_left = FRESH_LINK_TRIES;
    h->reject_pending = 0;
    if (seconds <= 0) {
        h->repage_at = 0;
        h->repage_left = 0;
        h->pair_pending_s = 0;
        h->pair_until = now;
        return 1;
    }

    /* During a pause for the system (see yield) our event reads are stopped:
     * a search now would hear nothing until it ends and lose that part of
     * its window. It waits, and then gets its whole window. The pause itself
     * is kept: it protects the console's DualSense. */
    if (h->yield_at && now - h->yield_at < T_YIELD) {
        h->pair_pending_s = seconds;
        h->pair_wait_until = h->yield_at + T_YIELD;
        log_line("pairing: waits %ld s for the pause for the system to end, then searches for %d s",
                 (h->pair_wait_until - now + 999) / 1000, seconds);
        return 1;
    }
    h->pair_pending_s = 0;
    h->pair_until = now + (long)seconds * 1000;
    h->search_started = now;
    log_line("pairing: searching for %d s", seconds);
    return 1;
}

int host_pair_wait_left(const host_t *h)
{
    long left;

    if (!h->pair_pending_s) return 0;
    left = h->pair_wait_until - now_ms();
    return left > 0 ? (int)((left + 999) / 1000) : 1;
}

int host_set_output(host_t *h, int slot, const pad_output *out)
{
    link_t *l;

    if (slot < 0 || slot >= HOST_MAX_PADS) return 0;
    l = &h->links[slot];
    if (!l->used || !l->ready || l->disconnecting || !host_admits(h)) return 0;
    l->out = *out;
    return send_output(h, l);
}

int host_connected(const host_t *h, int slot)
{
    return slot >= 0 && slot < HOST_MAX_PADS && h->links[slot].used &&
           h->links[slot].ready && !h->links[slot].disconnecting;
}

int host_paired_count(const host_t *h) { return h->committed.count; }

int host_transport_lost(const host_t *h) { return h->dead; }

/* Seconds left of the pause for the system (a call that was not ours,
 * usually the DualSense coming back), 0 if none. */
int host_pause_left(const host_t *h)
{
    long left;

    if (!h->yield_at) return 0;
    left = T_YIELD - (now_ms() - h->yield_at);
    return left > 0 ? (int)((left + 999) / 1000) : 0;
}

int host_pairing_left(const host_t *h)
{
    long left = h->pair_until - now_ms();
    return left > 0 ? (int)((left + 999) / 1000) : 0;
}

/* The address a link's pad is stored under: its identity, for LE pads
 * behind private addresses. */
static const unsigned char *link_identity(const link_t *l)
{
    static const unsigned char zero[6];
    if (l->le && memcmp(l->le_s.id_addr, zero, 6) != 0) return l->le_s.id_addr;
    return l->addr;
}

int host_paired_get(const host_t *h, int i, host_paired_info *out)
{
    const db_rec *d;
    int k;

    if (i < 0 || i >= h->committed.count) return 0;
    d = &h->committed.records[i];
    memset(out, 0, sizeof *out);
    memcpy(out->addr, d->addr, 6);
    out->vid = d->vid;
    out->pid = d->pid;
    out->le = d->le;
    for (k = 0; k < HOST_MAX_PADS; k++) {
        const link_t *l = &h->links[k];
        if (host_connected(h, k) && l->le == d->le &&
            (!d->le || l->le_s.id_type == d->addr_type) &&
            memcmp(link_identity(l), d->addr, 6) == 0) out->connected = 1;
    }
    return 1;
}

int host_pad_get(const host_t *h, int slot, pad_info *info, pad_state *st)
{
    const link_t *l;

    if (!host_connected(h, slot)) return 0;
    l = &h->links[slot];
    memset(info, 0, sizeof *info);
    info->vid = l->vid;
    info->pid = l->pid;
    info->profile = l->prof ? l->prof->name : "";
    memcpy(info->addr, link_identity(l), 6);
    *st = l->st;
    return 1;
}

int host_forget(host_t *h, const unsigned char addr[6])
{
    int k;
    db_rec identity, *found = NULL;

    for (k = 0; k < h->store.count; k++) {
        db_rec *r = &h->store.records[k];
        if (memcmp(r->addr, addr, 6)) continue;
        if (found) return 0;             /* address-only API cannot choose a namespace */
        found = r;
    }
    if (!found) return 0;
    identity = *found;
    if (db_forget(h, addr, identity.le, identity.addr_type) != BOND_COMMITTED) return 0;
    for (k = 0; k < HOST_MAX_PADS; k++) {
        link_t *l = &h->links[k];
        if (l->used && l->le == identity.le && memcmp(link_identity(l), addr, 6) == 0 &&
            (!l->le || l->le_s.id_type == identity.addr_type)) {
            send_unplug(h, l);
            link_disconnect(h, l, "forgotten from the web page");
        }
    }
    log_line("pad %s: forgotten", addr_str(addr));
    return 1;
}

/* Forgets every paired pad: each session starts clean. Connected pads are
 * left alone (none are expected right after opening). */
int host_forget_all(host_t *h)
{
    int n = h->store.count;

    if (!n) return 0;
    h->store.count = 0;
    memset(h->store.records, 0, sizeof h->store.records);
    if (db_save(h) != BOND_COMMITTED) return 0;
    log_line("store: %d paired pad(s) forgotten, the session starts clean", n);
    return n;
}

/* ---- controller setup ----------------------------------------------------- */

static int command_wait(host_t *h, own_ref ref)
{
    long deadline = now_ms() + (h->setup_retry ? 5000 : 2000);
#if WHATEVERPAD_PROBE
    deadline = probe_deadline_ms;
#endif
    h->sync = ref;
    h->sync_done = h->sync_ok = 0;
    h->cc_op = 0;
    while (!h->sync_done && host_admits(h) && now_ms() < deadline) pump(h, 20);
    own_tick(h->ownership, (uint64_t)now_ms());
    int ok = h->sync_done && h->sync_ok && host_admits(h);
#if WHATEVERPAD_PROBE
    if (!ok && now_ms() >= deadline) {
        long tail_end = deadline + PROBE_TAIL_MS;
        log_line("probe deadline mono_ms=%ld deadline_ms=%ld tail_end_ms=%ld submitted=%lu",
                 now_ms(), deadline, tail_end, attrib_get()->submitted);
        /* Observation cannot reopen admission or turn a quarantined ACK into success. */
        while (!h->dead && now_ms() < tail_end)
            pump(h, (int)(tail_end - now_ms() < 20 ? tail_end - now_ms() : 20));
        log_line("probe tail_complete mono_ms=%ld submitted=%lu late=%lu",
                 now_ms(), attrib_get()->submitted, attrib_get()->classes[ATTRIB_LATE]);
    }
#endif
    if ((!h->sync_done || (WHATEVERPAD_PROBE && !ok)) &&
        !h->diag_done && h->hci.ops->diag) {
        h->diag_done = 1;
        h->hci.ops->diag(h->hci.ctx);
    }
    h->sync = (own_ref){0};
    return ok;
}

int host_setting(host_t *h, unsigned op, const void *p, int len,
                 const void *original, int original_len)
{
    own_ref ref;
    if (h->sync.serial) return 0;
    int prepared = command_prepare(h, NULL, op, p, len, original, original_len, &ref);
    if (prepared == 2) return 1;
    if (!prepared) return 0;
    return command_wait(h, ref);
}

int hci_sync(host_t *h, unsigned op, const void *p, int plen)
{
    return host_setting(h, op, p, plen, NULL, 0);
}

/* Diagnostics only: reads a setting of the controller (a read command, which
 * changes nothing) and writes its reply to the log, to compare the
 * controller as the system leaves it after a plain restart and after turning
 * Bluetooth off and on in the settings. */
static void chip_read(host_t *h, const char *name, unsigned op, const void *p, int plen)
{
    h->cc_op = 0;
    hci_sync(h, op, p, plen);
    if (h->cc_op != op || h->cc_len < 6) {
        log_line("chip: %s: no reply", name);
        return;
    }
    log_line("chip: %s: status %#04x, len %d", name, h->cc[5], h->cc_len - 6);
}

static void chip_report(host_t *h)
{
    static const unsigned char page1[1] = { 1 };

    chip_read(h, "version", 0x1001, NULL, 0);
    chip_read(h, "features", 0x1003, NULL, 0);
    chip_read(h, "scan enable", 0x0C19, NULL, 0);
    chip_read(h, "page timeout", 0x0C17, NULL, 0);
    chip_read(h, "connection accept timeout", 0x0C15, NULL, 0);
    chip_read(h, "page scan activity", 0x0C1B, NULL, 0);
    chip_read(h, "page scan type", 0x0C46, NULL, 0);
    chip_read(h, "inquiry scan activity", 0x0C1D, NULL, 0);
    chip_read(h, "inquiry scan type", 0x0C42, NULL, 0);
    chip_read(h, "inquiry mode", 0x0C44, NULL, 0);
    chip_read(h, "current IAC LAP", 0x0C39, NULL, 0);
    chip_read(h, "inquiry response tx power", 0x0C58, NULL, 0);
    chip_read(h, "class of device", 0x0C23, NULL, 0);
    chip_read(h, "simple pairing mode", 0x0C55, NULL, 0);
    chip_read(h, "AFH assessment mode", 0x0C48, NULL, 0);
    chip_read(h, "default link policy", 0x080E, NULL, 0);
    chip_read(h, "LE host support", 0x0C6C, NULL, 0);
    chip_read(h, "local name", 0x0C14, NULL, 0);
    chip_read(h, "extended inquiry response", 0x0C51, NULL, 0);
    chip_read(h, "supported commands", 0x1002, NULL, 0);
    chip_read(h, "buffer size", 0x1005, NULL, 0);
    chip_read(h, "local extended features p1", 0x1004, page1, 1);
}

/* Prepares the controller, changing as little of it as it can. The
 * controller is shared with the system's driver, so:
 *   - no reset, no name, no class of device, no inquiry mode;
 *   - the event mask: needed, as Secure Simple Pairing events (IO
 *     capability, user confirmation) are off in the default mask. There is
 *     no command to read it back, so it is left as set;
 *   - Simple Pairing mode: only written if off. It cannot be turned off
 *     again once on, by specification;
 *   - page scan: needed for paired pads to reconnect by themselves. The
 *     value found is kept and put back on close, unless someone else has
 *     changed it meanwhile. */
static int setup(host_t *h)
{
    /* Connection Request (bit 3) is left out. WhateverPad never waits for calls:
     * every pad of a session is called by WhateverPad. And a call that reached
     * WhateverPad never reached the system: a DualSense switched off and on again
     * calls the console, WhateverPad took the call and nobody answered it, and the
     * DualSense stayed blinking blue (the pause for the system did not help:
     * the logs show the call waiting for WhateverPad's reads at the end of every
     * pause, 15 times in a row). */
    unsigned char mask[8] = { 0xF7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F };
    unsigned char scan[1];

    h->reads_now = HOST_EV_READS;
    /* No setting or link can obtain a barrier before this capability check. */
    if (!WHATEVERPAD_PROBE &&
        (!hci_sync(h, 0x1002, NULL, 0) || h->cc_len < 70 || !(h->cc[6 + 27] & 0x80))) {
        log_line("controller: LE Rand unavailable; ownership barriers unsupported");
        return 0;
    }
    if (!hci_sync(h, OP_READ_BUFFER_SIZE, NULL, 0) || h->cc_len < 13) return 0;
    h->acl_mtu = le16(h->cc + 6);
    h->br.credits = h->br.max = (int)le16(h->cc + 9);
    log_line("controller: %d ACL buffers of %u bytes", h->br.max, h->acl_mtu);
    if (WHATEVERPAD_PROBE) return 1; /* T8-v2: one query, then metadata only. */

    if (!g_tune.mask_conn) mask[0] = 0xFF;
    if (!hci_sync(h, OP_SET_EVENT_MASK, mask, 8)) return 0;
    if (g_tune.mask_conn)
        log_line("controller: calls to the console left to the system (event mask without Connection Request)");
    if (!hci_sync(h, OP_READ_SSP_MODE, NULL, 0) || h->cc_len < 7 || h->cc[6] != 1) {
        log_line("controller: Simple Pairing is not enabled; irreversible change refused");
        return 0;
    }
    if (!hci_sync(h, OP_READ_SCAN_ENABLE, NULL, 0) || h->cc_len < 7) return 0;
    unsigned char original = h->cc[6];
    if (!(original & 0x02)) {
        scan[0] = (unsigned char)(original | 0x02);
        if (!host_setting(h, OP_WRITE_SCAN_ENABLE, scan, 1, &original, 1)) return 0;
        log_line("controller: page scan turned on (was %#x)", original);
    }

    if (!hci_sync(h, OP_READ_BD_ADDR, NULL, 0) || h->cc_len < 12) {
        log_line("store: local adapter identity unavailable");
        return 0;
    }
    memcpy(h->bdaddr, h->cc + 6, 6);
    log_line("controller address %s", addr_str(h->bdaddr));
    if (!WHATEVERPAD_PROBE) chip_report(h); /* report repeats setup's queries */
    le_setup(h);
    return 1;
}

host_t *host_prepare(hci_t hci, const char *db_path, const host_events *ev)
{
    host_t *h = calloc(1, sizeof *h);

    if (!h) return NULL;
    closed_radio_left = 0;
    h->ownership = own_new(WHATEVERPAD_ATTRIB_POLICY, 1);
    if (!h->ownership) { free(h); return NULL; }
    h->output = own_acl_new(h->ownership);
    if (!h->output) { own_free(h->ownership); free(h); return NULL; }
    h->hci = hci;
#if WHATEVERPAD_PROBE
    probe_deadline_ms = 0;
#endif
    h->ambiguity_seen = ambiguities();
    if (WHATEVERPAD_ATTRIB_POLICY == OWN_POLICY_NO_RESTORE)
        log_line("NO_RESTORE: changed radio settings stay as written; turn bridged controllers off to drop their radio links");
    if (ev) h->ev = *ev;
    if (strlen(db_path) >= sizeof h->db_path) {
        own_acl_free(h->output); own_free(h->ownership); free(h); return NULL;
    }
    snprintf(h->db_path, sizeof h->db_path, "%s", db_path);
    {
        const char *slash = strrchr(db_path, '/');
        int dir = slash ? (int)(slash - db_path) : 1;
        snprintf(h->map_dir, sizeof h->map_dir, "%.*s/maps", dir, slash ? db_path : ".");
    }
    {
        const char *slash = strrchr(db_path, '/');
        int dir = slash ? (int)(slash - db_path) : 1;
        snprintf(h->ban_path, sizeof h->ban_path, "%.*s/ban.txt", dir, slash ? db_path : ".");
    }
    ban_load(h);
    h->last_rx = now_ms();
    return h;
}

own_ledger *host_ownership(host_t *h) { return h ? h->ownership : NULL; }

unsigned host_radio_left(const host_t *h)
{
    own_summary summary;
    if (!h) return closed_radio_left;
    own_snapshot(h->ownership, &summary);
    return summary.radio_left;
}

int host_start(host_t *h)
{
    if (!h || !h->hci.ops->op_submit) return 0;
    h->setup_retry = !WHATEVERPAD_PROBE;
    int ready = setup(h);
    h->setup_retry = 0;
    if (!ready) return 0;
    if (WHATEVERPAD_PROBE) return own_activate(h->ownership); /* No store/IRK work. */
    memcpy(h->store.adapter.addr, h->bdaddr, 6);
    if (h->hci.ops->chip_vendor) h->store.adapter.vid = (uint16_t)h->hci.ops->chip_vendor(h->hci.ctx);
    if (h->hci.ops->chip_product) h->store.adapter.pid = (uint16_t)h->hci.ops->chip_product(h->hci.ctx);
    h->store_error = bond_store_load(h->db_path, &h->store.adapter, &h->store);
    if (h->store_error == BOND_MISSING) h->store_error = BOND_COMMITTED;
    if (h->store_error != BOND_COMMITTED)
        log_line("store: load failed, preserved: %s", bond_store_result_name(h->store_error));
    h->committed = h->store;
    log_line("store: %d paired pad(s)", h->store.count);
    long deadline = now_ms() + 5000;
    while (host_admits(h)) {
        if (own_activate(h->ownership)) return 1;
        if (now_ms() >= deadline) break;
        pump(h, 20);
    }
    return 0;
}

void host_close(host_t *h)
{
    int i;
    long now, deadline;
    own_summary summary;

    if (!h) return;
    own_release(h->ownership, SESSION_STOP, (uint64_t)now_ms());
#if WHATEVERPAD_PROBE
    h->dead = 1; /* NO_RESTORE: local cleanup only, never guessed radio undo. */
#endif
    for (i = 0; i < HOST_MAX_PADS; i++) {
        if (!h->links[i].used) continue;
        if (h->dead) link_free(h, &h->links[i]);    /* nothing to tell the pad */
        else link_disconnect(h, &h->links[i], "closing");    /* the pad keeps its key: it reconnects next time */
    }
    own_snapshot(h->ownership, &summary);
    now = now_ms();
    deadline = now + session_work_remaining(&summary.runtime, (uint64_t)now, 2000);
    for (;;) {
        int any = 0;
        for (i = 0; i < HOST_MAX_PADS; i++) any |= h->links[i].used;
        now = now_ms();
        if (h->dead || !any || now >= deadline ||
            !own_drain_allowed(h->ownership, (uint64_t)now, 0)) break;
        pump(h, deadline - now < 50 ? (int)(deadline - now) : 50);
        if (now_ms() <= now) break; /* No progress: finalize instead of retrying the drain. */
    }
    if (!h->dead) host_flush(h); /* Only the ledger can select a derived undo. */
    if (h->hci.ops->close) h->hci.ops->close(h->hci.ctx);
    own_transport_closed(h->ownership, (uint64_t)now_ms());
    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used) link_free(h, &h->links[i]);
    own_snapshot(h->ownership, &summary);
    closed_radio_left = summary.radio_left;
    own_acl_free(h->output);
    own_free(h->ownership);
    free(h);
}

/* ---- dialog mode ------------------------------------------------------------ */

int host_tune_set(host_t *h, const char *name, long v)
{
    if (strcmp(name, "ps_light") == 0 && (v == 0 || v == 1)) {
        g_tune.ps_light = (int)v;
        if (h && !v && !yielding(h, now_ms())) page_scan_restore(h);
        if (h) h->ps_check_at = 0;
    } else if (strcmp(name, "ps_iv") == 0 && v >= 0x12 && v <= 0x1000 && !(v & 1) && (unsigned)v >= g_tune.ps_win) {
        g_tune.ps_iv = (unsigned)v;
        if (h) h->ps_check_at = 0;
    } else if (strcmp(name, "ps_win") == 0 && v >= 0x11 && v <= 0x1000 && (unsigned)v <= g_tune.ps_iv) {
        g_tune.ps_win = (unsigned)v;
        if (h) h->ps_check_at = 0;
    } else if (strcmp(name, "mask_conn") == 0 && (v == 0 || v == 1)) {
        unsigned char mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F };
        if (v) mask[0] = 0xF7;
        if (h && (h->dead || !hci_sync(h, OP_SET_EVENT_MASK, mask, 8))) {
            log_line("dialog: mask_conn %ld not confirmed by the controller", v);
            return 0;
        }
        g_tune.mask_conn = (int)v;
    } else if (strcmp(name, "inq_len") == 0 && v >= 1 && v <= 0x30) {
        g_tune.inq_len = (int)v;
    } else if (strcmp(name, "idle_reads") == 0 && v >= 0 && v <= HOST_EV_READS) {
        g_tune.idle_reads = (int)v;
    } else if (strcmp(name, "ev_len") == 0 && v >= 8 && v <= 1024) {
        g_tune.ev_len = (int)v;
        if (h && h->hci.ops->event_len) h->hci.ops->event_len(h->hci.ctx, (int)v);
    } else if (strcmp(name, "trace") == 0 && (v == 0 || v == 1)) {
        g_tune.trace = (int)v;
    } else {
        return 0;
    }
    log_line("dialog: %s set to %ld", name, v);
    return 1;
}

int host_tune_json(char *out, int n)
{
    return snprintf(out, (size_t)n,
                    "{\"ps_light\":%d,\"ps_iv\":%u,\"ps_win\":%u,\"mask_conn\":%d,\"inq_len\":%d,\"trace\":%d,\"idle_reads\":%d,"
                    "\"ev_len\":%d}",
                    g_tune.ps_light, g_tune.ps_iv, g_tune.ps_win, g_tune.mask_conn, g_tune.inq_len, g_tune.trace,
                    g_tune.idle_reads, g_tune.ev_len);
}

int host_raw_cmd(host_t *h, unsigned op, const unsigned char *p, int plen, unsigned char *reply, int *rlen)
{
    *rlen = 0;
    if (!host_admits(h) || plen < 0 || plen > 255 || (plen && !p)) return 0;
    log_hex("dialog: command", op, p, plen);
    h->cc_op = 0;
    hci_sync(h, op, p, plen);
    if (!host_admits(h) || h->cc_op != op || h->cc_len < 6) {
        log_line("dialog: command %04x: no Command Complete (see the log for a Command Status)", op);
        return 0;
    }
    *rlen = h->cc_len - 5;
    memcpy(reply, h->cc + 5, (size_t)*rlen);
    log_hex("dialog: reply", op, reply, *rlen);
    return 1;
}
