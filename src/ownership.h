/* GPL-3.0-or-later. Pure ownership transitions: no I/O, callbacks or clock reads.
 * Tokens are consumed at the final boundary, before the caller invokes an effect.
 * Backend evidence is SEQUENCED under A8, not a controller-supplied invocation ID.
 */
#ifndef WHATEVERPAD_OWNERSHIP_H
#define WHATEVERPAD_OWNERSHIP_H

#include "session.h"
#include <stddef.h>
#include <stdint.h>

#define OWN_MAX 32
#define OWN_LINK_MAX 4
#define OWN_BARRIER_MAX 256
#define OWN_FRESH_MS 1000

typedef struct { uint64_t attempt, serial; } own_ref;
typedef struct { uint64_t attempt, generation; uint8_t slot; } link_ref;
typedef enum { OWN_POLICY_NO_RESTORE, OWN_POLICY_SINGLE_PRODUCER } own_policy;
typedef enum {
    OWN_OK, OWN_BUSY, OWN_NO_SPACE, OWN_NO_PROOF, OWN_INVALID, OWN_STOPPED,
    OWN_UNCHANGED, OWN_REFUSED, OWN_UNCERTAIN, OWN_TRANSFERRED
} own_result;
typedef enum {
    OWN_FREE, OWN_PREPARED, OWN_SUBMITTED, OWN_ACKNOWLEDGED,
    OWN_UNDO_SUBMITTED, OWN_QUARANTINED, OWN_CLOSED
} own_state;
typedef enum { OWN_NOT_SENT, OWN_SENT, OWN_SEND_UNCERTAIN } own_submit_result;
typedef enum { OWN_FORWARD, OWN_BARRIER, OWN_UNDO } own_phase;
typedef enum { OWN_RECEIPT_NONE, OWN_RECEIPT_SEQUENCED, OWN_RECEIPT_RETIRED } own_provenance;
typedef enum { OWN_CC = 0x0e, OWN_CS = 0x0f } own_ack_event;
typedef enum {
    OWN_TERM_COMMAND, OWN_TERM_INQUIRY, OWN_TERM_CONNECTION, OWN_TERM_DISCONNECT,
    OWN_TERM_AUTH, OWN_TERM_ENCRYPTION, OWN_TERM_UPDATE,
    OWN_TERM_P256, OWN_TERM_DHKEY
} own_terminal_kind;
typedef enum {
    OWN_EFFECT_HCI, OWN_EFFECT_CONNECT, OWN_EFFECT_RETIRE,
    OWN_EFFECT_ADD, OWN_EFFECT_BIND, OWN_EFFECT_INPUT,
    OWN_EFFECT_DELETE, OWN_EFFECT_FINALIZE
} own_effect_kind;

typedef struct own_ledger own_ledger; /* Representation exists only in ownership.c. */
typedef struct { uint64_t _opaque[8]; } own_token;
typedef struct { uint64_t _opaque[12]; } own_receipt;

typedef struct {
    uint8_t adapter[6], le, address_type, identity[6], wire[6];
} own_peer;

typedef struct {
    uint16_t opcode;
    uint8_t plen, params[255];
    uint8_t original_known, original_len, original[255];
    own_peer peer;
    link_ref link; /* generation zero means no link */
    uint64_t ack_by, terminal_by; /* Absolute deadlines, supplied by the owner. */
    uint8_t setup_retry; /* Explicit pre-link startup only; never runtime/undo. */
} own_command;

typedef struct {
    own_ack_event event;
    uint16_t opcode;
    uint8_t status, random_len, random[8], malformed, overflow;
} own_ack;

typedef struct {
    own_terminal_kind kind;
    own_peer peer;
    uint16_t handle;
    uint8_t status;
} own_terminal_event;

typedef struct {
    own_effect_kind kind;
    own_ref owner;
    link_ref link;
    own_phase phase;
    uint16_t opcode, handle;
    uint8_t plen, params[255];
    uint64_t device_id;
    uint8_t resend;
} own_effect;

typedef struct {
    session_t runtime;
    unsigned records, quarantined, links, radio_left, unidentified, barriers;
    unsigned ambiguity_count, credit_nops;
    int transport_closed, finalized;
} own_summary;

/* Heap storage is private; allocation/free are the only non-transition operations. */
own_ledger *own_new(own_policy, int le_rand_supported);
void own_free(own_ledger *);
own_ledger *own_clone(const own_ledger *); /* A value copy, no external resources. */
int own_activate(own_ledger *);
int own_retry(own_ledger *);
int own_release(own_ledger *, enum session_reason, uint64_t now);
void own_fail(own_ledger *, uint64_t now, int recoverable);
void own_detector_trip(own_ledger *, uint64_t now);
void own_transport_closed(own_ledger *, uint64_t now);
void own_finalized(own_ledger *, enum cleanup_result);
void own_snapshot(const own_ledger *, own_summary *);

own_result own_prepare(own_ledger *, const own_command *, own_ref *);
own_result own_reclaim(own_ledger *, own_ref);
own_result own_status(const own_ledger *, own_ref, own_state *);
/* A pending barrier outranks all normal commands; undo is derived, never supplied. */
own_result own_next_hci(own_ledger *, uint64_t now, own_token *);
own_result own_request_undo(own_ledger *, own_ref, uint64_t now, own_token *);
own_result own_commit(own_ledger *, const own_token *, uint64_t now, own_effect *);
void own_submitted(own_ledger *, const own_token *, own_submit_result, uint64_t now);

/* Only hci_usb.c (or a test backend) may issue a receipt. No causal tags as input.
 * A credit-only NOP has NONE provenance but does not trip the detector.
 */
own_provenance own_backend_receipt(own_ledger *, const own_ack *, uint64_t now,
                                  own_receipt *);
void own_receive(own_ledger *, const own_receipt *, uint64_t now);
/* Successful connections are backend-only; host terminals cannot grant a binding. */
void own_terminal(own_ledger *, own_ref, const own_terminal_event *, uint64_t now);
void own_setting_changed(own_ledger *, uint16_t opcode, uint64_t now);
void own_readback(own_ledger *, uint16_t opcode, const uint8_t *, size_t);
void own_tick(own_ledger *, uint64_t now);
int own_drain_allowed(const own_ledger *, uint64_t now, unsigned completed_in_batch);

own_result own_reserve_link(own_ledger *, const own_peer *, link_ref *);
int own_link_current(const own_ledger *, link_ref);
/* Local identity survives wire revocation until retirement obligations settle. */
int own_link_retained(const own_ledger *, link_ref);
/* Only a profile-accepted report may call this, with the actual wire handle. */
own_result own_input(own_ledger *, link_ref, uint16_t handle, uint64_t now);
own_result own_next_local(own_ledger *, uint64_t now, own_token *);
/* Only the virtual-device backend may report Add/Bind/Delete results/identity. */
void own_backend_local_result(own_ledger *, const own_token *, int success);
void own_backend_identity(own_ledger *, link_ref, uint64_t device_id, int proven);
void own_disconnected(own_ledger *, link_ref, uint16_t handle, uint64_t now);
own_result own_detach_link(own_ledger *, link_ref);

/* Read-only validation and canonical, padding-free state for bounded exploration.
 * canonical returns required bytes; zeroed unused bytes/physical table order are
 * excluded. The caller must compare keys, not assume a hash has no collisions.
 */
const char *own_invariant(const own_ledger *);
size_t own_canonical(const own_ledger *, uint8_t *out, size_t capacity);

/* Additive output capabilities; no existing record or canonical encoding changes.
 * A lane borrows its ledger, holds one immutable packet, and must be freed first.
 * ACL output never counts as profile-accepted input or refreshes Disconnect proof.
 */
#define OWN_ACL_MAX 1100
typedef struct own_acl_lane own_acl_lane;
typedef struct { uint64_t _opaque[4]; } own_acl_token;
typedef struct {
    link_ref link;
    uint16_t handle, len;
    uint8_t packet[OWN_ACL_MAX];
} own_acl_effect;

own_acl_lane *own_acl_new(own_ledger *);
void own_acl_free(own_acl_lane *);
own_result own_acl_prepare(own_acl_lane *, link_ref, const uint8_t *, size_t,
                           uint64_t now, own_acl_token *);
/* The expected ledger prevents a valid foreign lane from crossing a backend. */
own_result own_acl_commit(own_ledger *, own_acl_lane *, const own_acl_token *,
                          uint64_t now, own_acl_effect *);
void own_acl_submitted(own_acl_lane *, const own_acl_token *, own_submit_result,
                       uint64_t now);
/* Typed final HCI boundary, including the exact phase's diagnostic ACK deadline. */
own_result own_commit_hci(own_ledger *, const own_token *, uint64_t now,
                          own_effect *, uint64_t *ack_by);
/* Wire observation revokes an existing lease before any output can be committed. */
void own_observe_disconnect(own_ledger *, uint16_t handle, uint64_t now);

/* Backend-only: revoke conflicting bindings before matching a submitted
 * Create/Accept. Retained pending terminals are subject to the same veto. */
void own_observe_connection(own_ledger *, uint8_t le, uint8_t address_type,
                            const uint8_t address[6], uint16_t handle, uint8_t status,
                            uint64_t now);

#endif
