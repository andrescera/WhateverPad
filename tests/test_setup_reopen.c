/* GPL-3.0-or-later. Accepted clean-open contract and setup-undo protection. */
#define main setup_retry_main
#include "test_setup_retry.c"
#undef main

static const own_policy policy = WHATEVERPAD_ATTRIB_POLICY;

static host_t *open_without_reset(sim_t **s)
{
    sim_pad pad = {.seed = 1};
    *s = sim_new(&pad);
    CHECK(*s != NULL);
    if (!*s) return NULL;
    host_t *h = host_prepare(sim_hci(*s), "build/reopen-absent.db", NULL);
    CHECK(h != NULL);
    if (h) sim_bind_ownership(*s, host_ownership(h));
    return h;
}

static void unanswered_host_reopen(void)
{
    attrib_reset(); /* A new product run, not a reset between acquisition tries. */
    sim_t *s;
    host_t *h = open_without_reset(&s);
    if (!h) { CHECK(0); return; }
    sim_go_deaf(s);
    CHECK(!host_start(h) && !host_admits(h));
    close_host(h, s);
    CHECK(attrib_get()->timeout == 1 && attrib_get()->submitted == 3);
    for (unsigned attempt = 2; attempt <= 5; attempt++) {
        unsigned long submitted = attrib_get()->submitted;
        h = open_without_reset(&s);
        if (!h) { CHECK(0); return; }
        CHECK(host_start(h) && host_admits(h));
        CHECK(attrib_get()->unanswered == 0 && attrib_get()->timeout == 1);
        CHECK(attrib_get()->submitted > submitted); /* Keep cumulative diagnostics. */
        close_host(h, s);
    }
}

static own_ref prepare_reopen(own_ledger *l, unsigned op, unsigned value, int setup)
{
    own_command c = {.opcode = (uint16_t)op, .ack_by = 8000, .terminal_by = 12000,
                     .setup_retry = (uint8_t)setup};
    if (op == 0x0c1a) {
        c.plen = c.original_len = c.original_known = 1;
        c.params[0] = (uint8_t)value;
    }
    own_ref ref = {0};
    CHECK(own_prepare(l, &c, &ref) == OWN_OK);
    return ref;
}

static int send_reopen(own_ledger *l, unsigned op, uint64_t at)
{
    own_token token;
    own_effect effect;
    uint64_t deadline;
    own_result next = own_next_hci(l, at, &token);
    CHECK(next == OWN_OK);
    if (next != OWN_OK) return 0;
    own_result committed = own_commit_hci(l, &token, at, &effect, &deadline);
    CHECK(committed == OWN_OK);
    if (committed != OWN_OK) return 0;
    CHECK(effect.opcode == op);
    own_submitted(l, &token, OWN_SENT, at);
    return effect.opcode == op;
}

static void answer_reopen(own_ledger *l, unsigned op, uint64_t at, unsigned random)
{
    own_ack ack = {.opcode = (uint16_t)op, .event = OWN_CC,
                   .random_len = op == 0x2018 ? 8 : 0, .random = {(uint8_t)random}};
    own_receipt receipt;
    CHECK(own_backend_receipt(l, &ack, at, &receipt) == OWN_RECEIPT_SEQUENCED);
    own_receive(l, &receipt, at);
}

static void retire_reopen(own_ledger *l, uint64_t at)
{
    own_release(l, SESSION_STOP, at);
    own_transport_closed(l, at);
    own_token token;
    own_effect effect;
    own_result next = own_next_local(l, at, &token);
    CHECK(next == OWN_OK);
    if (next == OWN_OK) {
        CHECK(own_commit(l, &token, at, &effect) == OWN_OK);
        CHECK(effect.kind == OWN_EFFECT_FINALIZE);
        own_finalized(l, CLEANUP_CONFIRMED);
    }
    own_summary summary;
    own_snapshot(l, &summary);
    CHECK(summary.finalized && own_invariant(l) == NULL);
    own_free(l);
}

static void clean_later_tries(void)
{
    for (unsigned loss = 0; loss < 3; loss++) {
        own_ledger *old = own_new(policy, 1);
        own_ref ref = prepare_reopen(old, loss == 2 ? 0x0c1a : 0x1002, 2, 1);
        if (!ref.serial || !send_reopen(old, loss == 2 ? 0x0c1a : 0x1002, 1000) ||
            !send_reopen(old, loss == 2 ? 0x0c1a : 0x1002, 2000)) {
            own_free(old);
            continue;
        }
        if (loss == 1) {
            CHECK(send_reopen(old, 0x1002, 3000));
            own_tick(old, 6000); /* Entire unanswered command budget exhausted. */
        } else {
            answer_reopen(old, loss == 2 ? 0x0c1a : 0x1002, 2100, 0);
            if (loss == 2) {
                CHECK(send_reopen(old, 0x2018, 2101));
                CHECK(send_reopen(old, 0x2018, 3101));
            }
        }
        retire_reopen(old, loss == 1 ? 6001 : 3200);
        for (unsigned attempt = 2; attempt <= 5; attempt++) {
            sim_t *s;
            host_t *h = open_host(&s);
            if (!h) { CHECK(0); return; }
            CHECK(host_start(h) && host_admits(h));
            close_host(h, s);
        }
    }
}

static void residual_reply(unsigned op)
{
    own_ledger *old = own_new(policy, 1);
    own_ref ref = prepare_reopen(old, op, 2, 1);
    if (!ref.serial || !send_reopen(old, op, 1000) || !send_reopen(old, op, 2000)) {
        own_free(old);
        return;
    }
    answer_reopen(old, op, 2100, 0);
    if (op == 0x0c1a) {
        CHECK(send_reopen(old, 0x2018, 2101));
        CHECK(send_reopen(old, 0x2018, 3101));
    }
    retire_reopen(old, 3200);
    own_ledger *fresh = own_new(policy, 1);
    ref = prepare_reopen(fresh, op, 3, 1);
    if (!ref.serial || !send_reopen(fresh, op, 4000)) { own_free(fresh); return; }
    answer_reopen(fresh, op, 4100, 0); /* Old wire reply; current command was lost. */
    if (op == 0x0c1a) {
        CHECK(send_reopen(fresh, 0x2018, 4101));
        answer_reopen(fresh, 0x2018, 4200, 17); /* Previously unseen old Rand. */
    }
    CHECK(own_activate(fresh)); /* Residual accepted by the chosen clean-open contract. */
    CHECK(own_invariant(fresh) == NULL);
    own_release(fresh, SESSION_STOP, 4201);
    own_token token;
    own_result automatic = own_next_hci(fresh, 4201, &token);
    CHECK(automatic != OWN_OK); /* Neither policy may derive a setup undo token. */
    if (automatic == OWN_OK) {
        own_effect effect;
        uint64_t deadline;
        own_result committed = own_commit_hci(fresh, &token, 4201, &effect, &deadline);
        CHECK(committed != OWN_OK);
        if (committed == OWN_OK) own_submitted(fresh, &token, OWN_NOT_SENT, 4201);
    }
    CHECK(own_request_undo(fresh, ref, 4202, &token) == OWN_NO_PROOF);
    own_state state;
    own_result result = own_status(fresh, ref, &state);
    if (policy == OWN_POLICY_SINGLE_PRODUCER && op == 0x0c1a)
        CHECK(result == OWN_UNCERTAIN && state == OWN_QUARANTINED);
    else CHECK(result == OWN_OK);
    CHECK(own_invariant(fresh) == NULL);
    retire_reopen(fresh, 4203);
    clean_later_tries(); /* Quarantine and retirement never poison a later fresh open. */
}

static void active_restore_control(void)
{
    own_ledger *l = own_new(policy, 1);
    CHECK(own_activate(l));
    own_ref ref = prepare_reopen(l, 0x0c1a, 2, 0); /* Not acquired during setup. */
    if (!ref.serial || !send_reopen(l, 0x0c1a, 1000)) { own_free(l); return; }
    answer_reopen(l, 0x0c1a, 1001, 0);
    CHECK(send_reopen(l, 0x2018, 1002));
    answer_reopen(l, 0x2018, 1003, 1);
    own_release(l, SESSION_STOP, 1004);
    own_token token;
    own_result undo = own_request_undo(l, ref, 1004, &token);
    CHECK(undo == (policy == OWN_POLICY_SINGLE_PRODUCER ? OWN_OK : OWN_NO_PROOF));
    if (undo == OWN_OK) {
        own_effect effect;
        uint64_t deadline;
        CHECK(own_commit_hci(l, &token, 1004, &effect, &deadline) == OWN_OK);
        CHECK(effect.phase == OWN_UNDO && effect.opcode == 0x0c1a && effect.params[0] == 0);
        own_submitted(l, &token, OWN_SENT, 1004);
        answer_reopen(l, 0x0c1a, 1005, 0);
    }
    retire_reopen(l, 1006);
}

int main(void)
{
    log_open("/dev/null");
    unanswered_host_reopen();
    clean_later_tries();
    residual_reply(0x1002);
    residual_reply(0x0c1a);
    active_restore_control();
    printf("setup reopen: policy=%u checks=%u failures=%u\n", policy, checks, failures);
    log_close();
    return failures ? 1 : 0;
}
