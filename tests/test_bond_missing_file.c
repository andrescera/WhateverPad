/* GPL-3.0-or-later. Real store/host/web with synthetic keys and SDK-style openat. */
#define main web_fixture_main
#include "test_web.c"
#undef main
#include "../src/host_int.h"
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>

static int posix_mode, final_errno, wrong_owner, force_fd2, final_fd = -1;
static int bogus_fd = -1, bogus_stats, bogus_reads, bogus_closes, fd2_reads;

int __real_openat(int, const char *, int, ...);
int __real_fstat(int, struct stat *);
ssize_t __real_read(int, void *, size_t);
int __real_close(int);

long bond_store_test_openat(int dir, const char *name, int flags, mode_t mode,
                           unsigned char *failed)
{
    int fd;
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
            CHECK(dup2(fd, 2) == 2);
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
    int result = __real_fstat(fd, st);
    if (!result && wrong_owner && fd == final_fd) st->st_uid = geteuid() ^ 1u;
    return result;
}

ssize_t __wrap_read(int fd, void *buf, size_t len)
{
    if (fd == bogus_fd) { bogus_reads++; errno = EIO; return -1; }
    if (fd == 2) fd2_reads++;
    return __real_read(fd, buf, len);
}

int __wrap_close(int fd)
{
    if (fd == bogus_fd) {
        bogus_fd = -1;
        bogus_closes++;
        return 0; /* Never close an unrelated descriptor in the RED run. */
    }
    if (fd == final_fd) final_fd = -1;
    return __real_close(fd);
}

static void rejected(const char *path, const bond_store *saved, bond_result want)
{
    bond_store out = *saved;
    CHECK(bond_store_load(path, &saved->adapter, &out) == want);
    CHECK(!memcmp(&out, saved, sizeof out));
    CHECK(bond_store_save(path, saved) == want);
}

static void store_cases(const char *dir, const char *path)
{
    bond_adapter adapter = {.addr = {1,2,3,4,5,6}, .vid = 1, .pid = 2};
    bond_store empty = {.adapter = adapter}, out = empty, saved = empty, reopened;
    db_rec record = {.addr = {7,8,9,10,11,12}, .complete = 1, .type = 4};
    struct stat st;
    char absent[256], parent[256], bytes[8] = {0};
    int fd;

    out.count = 9;
    CHECK(bond_store_load(path, &out.adapter, &out) == BOND_MISSING);
    CHECK(!memcmp(&out, &empty, sizeof out));
    CHECK(bogus_stats == 0 && bogus_reads == 0 && bogus_closes == 0);
    CHECK(lstat(path, &st) == -1 && errno == ENOENT);
    if (g_fail) return; /* Do not use a failed snapshot in the later transactions. */
    snprintf(parent, sizeof parent, "%s/missing", dir);
    snprintf(absent, sizeof absent, "%s/missing/pads.db", dir);
    CHECK(bond_store_load(absent, &adapter, &out) == BOND_MISSING);
    CHECK(!memcmp(&out, &empty, sizeof out));
    memset(record.key, 0x42, sizeof record.key);
    CHECK(bond_store_put(&saved, &record) == BOND_COMMITTED);
    saved.has_irk = 1;
    memset(saved.local_irk, 0x31, sizeof saved.local_irk);
    CHECK(bond_store_save(absent, &saved) == BOND_COMMITTED);
    CHECK(bond_store_load(absent, &adapter, &reopened) == BOND_COMMITTED);
    CHECK(!memcmp(&saved, &reopened, sizeof saved));
    unlink(absent);
    rmdir(parent);
    CHECK(bond_store_save(path, &saved) == BOND_COMMITTED);
    CHECK(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    CHECK(!memcmp(&saved, &reopened, sizeof saved));
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* Carry clear with a real fd 2 is not ENOENT, even with stale errno. */
    int stderr_copy = fcntl(2, F_DUPFD_CLOEXEC, 64);
    CHECK(stderr_copy >= 0);
    if (stderr_copy >= 0) {
        force_fd2 = 1;
        errno = ENOENT;
        CHECK(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
        CHECK(!memcmp(&saved, &reopened, sizeof saved) && fd2_reads > 0);
        CHECK(dup2(stderr_copy, 2) == 2);
        force_fd2 = 0;
        CHECK(close(stderr_copy) == 0);
    }
    wrong_owner = 1;
    rejected(path, &saved, BOND_ERROR);
    wrong_owner = 0;
    CHECK(bond_store_load(path, &adapter, &reopened) == BOND_COMMITTED);
    CHECK(!memcmp(&saved, &reopened, sizeof saved));
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
    CHECK(!memcmp(&out, &saved, sizeof out));
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

int main(int argc, char **argv)
{
    char dir[] = "/tmp/whateverpad-bond-missing-XXXXXX", path[256];
    setvbuf(stdout, NULL, _IOLBF, 0);
    posix_mode = argc == 2 && !strcmp(argv[1], "--posix");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/pads.db", dir);
    store_cases(dir, path);
    paired_state(path);
    CHECK(rmdir(dir) == 0);
    printf("bond missing file: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
