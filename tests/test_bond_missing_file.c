/* GPL-3.0-or-later. Real store/host/web with synthetic keys and SDK-style openat. */
#define main web_fixture_main
#include "test_web.c"
#undef main
#include "../src/host_int.h"
#include <fcntl.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>

static int posix_mode, final_errno, wrong_owner, force_fd2, final_fd = -1;
static int bogus_fd = -1, bogus_stats, bogus_reads, bogus_closes, fd2_reads;
static int private_fd = -1;
static int race_publish, temp_collision, fail_sync, fail_temp_unlink, dir_syncs;
static char collision_name[80];
static const char corrupt[] = "concurrent-corrupt", collision[] = "colliding-temp";

int __real_openat(int, const char *, int, ...);
int __real_fstat(int, struct stat *);
ssize_t __real_read(int, void *, size_t);
ssize_t __real___read_chk(int, void *, size_t, size_t);
int __real_close(int);
int __real_fsync(int);
int __real_renameat(int, const char *, int, const char *);
int __real_linkat(int, const char *, int, const char *, int);
int __real_unlinkat(int, const char *, int);

long bond_store_test_openat(int dir, const char *name, int flags, mode_t mode,
                           unsigned char *failed)
{
    int fd;
    if (temp_collision && !strncmp(name, ".bond-", 6)) {
        temp_collision = 0;
        snprintf(collision_name, sizeof collision_name, "%s", name);
        fd = __real_openat(dir, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(write(fd, collision, sizeof collision - 1) == sizeof collision - 1);
            CHECK(__real_close(fd) == 0);
        }
    }
    if (final_errno && !strcmp(name, "pads.db")) {
        errno = final_errno;
        fd = -1;
    } else fd = __real_openat(dir, name, flags, mode);
    *failed = fd < 0;
    if (fd < 0) {
        int error = errno;
        errno = EINTR; /* Raw kernel errors leave libc errno unchanged. */
        return error;
    }
    if (!strcmp(name, "pads.db")) {
        if (force_fd2) {
            /* Logical fd 2 maps to a private file, never the process's stderr. */
            private_fd = fcntl(fd, F_DUPFD_CLOEXEC, 64);
            CHECK(private_fd >= 0);
            if (private_fd < 0) {
                int error = errno;
                CHECK(__real_close(fd) == 0);
                *failed = 1;
                return error;
            }
            CHECK(__real_close(fd) == 0);
            fd = 2;
        }
        final_fd = fd;
    }
    return fd;
}

/* Also exercises the unchanged loader, which calls the SDK stub directly. */
int __wrap_openat(int dir, const char *name, int flags, ...)
{
    mode_t mode = 0;
    unsigned char failed;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    long result = bond_store_test_openat(dir, name, flags, mode, &failed);
    if (failed) {
        if (posix_mode) { errno = (int)result; return -1; }
        bogus_fd = (int)result;
    }
    return (int)result;
}

int __wrap_fstat(int fd, struct stat *st)
{
    if (fd == bogus_fd) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFCHR | 0600;
        st->st_uid = geteuid();
        bogus_stats++;
        return 0;
    }
    int result = __real_fstat(fd == 2 && private_fd >= 0 ? private_fd : fd, st);
    if (!result && wrong_owner && fd == final_fd) st->st_uid = geteuid() ^ 1u;
    return result;
}

ssize_t __wrap_read(int fd, void *buf, size_t len)
{
    if (fd == bogus_fd) { bogus_reads++; errno = EIO; return -1; }
    if (fd == 2 && private_fd >= 0) { fd2_reads++; fd = private_fd; }
    return __real_read(fd, buf, len);
}

ssize_t __wrap___read_chk(int fd, void *buf, size_t len, size_t capacity)
{
    if (fd == bogus_fd) { bogus_reads++; errno = EIO; return -1; }
    if (fd == 2 && private_fd >= 0) { fd2_reads++; fd = private_fd; }
    return __real___read_chk(fd, buf, len, capacity);
}

int __wrap_close(int fd)
{
    if (fd == bogus_fd) {
        bogus_fd = -1;
        bogus_closes++;
        return 0; /* Never close an unrelated descriptor in the RED run. */
    }
    if (fd == final_fd) final_fd = -1;
    if (fd == 2 && private_fd >= 0) {
        int owned = private_fd;
        private_fd = -1;
        return __real_close(owned);
    }
    return __real_close(fd);
}

int __wrap_fsync(int fd)
{
    struct stat st;
    int known = __real_fstat(fd, &st) == 0;
    if (known && S_ISDIR(st.st_mode)) dir_syncs++;
    if (fail_sync && known &&
        (S_ISDIR(st.st_mode) ? fail_sync == 2 : fail_sync == 1)) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static void concurrent_destination(int dir, const char *name)
{
    int kind = race_publish;
    race_publish = 0;
    if (kind == 1) {
        int fd = __real_openat(dir, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(write(fd, corrupt, sizeof corrupt - 1) == sizeof corrupt - 1);
            CHECK(__real_close(fd) == 0);
        }
    } else if (kind == 2) CHECK(mkdirat(dir, name, 0700) == 0);
    else if (kind == 3) CHECK(symlinkat("absent", dir, name) == 0);
}

int __wrap_renameat(int from, const char *temp, int to, const char *name)
{
    concurrent_destination(to, name);
    return __real_renameat(from, temp, to, name);
}

int __wrap_linkat(int from, const char *temp, int to, const char *name, int flags)
{
    concurrent_destination(to, name);
    return __real_linkat(from, temp, to, name, flags);
}

int __wrap_unlinkat(int dir, const char *name, int flags)
{
    if (fail_temp_unlink && !strncmp(name, ".bond-", 6)) {
        if (fail_temp_unlink > 0) fail_temp_unlink--;
        errno = EIO;
        return -1;
    }
    return __real_unlinkat(dir, name, flags);
}

static int descriptors(void)
{
    DIR *dir = opendir("/dev/fd");
    struct dirent *entry;
    int count = 0;
    CHECK(dir != NULL);
    if (!dir) return -1;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') count++;
    CHECK(closedir(dir) == 0);
    return count;
}

static int temp_files(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    int count = 0;
    CHECK(dir != NULL);
    if (!dir) return -1;
    while ((entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            strcmp(entry->d_name, "pads.db")) count++;
    CHECK(closedir(dir) == 0);
    return count;
}

static void same_bytes(const char *path, const char *bytes, size_t len)
{
    char got[BOND_FILE_MAX + 1];
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(read(fd, got, sizeof got) == (ssize_t)len && !memcmp(got, bytes, len));
    CHECK(close(fd) == 0);
}

static int same_store(const bond_store *a, const bond_store *b)
{
    if (memcmp(a->adapter.addr, b->adapter.addr, 6) || a->adapter.vid != b->adapter.vid ||
        a->adapter.pid != b->adapter.pid || a->count != b->count || a->has_irk != b->has_irk ||
        memcmp(a->local_irk, b->local_irk, 16)) return 0;
    for (int i = 0; i < BOND_MAX; i++) {
        const db_rec *x = &a->records[i], *y = &b->records[i];
        if (memcmp(x->addr, y->addr, 6) || memcmp(x->key, y->key, 16) || x->type != y->type ||
            x->vid != y->vid || x->pid != y->pid || x->le != y->le ||
            x->addr_type != y->addr_type || x->ediv != y->ediv || memcmp(x->rand, y->rand, 8) ||
            memcmp(x->irk, y->irk, 16) || x->has_irk != y->has_irk ||
            x->complete != y->complete || x->sc != y->sc || x->repair_needed != y->repair_needed)
            return 0;
    }
    return 1;
}

static void rejected(const char *path, const bond_store *saved, bond_result want)
{
    bond_store out = *saved;
    CHECK(bond_store_load(path, &saved->adapter, &out) == want);
    CHECK(same_store(&out, saved));
    CHECK(bond_store_save(path, saved) == want);
}

static void store_cases(const char *dir, const char *path)
{
    bond_adapter adapter = {.addr = {1,2,3,4,5,6}, .vid = 1, .pid = 2};
    bond_store empty = {.adapter = adapter}, out = empty, saved = empty, reopened = {0};
    db_rec record = {.addr = {7,8,9,10,11,12}, .complete = 1, .type = 4};
    struct stat st;
    char absent[256], parent[256], bytes[8] = {0};
    int fd;

    out.count = 9;
    CHECK(bond_store_load(path, &out.adapter, &out) == BOND_MISSING);
    CHECK(same_store(&out, &empty));
    CHECK(bogus_stats == 0 && bogus_reads == 0 && bogus_closes == 0);
    CHECK(lstat(path, &st) == -1 && errno == ENOENT);
    if (g_fail) return; /* Do not use a failed snapshot in the later transactions. */
    snprintf(parent, sizeof parent, "%s/missing", dir);
    snprintf(absent, sizeof absent, "%s/missing/pads.db", dir);
    CHECK(bond_store_load(absent, &adapter, &out) == BOND_MISSING);
    CHECK(same_store(&out, &empty));
    memset(record.key, 0x42, sizeof record.key);
    CHECK(bond_store_put(&saved, &record) == BOND_COMMITTED);
    saved.has_irk = 1;
    memset(saved.local_irk, 0x31, sizeof saved.local_irk);
    CHECK(bond_store_save(absent, &saved) == BOND_COMMITTED);
    CHECK(bond_store_load(absent, &adapter, &reopened) == BOND_COMMITTED);
    CHECK(same_store(&saved, &reopened));
    unlink(absent);
    rmdir(parent);
    CHECK(bond_store_save(path, &saved) == BOND_COMMITTED);
    CHECK(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    CHECK(same_store(&saved, &reopened));
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* Carry clear with logical fd 2 is not ENOENT, even with stale errno. */
    int before = fd2_reads;
    force_fd2 = 1;
    errno = ENOENT;
    CHECK(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    CHECK(same_store(&saved, &reopened));
    CHECK(fd2_reads > before && private_fd == -1);
    if (private_fd >= 0) { CHECK(__real_close(private_fd) == 0); private_fd = -1; }
    force_fd2 = 0;
    wrong_owner = 1;
    rejected(path, &saved, BOND_ERROR);
    wrong_owner = 0;
    CHECK(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    CHECK(same_store(&saved, &reopened));
    unlink(path);
    CHECK(mkdir(path, 0700) == 0);
    rejected(path, &saved, BOND_ERROR);
    CHECK(lstat(path, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(rmdir(path) == 0);
    CHECK(mkfifo(path, 0600) == 0);
    rejected(path, &saved, BOND_ERROR);
    CHECK(lstat(path, &st) == 0 && S_ISFIFO(st.st_mode));
    CHECK(unlink(path) == 0);
    CHECK(symlink("absent", path) == 0);
    rejected(path, &saved, BOND_ERROR);
    CHECK(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(unlink(path) == 0);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && write(fd, "corrupt", 7) == 7 && close(fd) == 0);
    rejected(path, &saved, BOND_INVALID);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && read(fd, bytes, sizeof bytes) == 7 && close(fd) == 0);
    CHECK(!memcmp(bytes, "corrupt", 7));
    CHECK(unlink(path) == 0);
    const int errors[] = {EINVAL, EACCES, EIO};
    for (unsigned i = 0; i < sizeof errors / sizeof *errors; i++) {
        final_errno = errors[i];
        rejected(path, &saved, BOND_ERROR);
        CHECK(lstat(path, &st) == -1 && errno == ENOENT);
    }
    final_errno = 0;
    bogus_fd = -1;
    bond_adapter invalid = {0};
    out = saved;
    CHECK(bond_store_load(path, &invalid, &out) == BOND_ADAPTER);
    CHECK(same_store(&out, &saved));
    CHECK(bogus_stats == 0 && bogus_reads == 0 && bogus_closes == 0);
}

static void paired_state(const char *path)
{
    sim_pad pad = {.mode = SIM_WAIT_PAIRING, .addr = {1,2,3,4,5,6},
        .cod = 0x002508, .vid = 0x045e, .pid = 0x02e0, .report_len = 17};
    sim_t *sim = sim_new(&pad);
    host_events ev = {.idle = web_idle};
    volatile int stop = 0;
    web_cfg cfg = {.host = &g_host, .stop = &stop, .bt_state = &g_bt_state};
    char out[16384];
    bond_store reopened = {0};
    g_port = 22000 + getpid() % 10000;
    g_bt_state = WEB_BT_OK;
    attrib_reset();
    g_host = sim_open_host(sim, path, &ev);
    CHECK(g_host != NULL);
    if (!g_host) { sim_free(sim); return; }
    g_web = web_start(&cfg, PORT);
    CHECK(g_web != NULL);
    if (g_web) {
        CHECK(get("POST", "/api/pair", out, sizeof out) > 0);
        CHECK(strstr(out, "HTTP/1.1 200") != NULL);
        run(15000); /* Virtual controller events, no wall-clock wait. */
        CHECK(host_connected(g_host, 0));
        CHECK(host_paired_count(g_host) == 1);
        CHECK(get("GET", "/api/state", out, sizeof out) > 0);
        CHECK(strstr(out, "\"pads\":[{\"slot\":1") != NULL);
        CHECK(strstr(out, "\"paired\":[{\"addr\":\"06:05:04:03:02:01\"") != NULL);
        CHECK(strstr(out, "\"pid\":\"02e0\",\"le\":false,\"connected\":true") != NULL);
        CHECK(bond_store_load(path, &g_host->store.adapter, &reopened) == BOND_COMMITTED);
        CHECK(reopened.count == 1 && reopened.records[0].complete);
        host_close(g_host);
        g_host = NULL;
        sim_free(sim);
        sim = sim_new(&pad);
        attrib_reset();
        g_host = sim_open_host(sim, path, &ev);
        CHECK(g_host != NULL);
        if (g_host) {
            CHECK(host_paired_count(g_host) == 1);
            CHECK(get("GET", "/api/state", out, sizeof out) > 0);
            CHECK(strstr(out, "\"paired\":[{\"addr\":\"06:05:04:03:02:01\"") != NULL);
        }
        web_stop(g_web);
        g_web = NULL;
    }
    if (g_host) host_close(g_host);
    g_host = NULL;
    sim_free(sim);
    ph_clock_hook = NULL;
    unlink(path);
}

static void transactions(const char *dir, const char *path)
{
    bond_adapter adapter = {.addr = {1,2,3,4,5,6}, .vid = 1, .pid = 2};
    bond_store saved = {.adapter = adapter}, next, out = {0};
    db_rec record = {.addr = {7,8,9,10,11,12}, .complete = 1, .type = 4};
    struct stat st;
    char temp[512];
    int initial = descriptors();
    CHECK(bond_store_put(&saved, &record) == BOND_COMMITTED);
    for (int kind = 1; kind <= 3; kind++) {
        race_publish = kind;
        bond_result result = bond_store_save(path, &saved);
        CHECK(result == BOND_ERROR && !race_publish);
        if (kind == 1) {
            same_bytes(path, corrupt, sizeof corrupt - 1);
            CHECK(unlink(path) == 0);
        } else if (kind == 2) {
            CHECK(lstat(path, &st) == 0 && S_ISDIR(st.st_mode));
            CHECK(rmdir(path) == 0);
        } else {
            CHECK(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
            CHECK(unlink(path) == 0);
        }
        CHECK(temp_files(dir) == 0);
        CHECK(descriptors() == initial);
    }

    CHECK(chmod(dir, 0777) == 0);
    CHECK(bond_store_save(path, &saved) == BOND_COMMITTED);
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    mode_t mask = umask(0777);
    bond_result restricted = bond_store_save(path, &saved);
    umask(mask);
    CHECK(restricted == BOND_COMMITTED);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(chmod(path, 0600) == 0); /* Restore the fixture even for the chmod mutant. */
    next = saved;
    next.records[0].key[0] = 0x42;
    for (int missing = 0; missing <= 1; missing++) {
        if (missing) CHECK(unlink(path) == 0);
        fail_sync = 1;
        CHECK(bond_store_save(path, &next) == BOND_ERROR);
        fail_sync = 0;
        if (missing) CHECK(lstat(path, &st) == -1 && errno == ENOENT);
        else {
            CHECK(bond_store_load(path, &adapter, &out) == BOND_COMMITTED);
            CHECK(same_store(&saved, &out));
        }
        fail_sync = 2;
        CHECK(bond_store_save(path, &next) == BOND_UNCERTAIN);
        fail_sync = 0;
        CHECK(bond_store_load(path, &adapter, &out) == BOND_COMMITTED);
        CHECK(same_store(&next, &out));
        CHECK(temp_files(dir) == 0);
        CHECK(descriptors() == initial);
    }
    temp_collision = 1;
    CHECK(bond_store_save(path, &saved) == BOND_ERROR);
    CHECK(!temp_collision);
    snprintf(temp, sizeof temp, "%s/%s", dir, collision_name);
    same_bytes(temp, collision, sizeof collision - 1);
    CHECK(bond_store_load(path, &adapter, &out) == BOND_COMMITTED);
    CHECK(same_store(&next, &out));
    CHECK(unlink(temp) == 0);
    CHECK(unlink(path) == 0);

    int synced = dir_syncs;
    fail_temp_unlink = 1;
    bond_result recovered = bond_store_save(path, &saved);
    CHECK(recovered == BOND_COMMITTED || recovered == BOND_UNCERTAIN);
    CHECK(dir_syncs > synced);
    CHECK(bond_store_load(path, &adapter, &out) == BOND_COMMITTED);
    CHECK(same_store(&saved, &out));
    CHECK(bond_store_save(path, &next) == BOND_COMMITTED);
    CHECK(bond_store_load(path, &adapter, &out) == BOND_COMMITTED);
    CHECK(same_store(&next, &out));
    CHECK(temp_files(dir) == 0);
    CHECK(unlink(path) == 0);
    CHECK(descriptors() == initial);

    for (int stage = 0; stage < 3; stage++) {
        fail_temp_unlink = -1;
        fail_sync = stage == 0 ? 1 : 0;
        race_publish = stage == 2 ? 1 : 0;
        synced = dir_syncs;
        bond_result unresolved = bond_store_save(path, &saved);
        CHECK(stage == 1 ? unresolved == BOND_COMMITTED || unresolved == BOND_UNCERTAIN :
                          unresolved == BOND_ERROR);
        CHECK(dir_syncs > synced);
        if (stage == 1) {
            CHECK(bond_store_load(path, &adapter, &out) == BOND_COMMITTED);
            CHECK(same_store(&saved, &out));
        }
        fail_temp_unlink = fail_sync = 0;
        if (stage == 2) {
            CHECK(bond_store_save(path, &next) == BOND_INVALID);
            same_bytes(path, corrupt, sizeof corrupt - 1);
            CHECK(temp_files(dir) == 0);
            CHECK(unlink(path) == 0);
        }
        CHECK(bond_store_save(path, &next) == BOND_COMMITTED);
        CHECK(bond_store_load(path, &adapter, &out) == BOND_COMMITTED);
        CHECK(same_store(&next, &out));
        CHECK(temp_files(dir) == 0);
        CHECK(descriptors() == initial);
        CHECK(unlink(path) == 0);
    }
}

int main(int argc, char **argv)
{
    char dir[] = "/tmp/whateverpad-bond-missing-XXXXXX", path[256], log[256];
    setvbuf(stdout, NULL, _IOLBF, 0);
    posix_mode = argc == 2 && !strcmp(argv[1], "--posix");
    CHECK(mkdtemp(dir) != NULL);
    CHECK(chmod(dir, 0700) == 0);
    snprintf(path, sizeof path, "%s/pads.db", dir);
    store_cases(dir, path);
    transactions(dir, path);
    /* A closed stderr may become a socket; keep host logging on its own file. */
    snprintf(log, sizeof log, "%s/host.log", dir);
    int logging = log_open(log);
    CHECK(logging);
    if (logging) {
        paired_state(path);
        log_close();
        CHECK(unlink(log) == 0);
    }
    CHECK(rmdir(dir) == 0);
    printf("bond missing file: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
