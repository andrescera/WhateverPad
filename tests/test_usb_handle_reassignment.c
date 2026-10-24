/* Standalone boundary regression: frozen backend/ledger repair is out of scope. */
#define main existing_usb_tests_main
#include "test_hci_usb.c"
#undef main
#undef CHECK
#define CHECK(c) do { checks++; if (!(c)) { \
    printf("FAIL boundary:%d: %s\n", __LINE__, #c); failures++; \
} } while (0)

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
        closed_once(h); own_acl_free(lane); own_free(l);
    }
    printf("USB handle-reassignment checks=%d violations=%d\n", checks, failures);
    return failures != 0;
}
