#define main explorer_main
#include "test_ownership_explore.c"
#undef main
#include <assert.h>

static unsigned take_bits(const uint8_t *key, size_t *bit, unsigned count)
{
    unsigned value = 0;
    while (count--) {
        value = (value << 1) | ((key[*bit / 8] >> (7 - *bit % 8)) & 1);
        (*bit)++;
    }
    return value;
}

static void round_trip(const uint8_t *key, size_t n)
{
    static const uint8_t literals[] = {0, 1, 2, 16, 128, 129};
    uint8_t packed[2 * KEY_MAX], decoded[KEY_MAX];
    size_t used = pack_key(key, n, packed), bit = 16, out = 0;
    assert(used <= sizeof packed);
    assert((size_t)(packed[0] | packed[1] << 8) == n);
    while (out < n) {
        assert(bit + 3 <= 8 * used);
        unsigned code = take_bits(packed, &bit, 3);
        if (code < 6) decoded[out++] = literals[code];
        else {
            assert(bit + 8 <= 8 * used);
            unsigned value = take_bits(packed, &bit, 8);
            if (code == 6) {
                assert(value >= 4 && value <= n - out);
                memset(decoded + out, 0, value);
                out += value;
            } else decoded[out++] = (uint8_t)value;
        }
    }
    assert((bit + 7) / 8 == used && !memcmp(key, decoded, n));
    while (bit < 8 * used) assert(take_bits(packed, &bit, 1) == 0);
}

int main(void)
{
    uint8_t key[KEY_MAX] = {0};
    static const size_t lengths[] = {0, 1, 2, 3, 4, 254, 255, 256, 257, 510, 511, 512, KEY_MAX};
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; i++) round_trip(key, lengths[i]);
    for (unsigned first = 0; first < 256; first++)
        for (unsigned second = 0; second < 256; second++) {
            key[0] = (uint8_t)first; key[1] = (uint8_t)second;
            round_trip(key, 2);
        }
    for (unsigned shift = 0; shift < 256; shift++) {
        for (size_t i = 0; i < sizeof key; i++) key[i] = (uint8_t)(i + shift);
        round_trip(key, sizeof key);
    }
    memset(key, 255, sizeof key);
    round_trip(key, sizeof key);
    puts("explorer key storage round trips passed");
    return 0;
}
