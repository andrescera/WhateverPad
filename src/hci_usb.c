#include "evstream.h"
#include "attrib.h"
#include "probe.h"
#include "hci_usb.h"
#include "log.h"
#include "usb_desc.h"
#include "util.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Where the Bluetooth chip may be: ugen0.2 on every console seen so far
 * (MediaTek and Marvell/NXP alike); the others as programs that run on
 * both chips also look. */
static const char *const k_nodes[] = { "/dev/ugen0.2", "/dev/ugen0.3", "/dev/ugen1.2", "/dev/ugen0.1" };
#define NODE        (g_ctx.node[0] ? g_ctx.node : k_nodes[0])

/* The endpoints are read from the chip's configuration descriptor. These are
 * a layout used only if the descriptor cannot be read: on a PS5 fat with
 * firmware 10.01 it is wrong (events come on
 * 0x81 and ACL on 0x82), which is why the descriptor is trusted instead. */
#define EP_EVENTS_DEFAULT   0x82
#define EP_ACL_IN_DEFAULT   0x81
#define EP_ACL_OUT_DEFAULT  0x01
#define MPS_EVENTS_DEFAULT  16

/* Reads kept pending to win packets from the system's driver. The kernel
 * caps the transfers one process may have open (around 65), so events get
 * buffers sized for the largest event and ACL reads take the rest. */
#define N_EVENT_RD  40
#define N_ACL_RD    20
#define ACL_BUF     HCI_PKT_MAX

#define X_ACL_OUT   (N_EVENT_RD + N_ACL_RD)
#define X_ACL_OUT2  (X_ACL_OUT + 1)     /* the chip's second bulk OUT, tried if the first never finishes */
#define X_CMD       (X_ACL_OUT2 + 1)
#define N_XFER      (X_CMD + 1)

#define RING        64
_Static_assert(OWN_ACL_MAX == HCI_PKT_MAX, "ACL capability packet size");

typedef struct {
    unsigned char pkt[RING][HCI_PKT_MAX];
    int len[RING];
    int head, count;
} ring;

typedef struct {
    int fd;
    int initialized;
    struct usb_fs_endpoint ep[N_XFER];
    void *bufp[N_XFER][2];
    uint32_t lenp[N_XFER][2];
    struct usb_device_request cmd_request;
    own_ledger *ledger;
    void (*service)(void *);
    void *service_ud;
    unsigned char buf[N_XFER][ACL_BUF];
    unsigned char busy[N_XFER];
    unsigned char opened[N_XFER];
    int n_event, n_acl;             /* reads actually opened */
    long t_topup;                   /* when the missing reads were last tried again */
    int dead;                       /* the device went away */
    ring events, acl;

    /* The HCI function in use and its endpoints. */
    int iface;                      /* its interface number: where commands go */
    uint8_t ep_events, ep_acl_in, ep_acl_out, ep_acl_out2;
    int out_idx;                    /* the OUT transfer in use: X_ACL_OUT or X_ACL_OUT2 */
    int out2_open;
    int out_done_logged;
    int ev_read_len;                /* one packet of the events endpoint */
    int n_functions;                /* how many the chip has, for the log */
    evstream es;
    long yield_until;               /* event reads left to the system until then */
    int ev_want;                    /* event reads to keep pending (dialog mode: fewer when idle) */
    int ev_len;                     /* length asked of each event read (dialog mode) */

    char node[16];                  /* the device node in use */

    /* What was seen, for diag(). */
    struct usb_device_descriptor dd;
    int dd_ok;
    unsigned reads_done, reads_empty, status_hist[48];
    unsigned events_seen, acl_seen;
#if !WHATEVERPAD_PROBE
    unsigned discovery_dropped;
#endif
#if WHATEVERPAD_PROBE
    unsigned probe_opens, probe_empty_completes;
    unsigned probe_event_completes[2], probe_acl_completes[2];
#endif
    int sample_len[12], n_sample, last_cmd_len;
    int first_idx, first_len, first_frames;
} usb_ctx;

static usb_ctx g_ctx;

#if WHATEVERPAD_PROBE
/* Observe lifecycle ioctls without changing the command/token submission seam. */
static int probe_ioctl(int fd, unsigned long request, void *arg)
{
    int r = ioctl(fd, request, arg), saved_errno = errno;
    const char *op = NULL;
    int index = -1, late = probe_deadline_ms && now_ms() >= probe_deadline_ms;

    if (request == USB_FS_START) { op = "start"; index = ((struct usb_fs_start *)arg)->ep_index; }
    if (request == USB_FS_STOP) { op = "stop"; index = ((struct usb_fs_stop *)arg)->ep_index; }
    if (request == USB_FS_UNINIT) op = "uninit";
    if (request == USB_FS_INIT) op = "init";
    if (request == USB_FS_COMPLETE) {
        op = "complete";
        if (r == 0) {
            index = ((struct usb_fs_complete *)arg)->ep_index;
            if (index < N_EVENT_RD) g_ctx.probe_event_completes[late]++;
            else if (index < X_ACL_OUT) g_ctx.probe_acl_completes[late]++;
        } else if (saved_errno == EAGAIN && g_ctx.probe_empty_completes++) {
            op = NULL; /* Aggregate ordinary empty polls; never flood away the receipt. */
        }
    }
    if (op)
        log_line("probe usb op=%s transfer=%d rc=%d errno=%d phase=%s mono_ms=%ld",
                 op, index, r, r < 0 ? saved_errno : 0, late ? "late" : "before", now_ms());
    errno = saved_errno;
    return r;
}
#undef ioctl
#define ioctl(fd, request, arg) probe_ioctl(fd, request, arg)
#endif

static void service(usb_ctx *c)
{
    if (c->service) c->service(c->service_ud);
    if (c->ledger) {
#if !WHATEVERPAD_PROBE
        attrib_watch(c->ledger, "deadline", NULL, 0, now_ms());
#endif
        own_tick(c->ledger, (uint64_t)now_ms());
#if !WHATEVERPAD_PROBE
        attrib_check(c->ledger);
#endif
        attrib_poll(now_ms());
    }
}

static int drain_allowed(usb_ctx *c, long deadline, unsigned completed)
{
    long now = now_ms();
    return completed < 64 && (!deadline || now < deadline) &&
           (!c->ledger || own_drain_allowed(c->ledger, (uint64_t)now, completed));
}

static int accepts(usb_ctx *c)
{
    own_summary s;
    if (c->fd < 0 || c->dead) return 0;
    if (!c->ledger) return 1;
    own_snapshot(c->ledger, &s);
    return (s.runtime.state == SESSION_STARTING || s.runtime.state == SESSION_ACTIVE) &&
           s.runtime.reason == SESSION_NONE && !s.runtime.cleanup_pending;
}

static void attribution_loss(usb_ctx *c)
{
    if (c->ledger) {
        own_ack a = {.overflow = 1};
        own_receipt receipt;
        own_backend_receipt(c->ledger, &a, (uint64_t)now_ms(), &receipt);
        own_receive(c->ledger, &receipt, (uint64_t)now_ms());
    }
}

/* ---- packet rings -------------------------------------------------------- */

#if !WHATEVERPAD_PROBE
static int discovery_event(unsigned code)
{
    return code == 0x02 || code == 0x22 || code == 0x2f;
}

static void discovery_dropped(void)
{
    unsigned n = ++g_ctx.discovery_dropped;
    if (!(n & (n - 1)))
        log_line("usb: discovery backlog dropped %u inquiry results; control events retained", n);
}
#endif

static void ring_put(ring *r, const unsigned char *p, int n)
{
    if (n > HCI_PKT_MAX) n = HCI_PKT_MAX;
    if (r->count == RING) {
        int discard = 0;
        if (r == &g_ctx.events) {
#if !WHATEVERPAD_PROBE
            /* Discovery may lose a result; never sacrifice a control event for it. */
            if (discovery_event(p[0])) { discovery_dropped(); return; }
            for (discard = 0; discard < RING; discard++)
                if (discovery_event(r->pkt[(r->head + discard) % RING][0])) break;
            if (discard < RING) discovery_dropped();
            else
#endif
            {
                discard = 0;
#if !WHATEVERPAD_PROBE
                attrib_watch(g_ctx.ledger, "event_ring", r->pkt[r->head],
                             r->len[r->head], now_ms());
#endif
                attrib_overflow();
                attribution_loss(&g_ctx);
#if !WHATEVERPAD_PROBE
                attrib_check(g_ctx.ledger);
#endif
            }
        }
        if (!discard) r->head = (r->head + 1) % RING;
        else for (int i = discard; i < RING - 1; i++) {
            int to = (r->head + i) % RING, from = (to + 1) % RING;
            r->len[to] = r->len[from];
            memcpy(r->pkt[to], r->pkt[from], (size_t)r->len[from]);
        }
        r->count--;
    }
    {
        int slot = (r->head + r->count) % RING;
        memcpy(r->pkt[slot], p, (size_t)n);
        r->len[slot] = n;
        r->count++;
    }
}

static int ring_get(ring *r, unsigned char *out, int max)
{
    int n;

    if (!r->count) return 0;
    n = r->len[r->head] < max ? r->len[r->head] : max;
    memcpy(out, r->pkt[r->head], (size_t)n);
    r->head = (r->head + 1) % RING;
    r->count--;
    return n;
}

/* ---- transfers ----------------------------------------------------------- */

static int xfer_open_quiet(usb_ctx *c, int index, int ep_addr, int size)
{
    struct usb_fs_open op;

    memset(&op, 0, sizeof op);
    op.ep_index = (uint8_t)index;
    op.ep_no = (uint8_t)ep_addr;
    op.max_bufsize = (uint32_t)size;
    op.max_frames = 1;
    if (ioctl(c->fd, USB_FS_OPEN, &op) != 0) return 0;
    c->opened[index] = 1;
    return 1;
}

static int xfer_open(usb_ctx *c, int index, int ep_addr, int size)
{
    struct usb_fs_open op;

    memset(&op, 0, sizeof op);
    op.ep_index = (uint8_t)index;
    op.ep_no = (uint8_t)ep_addr;
    op.max_bufsize = (uint32_t)size;
    op.max_frames = index == X_CMD ? 2 : 1;
    if (ioctl(c->fd, USB_FS_OPEN, &op) != 0) {
        log_line("usb: open endpoint %#x as transfer %d: errno %d", ep_addr, index, errno);
        return 0;
    }
    c->opened[index] = 1;
    return 1;
}

static int xfer_start(usb_ctx *c, int index, int length)
{
    struct usb_fs_start st;

    if (!c->initialized || !c->opened[index] || c->busy[index]) return 0;
    c->lenp[index][0] = (uint32_t)length;
    c->ep[index].nFrames = 1;
    c->ep[index].aFrames = 0;
    c->ep[index].status = 0;
    memset(&st, 0, sizeof st);
    st.ep_index = (uint8_t)index;
    if (ioctl(c->fd, USB_FS_START, &st) != 0) {
        log_line("usb: start transfer %d: errno %d", index, errno);
        if (errno == ENXIO || errno == ENODEV || errno == EIO || errno == EBADF) c->dead = 1;
        return 0;
    }
    c->busy[index] = 1;
    return 1;
}

/* A finished read ends on a short packet, so it holds whole HCI packets
 * unless the system's read took the start of one: a trailing part that
 * does not add up is dropped. */
static void event_done(void *ctx, const uint8_t *ev, int len)
{
    usb_ctx *c = ctx;

#if !WHATEVERPAD_PROBE
    attrib_event_seen(ev, len, now_ms());
#endif
    int retired = 0;
    if (c->ledger) {
        long now = now_ms();
#if !WHATEVERPAD_PROBE
        attrib_watch(c->ledger, "event", ev, len, now);
#endif
        if (ev[0] != OWN_CC && ev[0] != OWN_CS) attrib_observe(ev, len, now);
        /* Failure events do not provide a usable connection identity tuple. */
        if (ev[0] == 3 && len == 13 && ev[2] == 0 && ev[11] == 1)
            own_observe_connection(c->ledger, 0, 0, ev + 5, (uint16_t)le16(ev + 3),
                                   ev[2], (uint64_t)now);
        if (ev[0] == 0x3e && len >= 3 &&
            ((ev[2] == 1 && len == 21) || (ev[2] == 0x0a && len == 33)) && ev[3] == 0)
            own_observe_connection(c->ledger, 1, ev[7], ev + 8, (uint16_t)le16(ev + 4),
                                   ev[3], (uint64_t)now);
        if (ev[0] == 5) {
            if (len != 6 || le16(ev + 3) >= 0x0f00)
                own_detector_trip(c->ledger, (uint64_t)now);
            else if (!ev[2])
                own_observe_disconnect(c->ledger, (uint16_t)le16(ev + 3), (uint64_t)now);
        }
        if (ev[0] == OWN_CC || ev[0] == OWN_CS) {
            own_ack a = {.event = (own_ack_event)ev[0]};
            own_receipt receipt;
            if (ev[0] == OWN_CC && len == 5 && ev[1] == 3 && !le16(ev + 3)) {
                a.opcode = 0;
            } else if (len < 6 || len != 2 + ev[1] ||
                       (ev[0] == OWN_CS && ev[1] != 4)) {
                a.malformed = 1;
            } else {
                a.opcode = le16(ev + (ev[0] == OWN_CC ? 3 : 4));
                a.status = ev[ev[0] == OWN_CC ? 5 : 2];
                if (a.opcode == 0x2018 && ev[0] == OWN_CC) {
                    a.random_len = (uint8_t)(len - 6);
                    if (a.random_len == 8) memcpy(a.random, ev + 6, 8);
                }
            }
            own_provenance provenance = own_backend_receipt(c->ledger, &a, (uint64_t)now, &receipt);
            attrib_observe_receipt(ev, len, now, provenance);
            own_receive(c->ledger, &receipt, (uint64_t)now);
#if !WHATEVERPAD_PROBE
            retired = ev[0] == OWN_CS && provenance == OWN_RECEIPT_RETIRED;
#endif
        }
#if !WHATEVERPAD_PROBE
        attrib_check(c->ledger);
#endif
    }
    /* A Status the ledger retired answers nothing live. Queued, it would be taken for
     * the answer of whichever Inquiry is current when the host reaches it. */
    if (retired) return;
    c->events_seen++;
    if (c->n_sample < 12) {
        c->sample_len[c->n_sample++] = len;
    }
    ring_put(&c->events, ev, len);
}

static void split_read(usb_ctx *c, int index)
{
    const unsigned char *p = c->buf[index];
    int n = (int)c->lenp[index][0], off = 0;

    if (index < N_EVENT_RD) {           /* a piece of the event stream */
#if !WHATEVERPAD_PROBE
        long now = now_ms();
        /* Revoke before new bytes can answer anything; the old partial header
         * is lost evidence, not the replacement event about to be parsed. */
        if (c->es.len && now - c->es.t_last > EVSTREAM_TIMEOUT && n > 0) {
            attrib_watch(c->ledger, "stream_gap", c->es.buf, c->es.len, now);
            attrib_overflow();
            attribution_loss(c);
            attrib_check(c->ledger);
        }
        evstream_feed(&c->es, p, n, now, event_done, c);
#else
        unsigned dropped = c->es.dropped;
        evstream_feed(&c->es, p, n, now_ms(), event_done, c);
        if (c->es.dropped != dropped) {
            attrib_overflow();
            attribution_loss(c);
        }
#endif
        return;
    }
    while (off + 4 <= n) {              /* ACL: whole packets */
        int total = 4 + (int)le16(p + off + 2);
        if (off + total > n) break;
        c->acl_seen++;
        ring_put(&c->acl, p + off, total);
        off += total;
    }
}

static int reap(usb_ctx *c, long deadline, unsigned *completed)
{
    struct usb_fs_complete done;
    int any = 0;

    while (*completed < 64) {
        service(c);
        if (!drain_allowed(c, deadline, *completed)) break;
        memset(&done, 0, sizeof done);
        if (ioctl(c->fd, USB_FS_COMPLETE, &done) != 0) break;
        (*completed)++;
        any = 1;
        if (done.ep_index >= N_XFER) continue;
        c->busy[done.ep_index] = 0;
        if (done.ep_index == X_CMD) {
            if (c->ep[X_CMD].status) {
                c->dead = 1;
                if (c->ledger) {
#if !WHATEVERPAD_PROBE
                    attrib_watch(c->ledger, "dispatch", NULL, 0, now_ms());
#endif
                    own_detector_trip(c->ledger, (uint64_t)now_ms());
#if !WHATEVERPAD_PROBE
                    attrib_check(c->ledger);
#endif
                }
            }
            continue;
        }
        if (done.ep_index == X_ACL_OUT || done.ep_index == X_ACL_OUT2) {
            if (c->ep[done.ep_index].status) {
                log_line("usb: ACL out status %u", (unsigned)c->ep[done.ep_index].status);
                if (c->ledger) {
                    c->dead = 1;
#if !WHATEVERPAD_PROBE
                    attrib_watch(c->ledger, "dispatch", NULL, 0, now_ms());
#endif
                    own_detector_trip(c->ledger, (uint64_t)now_ms());
#if !WHATEVERPAD_PROBE
                    attrib_check(c->ledger);
#endif
                }
            } else if (!c->out_done_logged) {
                c->out_done_logged = 1;
                log_line("usb: first ACL packet written on endpoint %#x",
                         done.ep_index == X_ACL_OUT ? c->ep_acl_out : c->ep_acl_out2);
            }
            continue;
        }
        if (c->reads_done == 0) {
            int n = (int)c->lenp[done.ep_index][0];
            c->first_idx = done.ep_index;
            c->first_len = n;
            c->first_frames = c->ep[done.ep_index].aFrames;
        }
        c->reads_done++;
        {
            unsigned st = (unsigned)c->ep[done.ep_index].status;
            c->status_hist[st < 47 ? st : 47]++;
        }
        if (c->ep[done.ep_index].status == 0 && c->ep[done.ep_index].aFrames) split_read(c, done.ep_index);
        else c->reads_empty++;
    }
    return any;
}

/* If the controller was opened with fewer reads than wanted (another payload
 * still held some of its transfers), the missing ones are asked for again
 * every few seconds: when the other one lets go, this one recovers by itself
 * instead of running thin until it is restarted. */
static void topup_reads(usb_ctx *c)
{
    long now = now_ms();
    int before_e = c->n_event, before_a = c->n_acl;

    if (c->n_event >= N_EVENT_RD && c->n_acl >= N_ACL_RD) return;
    if (now - c->t_topup < 5000) return;
    c->t_topup = now;
    while (c->n_event < N_EVENT_RD && xfer_open_quiet(c, c->n_event, c->ep_events, ACL_BUF)) c->n_event++;
    while (c->n_acl < N_ACL_RD && xfer_open_quiet(c, N_EVENT_RD + c->n_acl, c->ep_acl_in, ACL_BUF)) c->n_acl++;
    if (c->n_event != before_e || c->n_acl != before_a)
        log_line("usb: more reads opened: %d event and %d ACL (wanted %d and %d)", c->n_event, c->n_acl,
                 N_EVENT_RD, N_ACL_RD);
}

static int op_missing_reads(void *ctx)
{
    usb_ctx *c = ctx;

    return (N_EVENT_RD - c->n_event) + (N_ACL_RD - c->n_acl);
}

static void keep_reads_pending(usb_ctx *c)
{
    int i;

    if (!accepts(c)) {
#if WHATEVERPAD_PROBE
        own_summary s;
        if (!c->ledger || c->fd < 0 || c->dead || !probe_deadline_ms ||
            now_ms() < probe_deadline_ms ||
            now_ms() > probe_deadline_ms + PROBE_TAIL_MS) return;
        own_snapshot(c->ledger, &s);
        /* Read-only tail after ambiguity; command commit still rejects every
         * token, and backend receipts never regain sequenced provenance. */
        if (s.runtime.reason != SESSION_AMBIGUOUS) return;
#else
        return;
#endif
    }
    if (now_ms() >= c->yield_until) topup_reads(c);
    for (i = 0; i < c->n_event && i < c->ev_want && now_ms() >= c->yield_until; i++)
        if (!c->busy[i]) xfer_start(c, i, c->ev_len);
    for (i = 0; i < c->n_acl; i++)
        if (!c->busy[N_EVENT_RD + i]) xfer_start(c, N_EVENT_RD + i, ACL_BUF);
}

/* ---- hci_ops ------------------------------------------------------------- */

static int command_start(usb_ctx *c, unsigned opcode, const void *params, int plen,
                         uint64_t ack_by)
{
    struct usb_fs_start start = {.ep_index = X_CMD};
    unsigned char *pkt = c->buf[X_CMD];

    put16(pkt, opcode);
    pkt[2] = (unsigned char)plen;
    if (plen) memcpy(pkt + 3, params, (size_t)plen);
    c->last_cmd_len = 3 + plen;

    memset(&c->cmd_request, 0, sizeof c->cmd_request);
    c->cmd_request.bmRequestType = UT_WRITE_CLASS_DEVICE;
    USETW(c->cmd_request.wIndex, c->iface);
    USETW(c->cmd_request.wLength, 3 + plen);
    c->bufp[X_CMD][0] = &c->cmd_request;
    c->bufp[X_CMD][1] = pkt;
    c->lenp[X_CMD][0] = sizeof c->cmd_request;
    c->lenp[X_CMD][1] = (uint32_t)(3 + plen);
    c->ep[X_CMD].nFrames = 2;
    c->ep[X_CMD].aFrames = 0;
    c->ep[X_CMD].timeout = 1000;
    if (c->ledger) {
        own_summary s;
        own_snapshot(c->ledger, &s);
        c->ep[X_CMD].timeout = (uint16_t)session_work_remaining(&s.runtime,
                                                              (uint64_t)now_ms(), 1000);
    }
    if (ack_by) {
        uint64_t now = (uint64_t)now_ms();
        if (now >= ack_by) return 0;
        if (ack_by - now < c->ep[X_CMD].timeout)
            c->ep[X_CMD].timeout = (uint16_t)(ack_by - now);
    }
    if (!c->ep[X_CMD].timeout) return 0;
    c->ep[X_CMD].status = 0;
    if (ioctl(c->fd, USB_FS_START, &start) != 0) {
        log_line("usb: command %#06x: errno %d", opcode, errno);
        if (errno == ENXIO || errno == ENODEV || errno == EIO || errno == EBADF) c->dead = 1;
        return 0;
    }
    c->busy[X_CMD] = 1;
    return 1;
}

static int op_cmd(void *ctx, unsigned opcode, const void *params, int plen)
{
    usb_ctx *c = ctx;
    unsigned completed = 0;
    if (c->ledger || !accepts(c) || plen < 0 || plen > 255 || (plen && !params) ||
        opcode > 0xffff || opcode == 0x0402 || opcode == 0x0408 ||
        opcode == 0x200e || opcode == 0x0c03) return 0;
    reap(c, now_ms() + 1000, &completed);
    if (!accepts(c) || c->busy[X_CMD]) return 0;
    return command_start(c, opcode, params, plen, 0);
}

static own_submit_result submit_command(usb_ctx *c, const own_token *token)
{
    own_effect effect;
    uint64_t ack_by;
    unsigned completed = 0;
    long deadline = now_ms() + 1000;
    int sent;
    if (!c->ledger || !token || c->fd < 0 || c->dead) return OWN_NOT_SENT;
#ifdef USB_MUTATE_NO_FINAL_CHECK
    if (own_commit_hci(c->ledger, token, (uint64_t)now_ms(), &effect, &ack_by) != OWN_OK)
        return OWN_NOT_SENT;
#endif
    reap(c, deadline, &completed);
    if (c->dead || c->busy[X_CMD] || now_ms() >= deadline) return OWN_NOT_SENT;
#ifndef USB_MUTATE_NO_FINAL_CHECK
    if (own_commit_hci(c->ledger, token, (uint64_t)now_ms(), &effect, &ack_by) != OWN_OK)
        return OWN_NOT_SENT;
#endif
    /* No callback between the consuming commit and USB_FS_START. */
    sent = command_start(c, effect.opcode, effect.params, effect.plen, ack_by);
    own_submitted(c->ledger, token, sent ? OWN_SENT : OWN_NOT_SENT, (uint64_t)now_ms());
    if (sent) {
        if (!effect.resend) attrib_submit(effect.opcode, 1, now_ms());
        else attrib_resend(effect.opcode);
        attrib_deadline(effect.opcode, ack_by > LONG_MAX ? LONG_MAX : (long)ack_by);
        if (effect.resend) log_line("setup resend opcode=0x%04x try=%u", effect.opcode, effect.resend + 1);
    }
    return sent ? OWN_SENT : OWN_NOT_SENT;
}

static own_submit_result submit_acl(usb_ctx *c, own_acl_lane *lane,
                                    const own_acl_token *token)
{
    own_acl_effect effect;
    unsigned completed = 0;
    long deadline = now_ms() + 500;
    int sent;
    if (!c->ledger || !lane || !token || c->fd < 0 || c->dead) return OWN_NOT_SENT;
#ifdef USB_MUTATE_NO_FINAL_CHECK
    if (own_acl_commit(c->ledger, lane, token, (uint64_t)now_ms(), &effect) != OWN_OK)
        return OWN_NOT_SENT;
#endif
    do {
        reap(c, deadline, &completed);
        if (c->dead || now_ms() >= deadline || completed == 64) return OWN_NOT_SENT;
#ifndef USB_MUTATE_NO_FINAL_CHECK
        if (!accepts(c)) return OWN_NOT_SENT;
#endif
        if (!c->busy[c->out_idx]) break;
        usleep(500);
    } while (c->busy[c->out_idx]);
#ifndef USB_MUTATE_NO_FINAL_CHECK
    if (own_acl_commit(c->ledger, lane, token, (uint64_t)now_ms(), &effect) != OWN_OK)
        return OWN_NOT_SENT;
#endif
    /* Immutable bytes and the link lease are checked after every callback. */
    memcpy(c->buf[c->out_idx], effect.packet, effect.len);
    sent = xfer_start(c, c->out_idx, effect.len);
    own_acl_submitted(lane, token, sent ? OWN_SENT : OWN_NOT_SENT, (uint64_t)now_ms());
    return sent ? OWN_SENT : OWN_NOT_SENT;
}

static own_submit_result op_submit(void *ctx, const hci_submission *submission)
{
    if (!submission) return OWN_NOT_SENT;
    switch (submission->kind) {
    case HCI_SUBMIT_COMMAND:
        return submit_command(ctx, submission->token.command);
    case HCI_SUBMIT_ACL:
        return submit_acl(ctx, submission->token.acl.lane, submission->token.acl.token);
    }
    return OWN_NOT_SENT;
}

static int op_acl_send(void *ctx, const unsigned char *pkt, int len)
{
    usb_ctx *c = ctx;
    long deadline = now_ms() + 500;
    unsigned completed = 0;

    if (c->ledger || !accepts(c) || !pkt || len < 4 || len > ACL_BUF ||
        len != 4 + (int)le16(pkt + 2)) return 0;
    while (c->busy[c->out_idx]) {
        reap(c, deadline, &completed);
        if (!c->busy[c->out_idx]) break;
        if (now_ms() >= deadline || completed == 64) {
            /* The write never finished: the chip may take ACL on its other
             * bulk OUT endpoint. Try it once, and say so. */
            if (c->out_idx == X_ACL_OUT && c->out2_open) {
                struct usb_fs_stop st;

                memset(&st, 0, sizeof st);
                st.ep_index = (uint8_t)X_ACL_OUT;
                ioctl(c->fd, USB_FS_STOP, &st);
                c->busy[X_ACL_OUT] = 0;
                c->out_idx = X_ACL_OUT2;
                log_line("usb: ACL out %#x never finished; switching to %#x", c->ep_acl_out, c->ep_acl_out2);
                break;
            }
            log_line("usb: ACL out still busy");
            return 0;
        }
        usleep(500);
    }
    memcpy(c->buf[c->out_idx], pkt, (size_t)len);
    return xfer_start(c, c->out_idx, len);
}

static int op_pump(void *ctx, int timeout_ms)
{
    usb_ctx *c = ctx;
    long deadline = now_ms() + timeout_ms;
    long previous = now_ms();
    unsigned completed = 0, stalled = 0;
    int first = 1;

    for (;;) {
        int any;

        if (c->dead || c->fd < 0) return -1;
        service(c);
        if (!drain_allowed(c, first ? 0 : deadline, completed)) return 0;
        keep_reads_pending(c);
        any = reap(c, timeout_ms > 0 ? deadline : 0, &completed);
        if (c->events.count || c->acl.count) return 1;
        if (c->dead) return -1;
        if (now_ms() >= deadline || !drain_allowed(c, 0, completed)) return 0;
        long now = now_ms();
        if (now > previous) stalled = 0;
        else if (++stalled == 64) {
            c->dead = 1; /* A stalled clock must not hold Stop or a query wait. */
            return -1;
        }
        previous = now;
        first = 0;
        if (any) continue;
        usleep(1000);
    }
}

static void op_event_len(void *ctx, int n)
{
    usb_ctx *c = ctx;
    struct usb_fs_stop st;
    int i;
    unsigned completed = 0;

    if (n < 8) n = 8;
    if (n > ACL_BUF) n = ACL_BUF;
    if (n == c->ev_len) return;
    c->ev_len = n;
    for (i = 0; i < c->n_event; i++) {         /* started again with the new length */
        if (!c->busy[i]) continue;
        memset(&st, 0, sizeof st);
        st.ep_index = (uint8_t)i;
        ioctl(c->fd, USB_FS_STOP, &st);
    }
    reap(c, now_ms() + 1000, &completed);
    log_line("usb: event reads of %d bytes", n);
}

static void op_event_reads(void *ctx, int n)
{
    usb_ctx *c = ctx;
    struct usb_fs_stop st;
    int i;
    unsigned completed = 0;

    if (n < 0) n = 0;
    if (n > N_EVENT_RD) n = N_EVENT_RD;
    if (n == c->ev_want) return;
    c->ev_want = n;
    for (i = n; i < c->n_event; i++) {
        if (!c->busy[i]) continue;
        memset(&st, 0, sizeof st);
        st.ep_index = (uint8_t)i;
        ioctl(c->fd, USB_FS_STOP, &st);
    }
    reap(c, now_ms() + 1000, &completed);
}

/* Leaves the events to the system's driver for a while: our pending event
 * reads are stopped and not renewed until then, so the system's own read is
 * the only one in line (a DualSense reconnecting to the console pages this
 * controller, and only the system can answer it). */
static void op_yield(void *ctx, int ms)
{
    usb_ctx *c = ctx;
    struct usb_fs_stop st;
    int i;
    unsigned completed = 0;

    c->yield_until = now_ms() + ms;
    for (i = 0; i < c->n_event; i++) {
        if (!c->busy[i]) continue;
        memset(&st, 0, sizeof st);
        st.ep_index = (uint8_t)i;
        ioctl(c->fd, USB_FS_STOP, &st);
    }
    reap(c, now_ms() + 1000, &completed);
    log_line("usb: event reads left to the system for %d ms", ms);
}

static int op_next_event(void *ctx, unsigned char *out, int max)
{
    return ring_get(&((usb_ctx *)ctx)->events, out, max);
}

static int op_next_acl(void *ctx, unsigned char *out, int max)
{
    return ring_get(&((usb_ctx *)ctx)->acl, out, max);
}

static void op_close(void *ctx)
{
    usb_ctx *c = ctx;
    struct usb_fs_uninit un;

    if (c->fd < 0) return;
    {
        /* Stop every pending transfer first, so that releasing them cannot
         * wait on reads that would only finish with more traffic. */
        struct usb_fs_stop st;
        int i;
        for (i = 0; i < N_XFER; i++) {
            if (!c->opened[i] || !c->busy[i]) continue;
            c->busy[i] = 0;
            memset(&st, 0, sizeof st);
            st.ep_index = (uint8_t)i;
            ioctl(c->fd, USB_FS_STOP, &st);
        }
    }
    if (c->initialized) {
        c->initialized = 0;
        memset(&un, 0, sizeof un);
        ioctl(c->fd, USB_FS_UNINIT, &un);
    }
#if WHATEVERPAD_PROBE
    {
        int r = close(c->fd), saved_errno = errno;
        log_line("probe close rc=%d errno=%d mono_ms=%ld node_opens=%u event_before=%u event_late=%u acl_before=%u acl_late=%u empty_completes=%u",
                 r, r < 0 ? saved_errno : 0, now_ms(), c->probe_opens,
                 c->probe_event_completes[0], c->probe_event_completes[1],
                 c->probe_acl_completes[0], c->probe_acl_completes[1], c->probe_empty_completes);
        errno = saved_errno;
    }
#else
    close(c->fd);
#endif
    c->fd = -1;
    if (c->ledger) own_transport_closed(c->ledger, (uint64_t)now_ms());
    c->ledger = NULL;
    c->service = NULL;
    c->service_ud = NULL;
}

/* Why the controller may not be answering: what the device is, whether
 * anything at all came in, in which state the transfers finished, and the
 * first events the system's own traffic put in front of us. */
static void op_diag(void *ctx)
{
    usb_ctx *c = ctx;
    unsigned i;
    int j;

    if (c->dd_ok)
        log_line("diag: device %04x:%04x class %02x/%02x/%02x, USB %04x, %d endpoint-0 bytes",
                 UGETW(c->dd.idVendor), UGETW(c->dd.idProduct), c->dd.bDeviceClass,
                 c->dd.bDeviceSubClass, c->dd.bDeviceProtocol, UGETW(c->dd.bcdUSB),
                 c->dd.bMaxPacketSize);
    else
        log_line("diag: the device descriptor could not be read");
    log_line("diag: %u reads finished (%u empty or failed); %u events and %u ACL packets seen",
             c->reads_done, c->reads_empty, c->events_seen, c->acl_seen);
    if (c->reads_done) {
        log_line("diag: first read transfer %d, %d bytes in %d frame(s)",
                 c->first_idx, c->first_len, c->first_frames);
        log_line("diag: using interface %d: events %#x, ACL in %#x, ACL out %#x (%d HCI function(s) in the chip)",
                 c->iface, c->ep_events, c->ep_acl_in, c->ep_acl_out, c->n_functions);
    }
    for (i = 0; i < 48; i++)
        if (c->status_hist[i]) log_line("diag: %u reads ended with USB status %u", c->status_hist[i], i);
    for (j = 0; j < c->n_sample; j++) {
        log_line("diag: event %d: len %d", j + 1, c->sample_len[j]);
    }
    if (c->last_cmd_len) log_line("diag: last command sent: len %d", c->last_cmd_len);

    /* Read-only questions about the device: what it calls itself, its whole
     * configuration (every interface and endpoint), and which kernel driver
     * has each interface. This is what shows how this firmware lays the chip
     * out. Nothing here sends anything to it or takes anything from it. */
    {
        struct usb_device_info di;
        struct usb_gen_descriptor gd;
        unsigned char cfg[512];
        char name[64];

        memset(&di, 0, sizeof di);
        if (ioctl(c->fd, USB_GET_DEVICEINFO, &di) == 0)
            log_line("diag: \"%.40s\" by \"%.40s\", bus %u address %u, config %u, speed %u, power %u mA, suspended %u",
                     di.udi_product, di.udi_vendor, di.udi_bus, di.udi_addr, di.udi_config_no,
                     di.udi_speed, di.udi_power, di.udi_suspended);

        memset(&gd, 0, sizeof gd);
        gd.ugd_data = cfg;
        gd.ugd_maxlen = sizeof cfg;
        gd.ugd_config_index = 0xFF;                 /* the current configuration */
        if (ioctl(c->fd, USB_GET_FULL_DESC, &gd) == 0) {
            int total = gd.ugd_actlen < (int)sizeof cfg ? gd.ugd_actlen : (int)sizeof cfg;
            log_line("diag: configuration descriptor, %d bytes", total);
        } else {
            log_line("diag: the configuration descriptor could not be read (errno %d)", errno);
        }

        for (j = 0; j < 6; j++) {
            memset(&gd, 0, sizeof gd);
            memset(name, 0, sizeof name);
            gd.ugd_data = name;
            gd.ugd_maxlen = sizeof name - 1;
            gd.ugd_iface_index = (uint8_t)j;
            if (ioctl(c->fd, USB_GET_IFACE_DRIVER, &gd) == 0)
                log_line("diag: interface %d is held by driver \"%.40s\"", j, name);
        }
    }
}

/* The HCI function and endpoints to use, from the chip's own descriptor: the
 * first function in descriptor order. (The second one is where the system
 * runs the DualSense; the first was silent in every trial.) */
static int choose_endpoints(usb_ctx *c)
{
    struct usb_gen_descriptor gd;
    unsigned char cfg[512];
    hci_function f[HCI_FUNCTIONS_MAX];
    int n = 0, i;

    memset(&gd, 0, sizeof gd);
    gd.ugd_data = cfg;
    gd.ugd_maxlen = sizeof cfg;
    gd.ugd_config_index = 0xFF;                 /* the current configuration */
    if (ioctl(c->fd, USB_GET_FULL_DESC, &gd) == 0)
        n = usb_find_hci_functions(cfg, gd.ugd_actlen < (int)sizeof cfg ? gd.ugd_actlen : (int)sizeof cfg, f);
    c->n_functions = n;
    if (n > 0) {
        for (i = 0; i < n; i++)
            log_line("usb: HCI function %d: interface %d, events %#x (%u bytes), ACL in %#x, ACL out %#x",
                     i, f[i].iface, f[i].ep_events, f[i].mps_events, f[i].ep_acl_in, f[i].ep_acl_out);
        c->iface = f[0].iface;
        c->ep_events = f[0].ep_events;
        c->ep_acl_in = f[0].ep_acl_in;
        c->ep_acl_out = f[0].ep_acl_out;
        c->ep_acl_out2 = usb_spare_acl_out(&f[0]);
        c->ev_read_len = f[0].mps_events >= 8 && f[0].mps_events <= 64 ? f[0].mps_events : MPS_EVENTS_DEFAULT;
        return n;
    }
    return 0;
}

static void default_endpoints(usb_ctx *c)
{
    log_line("usb: no HCI function found in the descriptor; using the default layout");
    c->iface = 0;
    c->ep_events = EP_EVENTS_DEFAULT;
    c->ep_acl_in = EP_ACL_IN_DEFAULT;
    c->ep_acl_out = EP_ACL_OUT_DEFAULT;
    c->ev_read_len = MPS_EVENTS_DEFAULT;
}

static int op_chip_vendor(void *ctx);
static int op_chip_product(void *ctx);

static const hci_ops k_usb_ops = {
    op_cmd, op_acl_send, op_pump, op_next_event, op_next_acl, op_close, op_diag, op_yield, op_missing_reads,
    op_event_reads, op_event_len, op_chip_vendor, op_chip_product,
    op_submit,
};

/* Opens one device node and reads its device descriptor. */
static int open_node(usb_ctx *c, const char *path, int log_failure)
{
    snprintf(c->node, sizeof c->node, "%s", path);
    c->fd = open(path, O_RDWR);
#if WHATEVERPAD_PROBE
    {
        int saved_errno = errno;
        log_line("probe open node=%s attempts=%u rc=%d errno=%d mono_ms=%ld",
                 path, ++c->probe_opens, c->fd >= 0 ? 0 : -1,
                 c->fd < 0 ? saved_errno : 0, now_ms());
        errno = saved_errno;
    }
#endif
    if (c->fd < 0) {
        if (log_failure) log_line("usb: open %s: errno %d", path, errno);
        return 0;
    }
    c->dd_ok = ioctl(c->fd, USB_GET_DEVICE_DESC, &c->dd) == 0;
    if (c->dd_ok)
        log_line("usb: %s is %04x:%04x", path, UGETW(c->dd.idVendor), UGETW(c->dd.idProduct));
    return 1;
}

static int g_chip_vendor;
static char g_chip_name[40];

/* Which chip this is: it decides what is safe to change on it. */
static void log_chip(usb_ctx *c)
{
    unsigned vid = UGETW(c->dd.idVendor);

    g_chip_vendor = (int)vid;
    snprintf(g_chip_name, sizeof g_chip_name, "%s (%04x:%04x)", vid == 0x0E8D ? "MediaTek" : vid == 0x1286 ? "Marvell/NXP" : "USB",
             vid, UGETW(c->dd.idProduct));
    if (vid == 0x0E8D)
        log_line("chip: MediaTek (%04x:%04x)", vid, UGETW(c->dd.idProduct));
    else if (vid == 0x1286)
        log_line("chip: Marvell/NXP (%04x:%04x): the system's page scan is left as it is", vid, UGETW(c->dd.idProduct));
    else
        log_line("chip: %04x:%04x, not seen before", vid, UGETW(c->dd.idProduct));
}

int hci_usb_chip_vendor(void) { return g_chip_vendor; }
const char *hci_usb_chip_name(void) { return g_chip_name; }

static int op_chip_vendor(void *ctx) { (void)ctx; return g_chip_vendor; }
static int op_chip_product(void *ctx)
{
    usb_ctx *c = ctx;
    return c->dd_ok ? UGETW(c->dd.idProduct) : 0;
}

int hci_usb_open(hci_t *out)
{
    usb_ctx *c = &g_ctx;
    struct usb_fs_init init;
    int i;
    size_t k;

    memset(c, 0, sizeof *c);
    c->fd = -1;
    /* The first node that is a Bluetooth chip (an HCI function in its
     * descriptor); if none is, ugen0.2 with the default layout, as before. */
    for (k = 0; k < sizeof k_nodes / sizeof k_nodes[0] && c->fd < 0; k++) {
        if (!open_node(c, k_nodes[k], k == 0)) continue;
        if (choose_endpoints(c) > 0) break;
        close(c->fd);
        c->fd = -1;
    }
    if (c->fd < 0) {
        if (!open_node(c, k_nodes[0], 1)) return 0;
        default_endpoints(c);
    }
#if WHATEVERPAD_PROBE
    log_line("probe selection function=%d interface=%d events=0x%02x acl_in=0x%02x acl_out=0x%02x",
             c->n_functions ? 0 : -1, c->iface, c->ep_events, c->ep_acl_in, c->ep_acl_out);
#endif
    if (c->dd_ok) log_chip(c);
    evstream_init(&c->es);

    for (i = 0; i < N_XFER; i++) {
        c->bufp[i][0] = c->buf[i];
        c->lenp[i][0] = ACL_BUF;
        c->ep[i].ppBuffer = c->bufp[i];
        c->ep[i].pLength = c->lenp[i];
        c->ep[i].nFrames = 1;
        c->ep[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    }
    /* A bulk OUT transfer that is a multiple of the packet size must end
     * with a zero-length packet. */
    c->ep[X_ACL_OUT].flags = USB_FS_FLAG_FORCE_SHORT;
    c->ep[X_ACL_OUT2].flags = USB_FS_FLAG_FORCE_SHORT;
    c->out_idx = X_ACL_OUT;

    memset(&init, 0, sizeof init);
    init.pEndpoints = c->ep;
    init.ep_index_max = N_XFER;
    if (ioctl(c->fd, USB_FS_INIT, &init) != 0) {
        log_line("usb: init: errno %d", errno);
        op_close(c);
        return 0;
    }
    c->initialized = 1;

    if (!xfer_open(c, X_CMD, 0, ACL_BUF)) {
        op_close(c);
        return 0;
    }
    /* The OUT transfer first: if the kernel's budget runs out, it must not
     * be the one left without a transfer. */
    if (!xfer_open(c, X_ACL_OUT, c->ep_acl_out, ACL_BUF)) {
        op_close(c);
        return 0;
    }
    /* The spare OUT is opened only if it is there; its absence is no failure. */
    if (c->ep_acl_out2) c->out2_open = xfer_open(c, X_ACL_OUT2, c->ep_acl_out2, ACL_BUF);
    /* On this chip (MediaTek 0e8d:3603) the bulk OUT the descriptor names
     * first (0x01) takes HCI commands, not ACL: an ACL packet written there
     * runs as a command whose opcode is its header (handle 0x033 with its
     * flags reads as 0x2033), answered "unknown command" to whoever reads
     * events, the system's stack included. ACL goes to the other one (0x02)
     * from the start; 0x01 only if there is no other. */
    if (c->out2_open) c->out_idx = X_ACL_OUT2;
    c->ev_want = N_EVENT_RD;
    c->ev_len = c->ev_read_len;
    for (c->n_event = 0; c->n_event < N_EVENT_RD; c->n_event++)
        if (!xfer_open(c, c->n_event, c->ep_events, ACL_BUF)) break;
    for (c->n_acl = 0; c->n_acl < N_ACL_RD; c->n_acl++)
        if (!xfer_open(c, N_EVENT_RD + c->n_acl, c->ep_acl_in, ACL_BUF)) break;
    if (!c->n_event || !c->n_acl) {
        op_close(c);
        return 0;
    }

    log_line("usb: controller open beside the system: %d event reads, %d ACL reads",
             c->n_event, c->n_acl);
    out->ops = &k_usb_ops;
    out->ctx = c;
    return 1;
}

int hci_usb_bind_ownership(hci_t *h, own_ledger *ledger,
                         void (*idle)(void *), void *ud)
{
    usb_ctx *c = &g_ctx;
    if (!h || h->ctx != c || h->ops != &k_usb_ops || !ledger ||
        c->fd < 0 || c->ledger || c->busy[X_CMD]) return 0;
    c->ledger = ledger;
    c->service = idle;
    c->service_ud = ud;
    return 1;
}

own_submit_result hci_usb_submit(hci_t *h, const own_token *token)
{
    const hci_submission submission = {.kind = HCI_SUBMIT_COMMAND, .token.command = token};
    if (!h || h->ctx != &g_ctx || h->ops != &k_usb_ops) return OWN_NOT_SENT;
    return h->ops->op_submit(h->ctx, &submission);
}
