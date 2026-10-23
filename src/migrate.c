#include "migrate.h"
#include "config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MIGRATE_TEST
#define fsync migrate_test_fsync
#endif

typedef struct {
    migrate_report report;
    void *ud;
    int problems;
} migration;

static void emit(migration *m, const char *path, migrate_status status)
{
    if (status >= MIGRATE_CONFLICT) m->problems++;
    m->report(m->ud, path, status);
}

const char *migrate_status_name(migrate_status status)
{
    switch (status) {
    case MIGRATE_COPIED: return "copied";
    case MIGRATE_FILTERED: return "legacy LAN/developer opt-ins not imported";
    case MIGRATE_PROTECTED: return "protected legacy credentials left untouched";
    case MIGRATE_CONFLICT: return "conflict: existing destination kept";
    case MIGRATE_INVALID: return "invalid or oversized legacy input";
    case MIGRATE_SYMLINK: return "symlink refused";
    case MIGRATE_IO_ERROR: return "I/O error: source preserved";
    case MIGRATE_UNCERTAIN: return "complete destination present; durability uncertain";
    }
    return "invalid migration status";
}

static migrate_status io_status(void)
{
    return errno == ELOOP ? MIGRATE_SYMLINK : MIGRATE_IO_ERROR;
}

static int directory_at(int parent, const char *name, int create)
{
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) {
        if (!create || errno != ENOENT || mkdirat(parent, name, 0700)) return -1;
    } else if (S_ISLNK(st.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    /* An existing name can be left by a failed or interrupted creation. */
    if (create && fsync(parent)) return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* Resolve even ancestor components through directory descriptors, not paths. */
static int open_root(const char *path, int create)
{
    char copy[1024], *part, *next, *state;
    int dir, child, saved;
    if (!path || !*path || strnlen(path, sizeof copy) == sizeof copy) {
        errno = EINVAL; return -1;
    }
    strcpy(copy, path);
    dir = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    part = strtok_r(copy, "/", &state);
    if (!part) { close(dir); errno = EINVAL; return -1; }
    while (part) {
        next = strtok_r(NULL, "/", &state);
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            close(dir); errno = EINVAL; return -1;
        }
        child = directory_at(dir, part, create && !next);
        saved = errno;
        close(dir);
        if (child < 0) { errno = saved; return -1; }
        dir = child;
        part = next;
    }
    return dir;
}

static int writable(int dir)
{
    struct stat st;
    if (fstat(dir, &st)) return 0;
    if (!(st.st_mode & 0222)) { errno = EACCES; return 0; }
    return 1;
}

static int fault(migrate_stage stage)
{
#ifdef MIGRATE_TEST
    if (migrate_test_fault(stage)) { errno = EIO; return 1; }
#else
    (void)stage;
#endif
    return 0;
}

static int write_all(int fd, const char *bytes, size_t len)
{
    while (len) {
        ssize_t n = write(fd, bytes, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Only our private transaction files can be recovered; arbitrary stale files
 * and symlinks are never unlinked, followed or truncated. */
static int open_stage(int dir, const char *name, int create)
{
    struct stat st;
    struct dirent *e;
    int fd = directory_at(dir, name, create), ok = 1;
    DIR *d;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 077) ||
        flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd); errno = EACCES; return -1;
    }
    d = fdopendir(dup(fd));
    if (!d) { close(fd); return -1; }
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (strcmp(e->d_name, "pending") && strcmp(e->d_name, "ready")) { ok = 0; break; }
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) ||
            st.st_nlink > (!strcmp(e->d_name, "ready") ? 2 : 1)) { ok = 0; break; }
        errno = 0;
    }
    if (errno) ok = 0;
    if (closedir(d)) ok = 0;
    if (!ok) { close(fd); errno = EINVAL; return -1; }
    return fd;
}

static int clear_stage(int fd)
{
    if (unlinkat(fd, "pending", 0) && errno != ENOENT) return -1;
    if (unlinkat(fd, "ready", 0) && errno != ENOENT) return -1;
    return fsync(fd);
}

static int finish_stage(int dir, int fd, const char *name)
{
    if (clear_stage(fd) || unlinkat(dir, name, AT_REMOVEDIR) || fsync(dir)) return -1;
    return 0;
}

static int text_ok(const char *bytes, size_t len, size_t line_max)
{
    size_t i, line = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '\n') line = 0;
        else {
            if (++line > line_max || (c < 32 && c != '\r' && c != '\t')) return 0;
        }
    }
    return 1;
}

static void copy_file(migration *m, int from, int to, const char *name, const char *relative)
{
    struct stat st;
    char scratch[64], *bytes;
    size_t len = 0, max = !strcmp(name, "config.ini") ? CONFIG_MAX_BYTES : MIGRATE_FILE_MAX;
    int input, stage, output, result;
    migrate_status status = MIGRATE_IO_ERROR;

    if (fstatat(from, name, &st, AT_SYMLINK_NOFOLLOW)) {
        if (errno != ENOENT) emit(m, relative, io_status());
        return;
    }
    snprintf(scratch, sizeof scratch, ".whp-migrate-%s", name);
    if (fstatat(to, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        emit(m, relative, MIGRATE_CONFLICT);
        stage = open_stage(to, scratch, 0);
        if (stage >= 0) {
            if (finish_stage(to, stage, scratch)) emit(m, relative, MIGRATE_IO_ERROR);
            close(stage);
        } else if (errno != ENOENT) emit(m, relative, io_status());
        return;
    }
    if (errno != ENOENT) { emit(m, relative, io_status()); return; }
    input = openat(from, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (input < 0) { emit(m, relative, io_status()); return; }
    if (fstat(input, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uintmax_t)st.st_size > max) {
        close(input); emit(m, relative, MIGRATE_INVALID); return;
    }
    bytes = malloc(max + 1);
    if (!bytes) { close(input); emit(m, relative, MIGRATE_IO_ERROR); return; }
    for (;;) {
        ssize_t n = read(input, bytes + len, max + 1 - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        len += (size_t)n;
        if (!n || len > max) { status = MIGRATE_INVALID; break; }
    }
    result = close(input);
    if (result) status = MIGRATE_IO_ERROR;
    if (status == MIGRATE_IO_ERROR) goto done;
    if (len > max || !text_ok(bytes, len, !strcmp(name, "ban.txt") ? 95 : CONFIG_LINE_MAX)) goto done;
    if (!strcmp(name, "config.ini")) {
        whateverpad_config c;
        char combo[96];
        config_defaults(&c);
        if (config_parse(&c, bytes, len)) goto done;
        if (c.lan_access || c.developer_mode) emit(m, relative, MIGRATE_FILTERED);
        config_combo_keys(c.menu_combo, combo, sizeof combo);
        len = (size_t)snprintf(bytes, max + 1,
            "menu_combo=%s\nmenu_hold_ms=%d\nauto_reconnect=%d\n"
            "lan_access=0\ndeveloper_mode=0\nnotifications=%d\n",
            combo, c.menu_hold_ms, c.auto_reconnect, c.notifications);
    }
    if (!writable(to)) { status = MIGRATE_IO_ERROR; goto done; }
    stage = open_stage(to, scratch, 1);
    if (stage < 0) {
        status = errno == EINVAL ? MIGRATE_CONFLICT : io_status();
        goto done;
    }
    status = MIGRATE_IO_ERROR;
    if (clear_stage(stage)) goto close_stage;
    output = openat(stage, "pending", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) goto close_stage;
    result = write_all(output, bytes, len);
    if (!result) result = fault(MIGRATE_STAGE_WRITE) || fault(MIGRATE_STAGE_FILE_FSYNC) || fsync(output);
    if (close(output)) result = -1;
    if (result || fault(MIGRATE_STAGE_RENAME) ||
        renameat(stage, "pending", stage, "ready") || fsync(stage) || fault(MIGRATE_STAGE_PUBLISH))
        goto close_stage;
    /* The final creation is no-replace even when a user writes during migration. */
    if (linkat(stage, "ready", to, name, 0)) {
        if (errno == EEXIST) {
            status = MIGRATE_CONFLICT;
            if (finish_stage(to, stage, scratch)) emit(m, relative, MIGRATE_IO_ERROR);
        }
        goto close_stage;
    }
    status = MIGRATE_UNCERTAIN;
    if (fault(MIGRATE_STAGE_DIR_FSYNC) || fsync(to) || finish_stage(to, stage, scratch))
        goto close_stage;
    status = MIGRATE_COPIED;
close_stage:
    close(stage);
done:
    free(bytes);
    emit(m, relative, status);
}

static int map_name(const char *name)
{
    size_t i;
    if (strlen(name) != 13 || name[4] != '_' || strcmp(name + 9, ".map")) return 0;
    for (i = 0; i < 9; i++)
        if (i != 4 && !isxdigit((unsigned char)name[i])) return 0;
    return 1;
}

static void copy_maps(migration *m, int source, int destination)
{
    struct stat st;
    struct dirent *e;
    int from, to, conflict;
    unsigned n = 0;
    DIR *d;

    from = directory_at(source, "maps", 0);
    if (from < 0) {
        if (errno != ENOENT) emit(m, "maps", io_status());
        return;
    }
    conflict = fstatat(destination, "maps", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
               !S_ISDIR(st.st_mode);
    if (conflict) {
        close(from); emit(m, "maps", MIGRATE_CONFLICT); return;
    }
    to = directory_at(destination, "maps", 1);
    if (to < 0) { close(from); emit(m, "maps", io_status()); return; }
    d = fdopendir(from);
    if (!d) { close(from); close(to); emit(m, "maps", MIGRATE_IO_ERROR); return; }
    errno = 0;
    while ((e = readdir(d))) {
        char relative[272];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (++n > MIGRATE_MAPS_MAX) { emit(m, "maps", MIGRATE_INVALID); break; }
        snprintf(relative, sizeof relative, "maps/%s", e->d_name);
        if (!map_name(e->d_name)) emit(m, relative, MIGRATE_INVALID);
        else copy_file(m, from, to, e->d_name, relative);
        errno = 0;
    }
    if (!e && errno) emit(m, "maps", MIGRATE_IO_ERROR);
    if (closedir(d)) emit(m, "maps", MIGRATE_IO_ERROR);
    close(to);
}

int migrate_legacy(const char *source, const char *destination, migrate_report report, void *ud)
{
    migration m = { report, ud, 0 };
    struct stat a, b;
    int from, to;
    static const char *files[] = { "config.ini", "ps_combos.txt", "ban.txt" };
    static const char *protected[] = { "pads.db", ".irk" };
    size_t i;

    if (!report) return 1;
    from = open_root(source, 0);
    if (from < 0) {
        if (errno != ENOENT) emit(&m, "source", io_status());
        return m.problems;
    }
    to = open_root(destination, 1);
    if (to < 0) { emit(&m, "destination", io_status()); close(from); return m.problems; }
    if (fstat(from, &a) || fstat(to, &b) || (a.st_dev == b.st_dev && a.st_ino == b.st_ino)) {
        emit(&m, "destination", MIGRATE_INVALID);
    } else {
        for (i = 0; i < sizeof files / sizeof files[0]; i++)
            copy_file(&m, from, to, files[i], files[i]);
        copy_maps(&m, from, to);
        for (i = 0; i < sizeof protected / sizeof protected[0]; i++)
            if (fstatat(from, protected[i], &a, AT_SYMLINK_NOFOLLOW) == 0)
                emit(&m, protected[i], MIGRATE_PROTECTED);
    }
    close(to);
    close(from);
    return m.problems;
}
