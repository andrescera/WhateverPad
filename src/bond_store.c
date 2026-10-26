#include "bond_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <sys/syscall.h>
#endif

static int store_openat(int dir, const char *name, int flags, mode_t mode)
{
#if defined(__PROSPERO__) || defined(BOND_STORE_OPENAT_TEST)
    unsigned char failed;
#ifdef BOND_STORE_OPENAT_TEST
    extern long bond_store_test_openat(int, const char *, int, mode_t, unsigned char *);
    long result = bond_store_test_openat(dir, name, flags, mode, &failed);
#else
    /* The SDK's openat stub drops carry, returning positive errno as an fd. */
    register long result __asm__("rax") = SYS_openat;
    register long arg4 __asm__("r10") = mode;
    __asm__ volatile("syscall; setc %1"
                     : "+a"(result), "=qm"(failed)
                     : "D"((long)dir), "S"(name), "d"((long)flags), "r"(arg4)
                     : "rcx", "r11", "memory", "cc");
#endif
    if (failed) { errno = (int)result; return -1; }
    return (int)result;
#else
    return openat(dir, name, flags, mode);
#endif
}

static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int all(const uint8_t *p, size_t n, uint8_t v)
{
    size_t i;
    for (i = 0; i < n; i++) if (p[i] != v) return 0;
    return 1;
}

static int address_ok(const uint8_t *p)
{
    return !all(p, 6, 0) && !all(p, 6, 0xff);
}

static int record_ok(const db_rec *r)
{
    if (r->le > 1 || r->addr_type > 1 || !address_ok(r->addr) ||
        r->complete > 1 || r->has_irk > 1 || r->sc > 1 || r->repair_needed > 1) return 0;
    if (!r->has_irk && !all(r->irk, 16, 0)) return 0;
    if (!r->le)
        return !r->addr_type && r->type <= 8 && !r->ediv && all(r->rand, 8, 0) &&
               !r->has_irk && !r->sc;
    if (r->type || (r->sc && (r->ediv || !all(r->rand, 8, 0)))) return 0;
    /* Only static random addresses are durable LE random identities. */
    if (r->addr_type && ((r->addr[5] & 0xc0) != 0xc0 ||
        (all(r->addr, 5, 0) && r->addr[5] == 0xc0) ||
        (all(r->addr, 5, 0xff) && r->addr[5] == 0xff))) return 0;
    return 1;
}

db_rec *bond_store_find(bond_store *s, uint8_t transport, uint8_t type, const uint8_t addr[6])
{
    int i;
    for (i = 0; i < s->count; i++) {
        db_rec *r = &s->records[i];
        if (r->le == transport && r->addr_type == type && !memcmp(r->addr, addr, 6)) return r;
    }
    return NULL;
}

bond_result bond_store_put(bond_store *s, const db_rec *record)
{
    db_rec *r;
    if (!address_ok(s->adapter.addr)) return BOND_ADAPTER;
    if (!record_ok(record) || s->count < 0) return BOND_INVALID;
    if (s->count > BOND_MAX) return BOND_CAPACITY;
    r = bond_store_find(s, record->le, record->addr_type, record->addr);
    if (!r) {
        if (s->count == BOND_MAX) return BOND_CAPACITY;
        r = &s->records[s->count++];
    }
    *r = *record;
    return BOND_COMMITTED;
}

bond_result bond_store_decode(const uint8_t *b, size_t n, const bond_adapter *adapter, bond_store *out)
{
    bond_store s = {0};
    unsigned count, i;
    if (!address_ok(adapter->addr)) return BOND_ADAPTER;
    if (n < BOND_HEADER_SIZE || memcmp(b, "WHPBOND", 8)) return BOND_INVALID;
    if (get16(b + 8) != 1) return BOND_VERSION;
    if (get16(b + 10) != 64 || get16(b + 12) != 64) return BOND_INVALID;
    count = get16(b + 14);
    if (count > BOND_MAX) return BOND_CAPACITY;
    if (n != BOND_HEADER_SIZE + count * BOND_RECORD_SIZE ||
        (get16(b + 26) & ~1u) || !all(b + 44, 20, 0) ||
        (!(b[26] & 1) && !all(b + 28, 16, 0))) return BOND_INVALID;
    if (memcmp(b + 16, adapter->addr, 6) || get16(b + 22) != adapter->vid ||
        get16(b + 24) != adapter->pid) return BOND_ADAPTER;
    s.adapter = *adapter;
    s.has_irk = b[26] & 1;
    memcpy(s.local_irk, b + 28, 16);
    for (i = 0; i < count; i++) {
        const uint8_t *p = b + 64 + i * 64;
        db_rec r = {0};
        bond_result result;
        if ((p[25] & ~15u) || !all(p + 56, 8, 0)) return BOND_INVALID;
        r.le = p[0]; r.addr_type = p[1];
        memcpy(r.addr, p + 2, 6); memcpy(r.key, p + 8, 16);
        r.type = p[24]; r.complete = p[25] & 1;
        r.has_irk = (p[25] >> 1) & 1; r.sc = (p[25] >> 2) & 1;
        r.repair_needed = (p[25] >> 3) & 1;
        r.vid = (uint16_t)get16(p + 26); r.pid = (uint16_t)get16(p + 28);
        r.ediv = (uint16_t)get16(p + 30);
        memcpy(r.rand, p + 32, 8); memcpy(r.irk, p + 40, 16);
        if (bond_store_find(&s, r.le, r.addr_type, r.addr)) return BOND_INVALID;
        result = bond_store_put(&s, &r);
        if (result != BOND_COMMITTED) return result;
    }
    *out = s;
    return BOND_COMMITTED;
}

bond_result bond_store_encode(const bond_store *s, uint8_t b[BOND_FILE_MAX], size_t *len)
{
    bond_store checked;
    bond_result result;
    int i;
    if (s->count < 0 || s->has_irk < 0 || s->has_irk > 1) return BOND_INVALID;
    if (s->count > BOND_MAX) return BOND_CAPACITY;
    memset(b, 0, BOND_FILE_MAX);
    memcpy(b, "WHPBOND", 8);
    put16(b + 8, 1); put16(b + 10, 64); put16(b + 12, 64);
    put16(b + 14, (unsigned)s->count);
    memcpy(b + 16, s->adapter.addr, 6);
    put16(b + 22, s->adapter.vid); put16(b + 24, s->adapter.pid);
    put16(b + 26, (unsigned)s->has_irk);
    memcpy(b + 28, s->local_irk, 16);
    for (i = 0; i < s->count; i++) {
        const db_rec *r = &s->records[i];
        uint8_t *p = b + 64 + i * 64;
        if (!record_ok(r)) return BOND_INVALID;
        p[0] = r->le; p[1] = r->addr_type;
        memcpy(p + 2, r->addr, 6); memcpy(p + 8, r->key, 16);
        p[24] = r->type;
        p[25] = r->complete | r->has_irk << 1 | r->sc << 2 | r->repair_needed << 3;
        put16(p + 26, r->vid); put16(p + 28, r->pid); put16(p + 30, r->ediv);
        memcpy(p + 32, r->rand, 8); memcpy(p + 40, r->irk, 16);
    }
    result = bond_store_decode(b, 64 + (size_t)s->count * 64, &s->adapter, &checked);
    if (result == BOND_COMMITTED) *len = 64 + (size_t)s->count * 64;
    return result;
}

/* Walk every directory component without following symlinks. Only the final
 * state directory may be created or have its permissions restricted. */
static int open_parent(const char *path, char name[256], int create)
{
    char copy[1024], *part, *next, *state;
    int fd, child;
    struct stat st;
    if (!path[0] || strlen(path) >= sizeof copy) { errno = EINVAL; return -1; }
    memcpy(copy, path, strlen(path) + 1);
    fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    part = strtok_r(copy, "/", &state);
    while (part) {
        next = strtok_r(NULL, "/", &state);
        if (!strcmp(part, "..") || !strcmp(part, ".")) { errno = EINVAL; break; }
        if (!next) {
            if (strlen(part) >= 256) { errno = ENAMETOOLONG; break; }
            if (fstat(fd, &st)) break;
            if (st.st_uid != geteuid()) { errno = EPERM; break; }
            if (create && fchmod(fd, 0700)) break;
            memcpy(name, part, strlen(part) + 1);
            return fd;
        }
        child = store_openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
        if (child < 0 && create && errno == ENOENT && !state[0]) {
            if (mkdirat(fd, part, 0700)) break;
            /* Persist creation of the state directory in its parent too. */
            if (fsync(fd)) break;
            child = store_openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
        }
        if (child < 0) break;
        close(fd); fd = child; part = next;
    }
    close(fd);
    return -1;
}

static bond_result load_at(int dir, const char *name, const bond_adapter *adapter, bond_store *out)
{
    uint8_t b[BOND_FILE_MAX + 1];
    size_t n = 0;
    ssize_t got;
    struct stat st;
    int fd = store_openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
    if (fd < 0) {
        if (errno != ENOENT) return BOND_ERROR;
        if (!address_ok(adapter->addr)) return BOND_ADAPTER;
        { bond_store empty = { .adapter = *adapter }; *out = empty; }
        return BOND_MISSING;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        close(fd); return BOND_ERROR;
    }
    do {
        got = read(fd, b + n, sizeof b - n);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { close(fd); return BOND_ERROR; }
        n += (size_t)got;
    } while (got && n < sizeof b);
    if (close(fd)) return BOND_ERROR;
    return bond_store_decode(b, n, adapter, out);
}

bond_result bond_store_load(const char *path, const bond_adapter *adapter, bond_store *out)
{
    char name[256];
    int dir = open_parent(path, name, 0);
    bond_result result;
    if (dir < 0) {
        if (errno != ENOENT) return BOND_ERROR;
        if (!address_ok(adapter->addr)) return BOND_ADAPTER;
        { bond_store empty = { .adapter = *adapter }; *out = empty; }
        return BOND_MISSING;
    }
    result = load_at(dir, name, adapter, out);
    close(dir);
    return result;
}

static int fault(bond_stage stage)
{
#ifdef BOND_STORE_TEST
    if (bond_store_test_fault(stage)) { errno = EIO; return 1; }
#else
    (void)stage;
#endif
    return 0;
}

/* Retain the owned directory/name until cleanup succeeds, even across saves. */
static int temp_dir = -1;
static char temp_name[80];

static int cleanup_temp(void)
{
    if (temp_dir < 0) return 1;
    for (int attempt = 0; temp_name[0] && attempt < 2; attempt++) {
        int result = unlinkat(temp_dir, temp_name, 0);
        if (!result || result == ENOENT || (result < 0 && errno == ENOENT))
            temp_name[0] = 0;
    }
    int synced = !fault(BOND_STAGE_DIR_FSYNC) && fsync(temp_dir) == 0;
    if (!temp_name[0]) { close(temp_dir); temp_dir = -1; }
    return synced;
}

bond_result bond_store_save(const char *path, const bond_store *s)
{
    uint8_t b[BOND_FILE_MAX];
    size_t len;
    char name[256], temp[80];
    bond_store old;
    bond_result result = bond_store_encode(s, b, &len);
    int dir, fd, missing;
    FILE *f;
    static unsigned sequence;
    if (!cleanup_temp() || temp_dir >= 0) return BOND_ERROR;
    if (result != BOND_COMMITTED) return result;
    dir = open_parent(path, name, 1);
    if (dir < 0) return BOND_ERROR;
    result = load_at(dir, name, &s->adapter, &old);
    if (result != BOND_COMMITTED && result != BOND_MISSING) { close(dir); return result; }
    /* Do not replace a file created after the missing-file preflight. */
    missing = result == BOND_MISSING;
    snprintf(temp, sizeof temp, ".bond-%ld-%u.tmp", (long)getpid(), ++sequence);
    fd = store_openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { close(dir); return BOND_ERROR; }
    temp_dir = dir;
    memcpy(temp_name, temp, strlen(temp) + 1);
    f = fdopen(fd, "wb");
    if (!f) { close(fd); cleanup_temp(); return BOND_ERROR; }
    result = BOND_ERROR;
    if (fchmod(fd, 0600) || fault(BOND_STAGE_WRITE) || fwrite(b, 1, len, f) != len ||
        fault(BOND_STAGE_FLUSH) || fflush(f) || fault(BOND_STAGE_FILE_FSYNC) || fsync(fd)) {
        fclose(f);
    } else if (fclose(f) == 0 && !fault(BOND_STAGE_RENAME) &&
               (missing ? linkat(dir, temp, dir, name, 0) : renameat(dir, temp, dir, name)) == 0) {
        result = BOND_COMMITTED;
        if (!missing) temp_name[0] = 0;
    }
    int synced = cleanup_temp();
    if (result == BOND_COMMITTED && !synced) result = BOND_UNCERTAIN;
    return result;
}

const char *bond_store_result_name(bond_result result)
{
    switch (result) {
    case BOND_COMMITTED: return "committed";
    case BOND_ERROR: return "I/O error";
    case BOND_UNCERTAIN: return "durability uncertain";
    case BOND_MISSING: return "missing";
    case BOND_INVALID: return "invalid file or identity";
    case BOND_VERSION: return "unknown version";
    case BOND_ADAPTER: return "adapter identity mismatch or unavailable";
    case BOND_CAPACITY: return "capacity exceeded";
    }
    return "invalid result";
}
