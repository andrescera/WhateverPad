/* Synthetic credentials only. The wire fixture is independent of the encoder. */
#include "../src/bond_store.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const bond_adapter adapter = { { 1, 2, 3, 4, 5, 6 }, 0x0e8d, 0x0608 };
static char root[] = "/tmp/whateverpad-bonds-XXXXXX";
static char dir[160], path[180];

static void put16(unsigned char *p, unsigned n) { p[0] = n; p[1] = n >> 8; }

static void fixture(unsigned char b[192])
{
    memset(b, 0, 192);
    memcpy(b, "WHPBOND", 8);
    put16(b + 8, 1); put16(b + 10, 64); put16(b + 12, 64); put16(b + 14, 2);
    memcpy(b + 16, adapter.addr, 6);
    put16(b + 22, adapter.vid); put16(b + 24, adapter.pid);
    b[26] = 1;
    memset(b + 28, 0x31, 16);
    b[66] = 7; b[71] = 0xc8;
    memset(b + 72, 0x42, 16);
    b[88] = 4; b[89] = 1;
    put16(b + 90, 0x054c); put16(b + 92, 0x05c4);
    b[128] = 1; b[129] = 1;
    memcpy(b + 130, b + 66, 6);
    memset(b + 136, 0x53, 16);
    b[153] = 3;
    put16(b + 154, 0x045e); put16(b + 156, 0x0b13);
    put16(b + 158, 0x1234);
    memset(b + 160, 0x64, 8);
    memset(b + 168, 0x75, 16);
}

static void write_bytes(const void *b, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    assert(write(fd, b, n) == (ssize_t)n);
    assert(close(fd) == 0);
}

static void same_file(const void *b, size_t n)
{
    unsigned char got[BOND_FILE_MAX + 1];
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0);
    assert(read(fd, got, sizeof got) == (ssize_t)n);
    assert(memcmp(got, b, n) == 0);
    assert(close(fd) == 0);
}

static int fail_stage;
int bond_store_test_fault(bond_stage stage) { return (int)stage == fail_stage; }

static void parser(void)
{
    unsigned char b[193], bad[256];
    bond_store s, untouched;
    bond_adapter other = adapter;
    unsigned i;
    static const unsigned invalid_offsets[] = {
        0, 7, 10, 11, 12, 13, 26, 27, 44, 63,
        64, 65, 88, 89, 94, 96, 104, 120, 127,
        128, 129, 135, 152, 153, 184, 191
    };

    fixture(b);
    assert(bond_store_decode(b, 192, &adapter, &s) == BOND_COMMITTED);
    assert(s.count == 2 && s.has_irk && s.local_irk[0] == 0x31);
    assert(s.records[0].complete && s.records[0].type == 4);
    assert(s.records[1].ediv == 0x1234 && s.records[1].has_irk);
    assert(s.records[1].key[15] == 0x53 && s.records[1].rand[7] == 0x64);
    assert(s.records[1].irk[15] == 0x75);
    untouched = s;
    for (i = 0; i < 192; i++) {
        assert(bond_store_decode(b, i, &adapter, &s) != BOND_COMMITTED);
        assert(memcmp(&s, &untouched, sizeof s) == 0);
    }
    b[192] = 0;
    assert(bond_store_decode(b, 193, &adapter, &s) == BOND_INVALID);
    for (i = 0; i < sizeof invalid_offsets / sizeof *invalid_offsets; i++) {
        memcpy(bad, b, 192);
        bad[invalid_offsets[i]] = 0x80;
        assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    }
    memcpy(bad, b, 192); bad[8] = 2;
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_VERSION);
    memcpy(bad, b, 192); bad[14] = 0xff; bad[15] = 0xff;
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_CAPACITY);
    memcpy(bad, b, 192); bad[14] = 1;
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    other.addr[0]++;
    assert(bond_store_decode(b, 192, &other, &s) == BOND_ADAPTER);
    other = adapter; other.vid++;
    assert(bond_store_decode(b, 192, &other, &s) == BOND_ADAPTER);
    other = adapter; other.pid++;
    assert(bond_store_decode(b, 192, &other, &s) == BOND_ADAPTER);
    other = adapter; memset(other.addr, 0, 6);
    assert(bond_store_decode(b, 192, &other, &s) == BOND_ADAPTER);
    memcpy(bad, b, 192); memset(bad + 66, 0, 6);
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    memcpy(bad, b, 192); bad[135] = 0x48; /* RPA is not an identity */
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    memcpy(bad, b, 192); bad[26] = 0; /* absent IRK must be zero */
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    memcpy(bad, b, 192); bad[153] = 1;
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    memcpy(bad, b, 192); bad[153] |= 4; /* SC cannot carry legacy EDIV/Rand */
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    memcpy(bad, b, 192); memcpy(bad + 128, bad + 64, 64);
    assert(bond_store_decode(bad, 192, &adapter, &s) == BOND_INVALID);
    /* Identical six bytes in Classic/public, LE/random, and LE/public. */
    memcpy(bad, b, 192); put16(bad + 14, 3);
    memcpy(bad + 192, b + 128, 64); bad[193] = 0;
    assert(bond_store_decode(bad, 256, &adapter, &s) == BOND_COMMITTED);
    assert(bond_store_find(&s, 0, 0, b + 66) == &s.records[0]);
    assert(bond_store_find(&s, 1, 1, b + 66) == &s.records[1]);
    assert(bond_store_find(&s, 1, 0, b + 66) == &s.records[2]);
    puts("parser: length, binding, namespace, flags, reserved fields passed");
}

static void roundtrip_and_capacity(void)
{
    unsigned char b[192], encoded[BOND_FILE_MAX];
    size_t n;
    bond_store s, reopened, before;
    db_rec r;
    struct stat st;
    int i;

    fixture(b);
    assert(bond_store_decode(b, 192, &adapter, &s) == BOND_COMMITTED);
    assert(bond_store_encode(&s, encoded, &n) == BOND_COMMITTED && n == 192);
    assert(memcmp(b, encoded, n) == 0);
    assert(bond_store_save(path, &s) == BOND_COMMITTED);
    same_file(b, n);
    assert(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    assert(memcmp(&s, &reopened, sizeof s) == 0);
    assert(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    printf("modes: directory 700, file 600 (%s)\n", path);
    r = s.records[0];
    for (i = 2; i < BOND_MAX; i++) {
        r.addr[0] = (unsigned char)(i + 20);
        assert(bond_store_put(&s, &r) == BOND_COMMITTED);
    }
    assert(s.count == 16);
    before = s; r.addr[0] = 99;
    assert(bond_store_put(&s, &r) == BOND_CAPACITY);
    assert(memcmp(&s, &before, sizeof s) == 0);
    assert(bond_store_save(path, &s) == BOND_COMMITTED);
    assert(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    assert(memcmp(&s, &reopened, sizeof s) == 0);
    /* SC and incomplete/repair-needed records round-trip without inventing keys. */
    s.records[1].sc = 1; s.records[1].ediv = 0;
    memset(s.records[1].rand, 0, 8);
    s.records[0].complete = 0; s.records[0].repair_needed = 1;
    assert(bond_store_save(path, &s) == BOND_COMMITTED);
    assert(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    assert(memcmp(&s, &reopened, sizeof s) == 0);
    puts("roundtrip and capacity: 17th rejected, all 16 retained");
}

static void failures(void)
{
    unsigned char b[192], bad[193], stale[192];
    bond_store old, next, loaded;
    bond_result want;
    int stage;
    char tmp[220], alias[200];
    DIR *d;
    struct dirent *entry;

    fixture(b);
    assert(bond_store_decode(b, 192, &adapter, &old) == BOND_COMMITTED);
    next = old; next.records[0].key[0]++; next.local_irk[0]++;
    for (stage = BOND_STAGE_WRITE; stage <= BOND_STAGE_DIR_FSYNC; stage++) {
        write_bytes(b, 192);
        fail_stage = stage;
        want = stage == BOND_STAGE_DIR_FSYNC ? BOND_UNCERTAIN : BOND_ERROR;
        assert(bond_store_save(path, &next) == want);
        fail_stage = 0;
        assert(bond_store_load(path, &adapter, &loaded) == BOND_COMMITTED);
        if (want == BOND_ERROR) { same_file(b, 192); assert(memcmp(&loaded, &old, sizeof old) == 0); }
        else assert(memcmp(&loaded, &next, sizeof next) == 0);
        printf("fault stage %d: %s, file %s\n", stage,
               want == BOND_UNCERTAIN ? "UNCERTAIN" : "ERROR",
               want == BOND_UNCERTAIN ? "new complete transaction" : "old intact");
    }
    for (stage = 0; stage < 4; stage++) {
        memcpy(bad, b, 192); bad[192] = 0;
        if (stage == 0) bad[0] = 'X';
        if (stage == 1) bad[8] = 2;
        if (stage == 2) bad[16]++;
        write_bytes(bad, stage == 3 ? 193 : 192);
        want = stage == 1 ? BOND_VERSION : stage == 2 ? BOND_ADAPTER : BOND_INVALID;
        assert(bond_store_load(path, &adapter, &loaded) == want);
        assert(bond_store_save(path, &next) == want);
        same_file(bad, stage == 3 ? 193 : 192);
    }
    assert(unlink(path) == 0);
    assert(symlink("missing", path) == 0);
    assert(bond_store_load(path, &adapter, &loaded) == BOND_ERROR);
    assert(bond_store_save(path, &next) == BOND_ERROR);
    assert(unlink(path) == 0);
    snprintf(alias, sizeof alias, "%s/alias", root);
    assert(symlink(dir, alias) == 0);
    snprintf(tmp, sizeof tmp, "%s/pads.db", alias);
    assert(bond_store_save(tmp, &next) == BOND_ERROR);
    assert(unlink(alias) == 0);
    /* Interrupted writes are never read or adopted, even with a valid frame. */
    snprintf(tmp, sizeof tmp, "%s.new", path);
    memcpy(stale, b, 192);
    { int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
      assert(fd >= 0 && write(fd, stale, 192) == 192 && close(fd) == 0); }
    assert(bond_store_load(path, &adapter, &loaded) == BOND_MISSING);
    assert(loaded.count == 0 && !loaded.has_irk);
    assert(bond_store_save(path, &next) == BOND_COMMITTED);
    assert(unlink(tmp) == 0);
    assert(unlink(path) == 0);
    d = opendir(dir); assert(d);
    while ((entry = readdir(d))) assert(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
    assert(closedir(d) == 0);
    puts("preservation, symlinks, stale temp, and cleanup passed");
}

int main(void)
{
    assert(mkdtemp(root));
    snprintf(dir, sizeof dir, "%s/state", root);
    snprintf(path, sizeof path, "%s/pads.db", dir);
    parser();
    roundtrip_and_capacity();
    failures();
    assert(rmdir(dir) == 0);
    assert(rmdir(root) == 0);
    puts("all bond store checks passed");
    return 0;
}
