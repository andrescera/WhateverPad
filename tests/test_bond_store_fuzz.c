/* Fixed-seed malformed input, under ASan/UBSan. No disk or real credentials. */
#include "../src/bond_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned seed = 731;
static unsigned rnd(void) { seed = seed * 1664525u + 1013904223u; return seed; }

int main(void)
{
    const bond_adapter adapter = { {1, 2, 3, 4, 5, 6}, 0x0e8d, 0x0608 };
    bond_store valid = { .adapter = adapter }, parsed, roundtrip, sentinel;
    uint8_t original[BOND_FILE_MAX], bytes[BOND_FILE_MAX + 65], encoded[BOND_FILE_MAX];
    size_t len, encoded_len, n;
    unsigned i, j, accepted = 0;

    valid.count = 2;
    valid.records[0] = (db_rec){ .addr = {7, 8, 9, 10, 11, 12}, .complete = 1 };
    valid.records[1] = (db_rec){ .addr = {7, 8, 9, 10, 11, 0xcc}, .le = 1,
        .addr_type = 1, .complete = 1, .sc = 1, .has_irk = 1 };
    memset(valid.records[0].key, 0x41, 16);
    memset(valid.records[1].key, 0x52, 16);
    memset(valid.records[1].irk, 0x63, 16);
    assert(bond_store_encode(&valid, original, &len) == BOND_COMMITTED);
    memset(&sentinel, 0x5a, sizeof sentinel);
    for (i = 0; i < 100000; i++) {
        n = rnd() % sizeof bytes;
        for (j = 0; j < sizeof bytes; j++) bytes[j] = (uint8_t)(rnd() >> 24);
        if (i & 1) {
            memcpy(bytes, original, len);
            n = len;
            for (j = 0; j < 3; j++) bytes[rnd() % len] ^= (uint8_t)((rnd() >> 24) | 1);
        }
        parsed = sentinel;
        if (bond_store_decode(bytes, n, &adapter, &parsed) == BOND_COMMITTED) {
            accepted++;
            assert(bond_store_encode(&parsed, encoded, &encoded_len) == BOND_COMMITTED);
            assert(n == encoded_len && memcmp(bytes, encoded, n) == 0);
            assert(bond_store_decode(encoded, n, &adapter, &roundtrip) == BOND_COMMITTED);
            assert(memcmp(&parsed, &roundtrip, sizeof parsed) == 0);
        } else assert(memcmp(&parsed, &sentinel, sizeof parsed) == 0);
        /* Guarantee malformed framing even when a random mutation is valid. */
        bytes[0] ^= 0x80;
        if (bytes[0] == 'W') bytes[0] = 0;
        assert(bond_store_decode(bytes, n, &adapter, &parsed) != BOND_COMMITTED);
        for (j = 0; j < len; j++)
            assert(bond_store_decode(original, j, &adapter, &parsed) != BOND_COMMITTED);
    }
    printf("bond fuzz: 100000 inputs, %u valid mutations, malformed frames rejected, no fault\n", accepted);
    return 0;
}
