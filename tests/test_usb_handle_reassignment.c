/* Standalone boundary regression: frozen backend/ledger repair is out of scope. */
#define main existing_usb_tests_main
#include "test_hci_usb.c"
#undef main
#undef CHECK
#define CHECK(c) do { checks++; if (!(c)) { \
    printf("FAIL boundary:%d: %s\n", __LINE__, #c); failures++; \
} } while (0)

#define main original_reassignment
int main(void)
{
    for (int policy = 0; policy < 2; policy++) for (int foreign = 0; foreign < 2; foreign++) {
        own_ref parent;
        own_acl_token token;
        const unsigned char packet[] = {0x41, 0x20, 0, 0};
        unsigned char connected[] = {3, 11, 0, 0x41, 0, 1, 2, 3, 4, 5, 6, 1, 0};
        reset(); attrib_reset();
        hci_t h = start();
        own_ledger *l = bound(&h, (own_policy)policy);
        link_ref link = acquire_link(h, l, &parent);
        own_acl_lane *lane = own_acl_new(l);
        CHECK(lane != NULL);
        CHECK(own_acl_prepare(lane, link, packet, sizeof packet,
                              (uint64_t)clock_ms, &token) == OWN_OK);
        h.ops->pump(h.ctx, 0);
        if (foreign) connected[5] ^= 0x80;
        completion(0, connected, sizeof connected);
        own_submit_result result = acl_submit(h, lane, &token);
        printf("boundary policy=%d foreign=%d result=%d outputs=%d current=%d\n",
               policy, foreign, result, usb.outputs, own_link_current(l, link));
        CHECK(result == (foreign ? OWN_NOT_SENT : OWN_SENT));
        CHECK(usb.outputs == !foreign);
        CHECK(own_link_current(l, link) == !foreign);
        CHECK(own_invariant(l) == NULL);
        closed_once(h); own_acl_free(lane); own_free(l);
    }
    printf("USB handle-reassignment checks=%d violations=%d\n", checks, failures);
    return failures != 0;
}
#undef main

typedef struct {
    hci_t h;
    own_ledger *l;
    own_peer peer;
    own_ref parent;
    link_ref link;
    unsigned transport;
} binding_fixture;

static int binding_event(unsigned transport, unsigned char event[33])
{
    static const unsigned char classic[] = {3,11,0,0x41,0,1,2,3,4,5,6,1,0};
    static const unsigned char le[] = {
        0x3e,19,1,0,0x41,0,0,0,1,2,3,4,5,6,24,0,0,0,72,0,0
    };
    static const unsigned char enhanced[] = {
        0x3e,31,0x0a,0,0x41,0,0,0,1,2,3,4,5,6,
        0,0,0,0,0,0,0,0,0,0,0,0,24,0,0,0,72,0,0
    };
    int n = transport == 0 ? sizeof classic : transport == 1 ? sizeof le : sizeof enhanced;
    memcpy(event, transport == 0 ? classic : transport == 1 ? le : enhanced, (size_t)n);
    return n;
}

static binding_fixture binding_start(unsigned policy, unsigned transport)
{
    binding_fixture f = {.transport = transport,
        .peer = {.le = transport != 0, .wire = {1,2,3,4,5,6}, .identity = {1,2,3,4,5,6}}};
    own_token t;
    own_command c = {.opcode = transport ? 0x200d : 0x0405,
        .plen = transport ? 25 : 13, .ack_by = 3000, .terminal_by = 5000, .peer = f.peer};
    reset(); attrib_reset(); f.h = start(); f.l = bound(&f.h, (own_policy)policy);
    CHECK(own_reserve_link(f.l, &f.peer, &f.link) == OWN_OK);
    c.link = f.link;
    memcpy(c.params + (transport ? 6 : 0), f.peer.wire, 6);
    CHECK(own_prepare(f.l, &c, &f.parent) == OWN_OK);
    CHECK(own_next_hci(f.l, (uint64_t)clock_ms, &t) == OWN_OK);
    CHECK(hci_usb_submit(&f.h, &t) == OWN_SENT);
    return f;
}

static void binding_live(binding_fixture *f)
{
    unsigned char ev[33];
    int n = binding_event(f->transport, ev);
    acknowledge(f->h, f->transport ? 0x200d : 0x0405, 1, 0);
    receive_event(f->h, ev, n);
    fence(f->h, f->l, 1);
    CHECK(own_input(f->l, f->link, 0x41, (uint64_t)clock_ms) == OWN_OK);
}

static own_result binding_command(binding_fixture *f, own_token *token)
{
    own_ref r;
    own_command c = {.opcode = f->transport ? 0x2013 : 0x0411,
        .plen = f->transport ? 14 : 2, .params = {0x41,0},
        .peer = f->peer, .link = f->link, .ack_by = 3000, .terminal_by = 5000};
    own_result result = own_prepare(f->l, &c, &r);
    return result == OWN_OK ? own_next_hci(f->l, (uint64_t)clock_ms, token) : result;
}

static void binding_commands_during_reap(int undo)
{
    for (unsigned policy = 0; policy < 2; policy++)
        for (unsigned transport = 0; transport < 3; transport++)
            for (unsigned foreign = 0; foreign < 2; foreign++) {
                binding_fixture f = binding_start(policy, transport);
                own_token t = {{0}};
                unsigned char ev[33];
                int n = binding_event(transport, ev);
                binding_live(&f);
                own_result prepared = undo ?
                    own_request_undo(f.l, f.parent, (uint64_t)clock_ms, &t) :
                    binding_command(&f, &t);
                CHECK(prepared == (undo && !policy ? OWN_NO_PROOF : OWN_OK));
                f.h.ops->pump(f.h.ctx, 0);
                if (foreign) ev[transport ? 8 : 5] ^= 0x80;
                completion(0, ev, n);
                int before = usb.commands;
                own_submit_result sent = prepared == OWN_OK ?
                    hci_usb_submit(&f.h, &t) : OWN_NOT_SENT;
                CHECK(sent == (!foreign && (!undo || policy) ? OWN_SENT : OWN_NOT_SENT));
                CHECK(usb.commands == before + (!foreign && (!undo || policy)));
                if (undo && !policy) {
                    CHECK(own_request_undo(f.l, f.parent, (uint64_t)clock_ms, &t) != OWN_OK);
                    CHECK(usb.last_opcode != 0x0406);
                }
                printf("binding-%s policy=%u transport=%u foreign=%u sent=%d commands=%d\n",
                       undo ? "undo" : "forward", policy, transport, foreign, sent, usb.commands);
                closed_once(f.h); own_free(f.l);
            }
}

static void binding_local(binding_fixture *f, own_effect_kind kind)
{
    own_token t = {{0}};
    own_effect e = {0};
    own_result result = own_next_local(f->l, (uint64_t)clock_ms, &t);
    CHECK(result == OWN_OK);
    if (result != OWN_OK) return;
    result = own_commit(f->l, &t, (uint64_t)clock_ms, &e);
    CHECK(result == OWN_OK);
    if (result != OWN_OK) return;
    CHECK(e.kind == kind);
    CHECK(own_commit(f->l, &t, (uint64_t)clock_ms, &e) != OWN_OK);
    own_backend_local_result(f->l, &t, 1);
}

static void binding_pending_and_retirement(void)
{
    static const unsigned char random[] = {0x0e,12,1,0x18,0x20,0,1,0,0,0,0,0,0,0};
    static const unsigned char packet[] = {0x41,0x20,0,0};
    for (unsigned policy = 0; policy < 2; policy++)
        for (unsigned transport = 0; transport < 3; transport++)
            for (unsigned stage = 0; stage < 3; stage++) {
                binding_fixture f = binding_start(policy, transport);
                own_token t;
                unsigned char ev[33], foreign[33], queued[HCI_PKT_MAX];
                int n = binding_event(transport, ev);
                if (stage == 2) {
                    binding_live(&f);
                    binding_local(&f, OWN_EFFECT_CONNECT);
                    binding_local(&f, OWN_EFFECT_ADD);
                    own_backend_identity(f.l, f.link, 7, 1);
                    binding_local(&f, OWN_EFFECT_BIND);
                } else if (stage == 1) {
                    acknowledge(f.h, transport ? 0x200d : 0x0405, 1, 0);
                    CHECK(own_next_hci(f.l, (uint64_t)clock_ms, &t) == OWN_OK);
                    CHECK(hci_usb_submit(&f.h, &t) == OWN_SENT);
                }
                /* A's event remains queued while B arrives, before any host dispatch. */
                f.h.ops->pump(f.h.ctx, 0);
                completion(0, ev, n);
                CHECK(f.h.ops->pump(f.h.ctx, 0) == 1);
                memcpy(foreign, ev, (size_t)n);
                foreign[transport ? 8 : 5] ^= 0x80;
                f.h.ops->pump(f.h.ctx, 0);
                completion(0, foreign, n);
                CHECK(f.h.ops->pump(f.h.ctx, 0) == 1);
                CHECK(!own_link_current(f.l, f.link));
                CHECK(f.h.ops->next_event(f.h.ctx, queued, sizeof queued) == n);
                CHECK(!memcmp(queued, ev, (size_t)n));
                CHECK(f.h.ops->next_event(f.h.ctx, queued, sizeof queued) == n);
                if (stage == 0) {
                    acknowledge(f.h, transport ? 0x200d : 0x0405, 1, 0);
                    fence(f.h, f.l, 1);
                } else if (stage == 1) receive_event(f.h, random, sizeof random);
                own_terminal_event old = {.kind = OWN_TERM_CONNECTION, .peer = f.peer, .handle = 0x41};
                own_terminal(f.l, f.parent, &old, (uint64_t)clock_ms);
                receive_event(f.h, ev, n); /* Duplicate backend delivery must not revive A. */
                CHECK(!own_link_current(f.l, f.link));
                CHECK(own_input(f.l, f.link, 0x41, (uint64_t)clock_ms) != OWN_OK);
                own_acl_lane *lane = own_acl_new(f.l);
                own_acl_token output;
                CHECK(lane != NULL);
                CHECK(own_acl_prepare(lane, f.link, packet, sizeof packet,
                                      (uint64_t)clock_ms, &output) != OWN_OK);
                CHECK(binding_command(&f, &t) != OWN_OK);
                CHECK(own_request_undo(f.l, f.parent, (uint64_t)clock_ms, &t) != OWN_OK);
                CHECK(usb.commands == 2 && usb.outputs == 0 && usb.last_opcode != 0x0406);
                if (stage == 2) {
                    CHECK(own_detach_link(f.l, f.link) != OWN_OK);
                    binding_local(&f, OWN_EFFECT_RETIRE);
                    binding_local(&f, OWN_EFFECT_DELETE);
                }
                CHECK(own_next_local(f.l, (uint64_t)clock_ms, &t) != OWN_OK);
                CHECK(own_detach_link(f.l, f.link) == OWN_OK);
                own_summary summary;
                own_snapshot(f.l, &summary);
                CHECK(summary.radio_left == 1); /* Revocation is not a Disconnect receipt. */
                CHECK(own_invariant(f.l) == NULL);
                printf("binding-pending policy=%u transport=%u stage=%u commands=%d outputs=%d\n",
                       policy, transport, stage, usb.commands, usb.outputs);
                closed_once(f.h); own_acl_free(lane); own_free(f.l);
            }
}

static void binding_unobserved_host_success(void)
{
    static const unsigned char packet[] = {0x41,0x20,0,0};
    for (unsigned policy = 0; policy < 2; policy++)
        for (unsigned transport = 0; transport < 3; transport++) {
            binding_fixture f = binding_start(policy, transport);
            own_token t;
            acknowledge(f.h, transport ? 0x200d : 0x0405, 1, 0);
            fence(f.h, f.l, 1);
            own_terminal_event old = {.kind = OWN_TERM_CONNECTION, .peer = f.peer, .handle = 0x41};
            own_terminal(f.l, f.parent, &old, (uint64_t)clock_ms);
            CHECK(own_input(f.l, f.link, 0x41, (uint64_t)clock_ms) != OWN_OK);
            own_acl_lane *lane = own_acl_new(f.l);
            own_acl_token output;
            CHECK(lane != NULL);
            CHECK(own_acl_prepare(lane, f.link, packet, sizeof packet,
                                  (uint64_t)clock_ms, &output) != OWN_OK);
            CHECK(binding_command(&f, &t) != OWN_OK);
            CHECK(own_request_undo(f.l, f.parent, (uint64_t)clock_ms, &t) != OWN_OK);
            CHECK(usb.commands == 2 && usb.outputs == 0);
            printf("binding-unobserved policy=%u transport=%u\n", policy, transport);
            closed_once(f.h); own_acl_free(lane); own_free(f.l);
        }
}

static void binding_pending_refusal_cannot_recycle_handle(void)
{
    static const unsigned char packet[] = {0x41,0x20,0,0};
    for (unsigned policy = 0; policy < 2; policy++)
        for (unsigned transport = 0; transport < 3; transport++) {
            binding_fixture f = binding_start(policy, transport);
            unsigned char ev[33];
            int n = binding_event(transport, ev);
            own_ref old = f.parent;
            link_ref previous = f.link;
            own_token t;
            receive_event(f.h, ev, n);
            acknowledge(f.h, transport ? 0x200d : 0x0405, 1, 0x0c);
            CHECK(own_detach_link(f.l, previous) == OWN_OK);
            CHECK(own_reclaim(f.l, old) == OWN_OK);
            CHECK(own_reserve_link(f.l, &f.peer, &f.link) == OWN_OK);
            CHECK(f.link.slot == previous.slot && f.link.generation != previous.generation);
            own_command c = {.opcode = transport ? 0x200d : 0x0405,
                .plen = transport ? 25 : 13, .peer = f.peer, .link = f.link,
                .ack_by = 3000, .terminal_by = 5000};
            memcpy(c.params + (transport ? 6 : 0), f.peer.wire, 6);
            CHECK(own_prepare(f.l, &c, &f.parent) == OWN_OK);
            CHECK(own_next_hci(f.l, (uint64_t)clock_ms, &t) == OWN_OK);
            CHECK(hci_usb_submit(&f.h, &t) == OWN_SENT);
            acknowledge(f.h, c.opcode, 1, 0);
            fence(f.h, f.l, 1);
            own_terminal_event queued = {.kind = OWN_TERM_CONNECTION, .peer = f.peer, .handle = 0x41};
            own_terminal(f.l, old, &queued, (uint64_t)clock_ms);
            receive_event(f.h, ev, n); /* An old success cannot select the replacement generation. */
            CHECK(!own_link_current(f.l, previous));
            CHECK(own_input(f.l, f.link, 0x41, (uint64_t)clock_ms) != OWN_OK);
            own_acl_lane *lane = own_acl_new(f.l);
            own_acl_token output;
            CHECK(lane != NULL);
            own_result prepared = own_acl_prepare(lane, f.link, packet, sizeof packet,
                                                  (uint64_t)clock_ms, &output);
            own_submit_result sent = prepared == OWN_OK ? acl_submit(f.h, lane, &output) : OWN_NOT_SENT;
            CHECK(sent == OWN_NOT_SENT && usb.outputs == 0);
            CHECK(own_request_undo(f.l, f.parent, (uint64_t)clock_ms, &t) != OWN_OK);
            CHECK(own_invariant(f.l) == NULL);
            printf("binding-reuse policy=%u transport=%u sent=%d outputs=%d\n",
                   policy, transport, sent, usb.outputs);
            closed_once(f.h); own_acl_free(lane); own_free(f.l);
        }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--original")) return original_reassignment();
    if (argc == 1) original_reassignment();
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--forward"))) binding_commands_during_reap(0);
    else if (argc == 2 && !strcmp(argv[1], "--undo")) binding_commands_during_reap(1);
    else if (argc == 2 && !strcmp(argv[1], "--pending")) binding_pending_and_retirement();
    else if (argc == 2 && !strcmp(argv[1], "--unobserved")) binding_unobserved_host_success();
    else if (argc == 2 && !strcmp(argv[1], "--reuse")) binding_pending_refusal_cannot_recycle_handle();
    else return 2;
    if (argc == 1) {
        binding_commands_during_reap(1);
        binding_pending_and_retirement();
        binding_unobserved_host_success();
        binding_pending_refusal_cannot_recycle_handle();
    }
    printf("binding regressions checks=%d violations=%d\n", checks, failures);
    return failures != 0;
}
