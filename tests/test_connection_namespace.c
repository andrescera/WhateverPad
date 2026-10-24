/* A foreign connection terminal must not inherit a local Create's peer type.
 * Reuse the raw-packet/token stand-in, not a model of the production decoder. */
#define main existing_crypto_owner_main
#include "test_le_crypto_owner.c"
#undef main

int main(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        backend b = {.terminal_mode = mode};
        char dir[] = "/tmp/whateverpad-connection-namespace-XXXXXX", db[128];
        char *created = mkdtemp(dir);
        CHECK(created != NULL);
        if (!created) continue;
        snprintf(db, sizeof db, "%s/bonds", dir);
        clock_ms = 5000; inquiry_end = 0;
        ph_clock_hook = clock_now; attrib_reset();
        host_t *h = host_prepare((hci_t){&ops, &b}, db, NULL);
        CHECK(h != NULL);
        if (h) {
            b.ledger = host_ownership(h);
            int started = host_start(h); CHECK(started);
            if (started) {
                CHECK(host_pair(h, 60));
                settle(h, &b);
                advertise(&b, 0);
                for (int i = 0; i < 1000; i++) {
                    host_poll(h, 0);
                    if (b.creates && !b.events.count && !b.acl.count) break;
                }
                CHECK(b.creates == 1);
                /* Count actual outgoing controller commands and SMP frames. */
                CHECK(b.p256 == (mode == 0 ? 1u : 0u));
                CHECK(b.requests[0] == (mode == 0 ? 1u : 0u));
                if (mode == 0) {
                    unsigned before = b.sends;
                    unsigned char foreign[19] = {1, 0, 0x51};
                    memcpy(foreign + 6, peers[1], 6);
                    put16(foreign + 12, 12);
                    put16(foreign + 16, 200);
                    event(&b, 0x3e, foreign, sizeof foreign);
                    unsigned char reply[] = {0x51, 0, 11, 0, 7, 0, 6, 0, 2, 3, 0, 1, 16, 0, 1};
                    push(&b.acl, reply, sizeof reply);
                    host_poll(h, 0);
                    CHECK(b.sends == before); /* Observed reassignment revokes every output. */
                }
            }
            host_close(h);
        }
        CHECK(b.closes == 1 && b.forbidden == 0);
        ph_clock_hook = NULL;
        static const char *suffixes[] = {"", ".new", ".irk", ".irk.new"};
        for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
            char path[160];
            snprintf(path, sizeof path, "%s%s", db, suffixes[i]);
            CHECK(unlink(path) == 0 || errno == ENOENT);
        }
        CHECK(rmdir(dir) == 0);
    }
    printf("connection namespaces: %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
