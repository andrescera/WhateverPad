/* GPL-3.0-or-later. Synthetic files only; deterministic interruption, no sleeps. */
#include "../src/migrate.h"
#include "../src/config.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); abort(); \
} } while (0)

static char root[128], legacy[160], dest[160];
static int fail_stage, crash_stage, race;
static dev_t sync_parent_dev;
static ino_t sync_parent_ino;
static int sync_parent_armed, sync_fail_once, sync_attempts, sync_successes, premature_copy;
static struct { migrate_status status; char path[256]; } reports[512];
static unsigned nreports;
static const char config[] = "menu_combo=l1+r1\nmenu_hold_ms=2400\n"
    "auto_reconnect=0\nnotifications=0\nlan_access=1\ndeveloper_mode=1\n";

static void path(char out[512], const char *base, const char *name)
{
    CHECK(snprintf(out, 512, "%s/%s", base, name) < 512);
}

static void put(const char *base, const char *name, const char *bytes, size_t len)
{
    char p[512];
    FILE *f;
    path(p, base, name);
    f = fopen(p, "wb"); CHECK(f);
    CHECK(fwrite(bytes, 1, len, f) == len);
    CHECK(fclose(f) == 0);
}

static void text(const char *base, const char *name, const char *s)
{
    put(base, name, s, strlen(s));
}

static void same(const char *base, const char *name, const char *s)
{
    char p[512], b[MIGRATE_FILE_MAX + 1];
    FILE *f;
    size_t n;
    path(p, base, name);
    f = fopen(p, "rb"); CHECK(f);
    n = fread(b, 1, sizeof b, f);
    CHECK(!ferror(f) && fclose(f) == 0);
    CHECK(n == strlen(s) && memcmp(b, s, n) == 0);
}

static int exists(const char *base, const char *name)
{
    char p[512];
    struct stat st;
    path(p, base, name);
    if (lstat(p, &st) == 0) return 1;
    CHECK(errno == ENOENT);
    return 0;
}

static void remove_tree(const char *base)
{
    DIR *d = opendir(base);
    struct dirent *e;
    CHECK(d);
    while ((e = readdir(d))) {
        char p[512];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        path(p, base, e->d_name); CHECK(lstat(p, &st) == 0);
        if (S_ISDIR(st.st_mode)) remove_tree(p);
        else CHECK(unlink(p) == 0);
    }
    CHECK(closedir(d) == 0 && rmdir(base) == 0);
}

static void setup(void)
{
    strcpy(root, "/tmp/whateverpad-migrate-XXXXXX");
    CHECK(mkdtemp(root));
    CHECK(snprintf(legacy, sizeof legacy, "%s/anypad", root) < (int)sizeof legacy);
    CHECK(snprintf(dest, sizeof dest, "%s/whateverpad", root) < (int)sizeof dest);
    CHECK(mkdir(legacy, 0700) == 0);
    nreports = 0;
}

static void fixture(void)
{
    char p[512];
    text(legacy, "config.ini", config);
    text(legacy, "ps_combos.txt", "057e:2009 l1+l2+options\n054c:09cc touchpad\n");
    text(legacy, "ban.txt", "# native pad\n01:23:45:67:89:AB\n");
    path(p, legacy, "maps"); CHECK(mkdir(p, 0700) == 0);
    text(legacy, "maps/057E_2009.map", "# keep my mapping\nbutton1 = cross\n");
    text(legacy, "pads.db", "WHATEVER-unbound-credentials");
    text(legacy, ".irk", "unbound-secret");
    text(legacy, "notes.txt", "user data, not a migration input");
}

static void report(void *ud, const char *name, migrate_status status)
{
    (void)ud;
    if (sync_parent_armed && status == MIGRATE_COPIED && !sync_successes)
        premature_copy = 1;
    CHECK(nreports < sizeof reports / sizeof reports[0]);
    reports[nreports].status = status;
    CHECK(strlen(name) < sizeof reports[0].path);
    strcpy(reports[nreports++].path, name);
}

static unsigned count(migrate_status status)
{
    unsigned i, n = 0;
    for (i = 0; i < nreports; i++) n += reports[i].status == status;
    return n;
}

static int saw(const char *name, migrate_status status)
{
    unsigned i;
    for (i = 0; i < nreports; i++)
        if (reports[i].status == status && !strcmp(reports[i].path, name)) return 1;
    return 0;
}

int migrate_test_fault(migrate_stage stage)
{
    if ((int)stage == crash_stage) _exit(73);
    if (race && stage == MIGRATE_STAGE_PUBLISH) {
        text(dest, "config.ini", "notifications=1\n");
        race = 0;
    }
    return (int)stage == fail_stage;
}

int migrate_test_fsync(int fd)
{
    struct stat st;
    int matched = 0, result;
    if (sync_parent_armed) {
        CHECK(fstat(fd, &st) == 0);
        matched = st.st_dev == sync_parent_dev && st.st_ino == sync_parent_ino;
        if (matched) {
            sync_attempts++;
            if (sync_fail_once) {
                sync_fail_once = 0;
                errno = EIO;
                return -1;
            }
        }
    }
    result = fsync(fd);
    if (matched && !result) sync_successes++;
    return result;
}

static void clean_destination(void)
{
    DIR *d = opendir(dest);
    struct dirent *e;
    CHECK(d);
    while ((e = readdir(d)))
        CHECK(strncmp(e->d_name, ".whp-migrate-", 13) != 0);
    CHECK(closedir(d) == 0);
}

static void happy(void)
{
    char p[512];
    whateverpad_config c;
    setup(); fixture();
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 0);
    CHECK(count(MIGRATE_COPIED) == 4);
    CHECK(count(MIGRATE_FILTERED) == 1 && count(MIGRATE_PROTECTED) == 2);
    same(legacy, "config.ini", config);
    same(dest, "ps_combos.txt", "057e:2009 l1+l2+options\n054c:09cc touchpad\n");
    same(dest, "maps/057E_2009.map", "# keep my mapping\nbutton1 = cross\n");
    same(dest, "ban.txt", "# native pad\n01:23:45:67:89:AB\n");
    path(p, dest, "ps_combos.txt");
    CHECK(pscombo_load(p) == 2);
    CHECK(pscombo_get(0x057e, 0x2009) == (PAD_L1 | PAD_L2 | PAD_OPTIONS));
    CHECK(pscombo_get(0x054c, 0x09cc) == PAD_TOUCHPAD);
    config_defaults(&c); path(p, dest, "config.ini");
    CHECK(config_load(&c, p) == 0);
    CHECK(c.menu_combo == (PAD_L1 | PAD_R1) && c.menu_hold_ms == 2400);
    CHECK(!c.auto_reconnect && !c.notifications && !c.lan_access && !c.developer_mode);
    CHECK(!exists(dest, "pads.db") && !exists(dest, ".irk") && !exists(dest, "notes.txt"));
    text(dest, "notes.txt", "destination user data");
    nreports = 0;
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 4);
    CHECK(count(MIGRATE_COPIED) == 0 && count(MIGRATE_CONFLICT) == 4);
    CHECK(saw("maps/057E_2009.map", MIGRATE_CONFLICT));
    same(dest, "notes.txt", "destination user data");
    clean_destination();
    remove_tree(root);
    puts("migration: copy, benign settings, model combinations, protected inputs, idempotence passed");
}

static void conflicts(void)
{
    setup(); fixture();
    CHECK(mkdir(dest, 0700) == 0);
    text(dest, "config.ini", "lan_access=1\n");
    text(dest, "ps_combos.txt", "user combinations");
    text(dest, "ban.txt", "user bans");
    text(dest, "maps", "a user file, not a directory");
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 4);
    CHECK(count(MIGRATE_CONFLICT) == 4 && count(MIGRATE_COPIED) == 0);
    same(dest, "config.ini", "lan_access=1\n");
    same(dest, "ps_combos.txt", "user combinations");
    same(dest, "ban.txt", "user bans");
    same(dest, "maps", "a user file, not a directory");
    same(legacy, "config.ini", config);
    remove_tree(root);
    puts("migration: every existing destination kept and reported");
}

static void interrupted(void)
{
    int stage;
    for (stage = MIGRATE_STAGE_WRITE; stage <= MIGRATE_STAGE_DIR_FSYNC; stage++) {
        pid_t child;
        int status;
        setup(); text(legacy, "config.ini", config);
        crash_stage = stage;
        child = fork(); CHECK(child >= 0);
        if (!child) {
            (void)migrate_legacy(legacy, dest, report, NULL);
            _exit(74);
        }
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 73);
        crash_stage = 0;
        same(legacy, "config.ini", config);
        if (stage <= MIGRATE_STAGE_PUBLISH) CHECK(!exists(dest, "config.ini"));
        /* Re-read source rather than trusting interrupted or stale staging bytes. */
        if (stage == MIGRATE_STAGE_WRITE)
            text(legacy, "config.ini", "notifications=1\nmenu_hold_ms=2500\n");
        CHECK(migrate_legacy(legacy, dest, report, NULL) == (stage == MIGRATE_STAGE_DIR_FSYNC ? 1 : 0));
        CHECK(exists(dest, "config.ini"));
        if (stage == MIGRATE_STAGE_WRITE) {
            whateverpad_config c;
            char p[512];
            path(p, dest, "config.ini"); config_defaults(&c);
            CHECK(config_load(&c, p) == 0 && c.notifications && c.menu_hold_ms == 2500);
        }
        clean_destination();
        remove_tree(root);
    }
    puts("migration: real process interruption at every stage, restart, stale source and cleanup passed");
}

static void bad_inputs(void)
{
    char p[512], alias[512];
    char *huge = malloc(MIGRATE_FILE_MAX + 1);
    CHECK(huge); memset(huge, 'x', MIGRATE_FILE_MAX + 1);
    setup();
    text(legacy, "config.ini", "menu_hold_ms=2000oops\nlan_access=1\n");
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(saw("config.ini", MIGRATE_INVALID) && !exists(dest, "config.ini"));
    same(legacy, "config.ini", "menu_hold_ms=2000oops\nlan_access=1\n");
    text(legacy, "config.ini", "notifications=2\n");
    nreports = 0;
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(saw("config.ini", MIGRATE_INVALID));
    put(legacy, "config.ini", huge, MIGRATE_FILE_MAX + 1);
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(!exists(dest, "config.ini"));
    path(p, legacy, "config.ini"); CHECK(unlink(p) == 0);
    path(alias, root, "real.ini"); text(root, "real.ini", "notifications=0\n");
    CHECK(symlink(alias, p) == 0);
    nreports = 0;
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(saw("config.ini", MIGRATE_SYMLINK));
    same(root, "real.ini", "notifications=0\n");
    CHECK(unlink(p) == 0);
    path(p, legacy, "ban.txt"); CHECK(mkfifo(p, 0600) == 0);
    nreports = 0;
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(saw("ban.txt", MIGRATE_INVALID));
    CHECK(unlink(p) == 0);
    path(p, legacy, "maps"); CHECK(symlink(root, p) == 0);
    nreports = 0;
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(saw("maps", MIGRATE_SYMLINK));
    CHECK(unlink(p) == 0);
    path(p, root, "alias"); CHECK(symlink(legacy, p) == 0);
    nreports = 0;
    CHECK(migrate_legacy(p, dest, report, NULL) == 1 && count(MIGRATE_SYMLINK) == 1);
    CHECK(unlink(p) == 0);
    remove_tree(root);
    free(huge);
    puts("migration: malformed/oversized input, file/root/maps symlinks and FIFO refused without loss");
}

static void failures(void)
{
    char p[512];
    int stage;
    for (stage = MIGRATE_STAGE_FILE_FSYNC; stage <= MIGRATE_STAGE_DIR_FSYNC; stage++) {
        setup(); text(legacy, "config.ini", config);
        fail_stage = stage;
        CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
        CHECK(count(stage == MIGRATE_STAGE_DIR_FSYNC ? MIGRATE_UNCERTAIN : MIGRATE_IO_ERROR) == 1);
        same(legacy, "config.ini", config);
        CHECK(exists(dest, "config.ini") == (stage == MIGRATE_STAGE_DIR_FSYNC));
        fail_stage = 0; nreports = 0;
        CHECK(migrate_legacy(legacy, dest, report, NULL) == (stage == MIGRATE_STAGE_DIR_FSYNC ? 1 : 0));
        clean_destination(); remove_tree(root);
    }
    setup(); text(legacy, "config.ini", config); CHECK(mkdir(dest, 0500) == 0);
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(count(MIGRATE_IO_ERROR) == 1 && !exists(dest, "config.ini"));
    same(legacy, "config.ini", config);
    CHECK(chmod(dest, 0700) == 0);
    nreports = 0; race = 1;
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(saw("config.ini", MIGRATE_CONFLICT));
    same(dest, "config.ini", "notifications=1\n");
    clean_destination();
    remove_tree(root);
    setup(); text(legacy, "config.ini", config);
    path(p, root, "alias"); CHECK(symlink(dest, p) == 0);
    CHECK(migrate_legacy(legacy, p, report, NULL) == 1);
    CHECK(count(MIGRATE_SYMLINK) == 1 && !exists(dest, "config.ini"));
    remove_tree(root);
    puts("migration: durability faults, unwritable destination, concurrent destination and destination symlink passed");
}

static void parent_sync_recovery(void)
{
    int maps;
    for (maps = 0; maps < 2; maps++) {
        struct stat st;
        char p[512];
        int retry;
        unsigned copied;
        setup();
        if (maps) {
            CHECK(mkdir(dest, 0700) == 0);
            path(p, legacy, "maps"); CHECK(mkdir(p, 0700) == 0);
            text(legacy, "maps/1234_5678.map", "button1=cross\n");
            CHECK(stat(dest, &st) == 0);
        } else {
            text(legacy, "config.ini", config);
            CHECK(stat(root, &st) == 0);
        }
        sync_parent_dev = st.st_dev;
        sync_parent_ino = st.st_ino;
        sync_parent_armed = sync_fail_once = 1;
        sync_attempts = sync_successes = premature_copy = 0;
        CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
        CHECK(count(MIGRATE_IO_ERROR) == 1 && count(MIGRATE_COPIED) == 0);
        CHECK(sync_attempts == 1 && sync_successes == 0);
        CHECK(exists(dest, maps ? "maps" : "."));
        CHECK(!exists(dest, maps ? "maps/1234_5678.map" : "config.ini"));
        nreports = 0;
        retry = migrate_legacy(legacy, dest, report, NULL);
        copied = count(MIGRATE_COPIED);
        sync_parent_armed = 0;
        if (maps) {
            same(legacy, "maps/1234_5678.map", "button1=cross\n");
            same(dest, "maps/1234_5678.map", "button1=cross\n");
        } else {
            same(legacy, "config.ini", config);
            CHECK(exists(dest, "config.ini"));
        }
        clean_destination();
        remove_tree(root);
        printf("parent-sync recovery: %s retry=%d copied=%u attempts=%d successes=%d premature=%d\n",
               maps ? "maps" : "destination-root", retry, copied,
               sync_attempts, sync_successes, premature_copy);
        CHECK(retry == 0 && copied == 1 && sync_successes > 0 && !premature_copy);
    }
}

static void maps_and_staging(void)
{
    char p[512], q[512], name[64];
    unsigned i;
    setup(); fixture();
    path(p, legacy, "maps/0001_0002.map"); CHECK(symlink("../ban.txt", p) == 0);
    text(legacy, "maps/notes.txt", "keep unsupported user file");
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 2);
    CHECK(saw("maps/0001_0002.map", MIGRATE_SYMLINK) && saw("maps/notes.txt", MIGRATE_INVALID));
    CHECK(!exists(dest, "maps/0001_0002.map") && !exists(dest, "maps/notes.txt"));
    remove_tree(root);
    setup(); text(legacy, "config.ini", config); CHECK(mkdir(dest, 0700) == 0);
    path(p, dest, ".whp-migrate-config.ini"); CHECK(mkdir(p, 0700) == 0);
    text(p, "user-file", "do not delete an unknown stale directory");
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    same(p, "user-file", "do not delete an unknown stale directory");
    CHECK(!exists(dest, "config.ini")); remove_tree(root);
    setup(); path(p, legacy, "maps"); CHECK(mkdir(p, 0700) == 0);
    for (i = 0; i <= MIGRATE_MAPS_MAX; i++) {
        snprintf(name, sizeof name, "maps/%04x_0001.map", i);
        text(legacy, name, "button1=cross\n");
    }
    CHECK(migrate_legacy(legacy, dest, report, NULL) == 1);
    CHECK(count(MIGRATE_COPIED) == MIGRATE_MAPS_MAX && saw("maps", MIGRATE_INVALID));
    /* Bounds stop work visibly and leave even unprocessed sources unchanged. */
    path(q, legacy, "maps"); CHECK(access(q, R_OK) == 0);
    remove_tree(root);
    puts("migration: map bounds, unsupported entries and unrelated stale staging data preserved");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--qa-prepare")) {
        setup(); fixture(); CHECK(mkdir(dest, 0700) == 0);
        text(dest, "ban.txt", "destination ban must win\n");
        {
            char p[512];
            path(p, legacy, "maps/0001_0002.map"); CHECK(symlink("../ban.txt", p) == 0);
        }
        printf("QA_ROOT=%s\n", root);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--qa-run")) {
        unsigned i;
        CHECK(!strncmp(argv[2], "/tmp/whateverpad-migrate-", strlen("/tmp/whateverpad-migrate-")) &&
              strlen(argv[2]) < sizeof root);
        strcpy(root, argv[2]);
        snprintf(legacy, sizeof legacy, "%s/anypad", root);
        snprintf(dest, sizeof dest, "%s/whateverpad", root);
        CHECK(migrate_legacy(legacy, dest, report, NULL) == 2);
        for (i = 0; i < nreports; i++)
            printf("REPORT %s %s\n", reports[i].path, migrate_status_name(reports[i].status));
        return 0;
    }
    CHECK(argc == 1);
    happy(); conflicts(); interrupted(); bad_inputs(); failures(); parent_sync_recovery(); maps_and_staging();
    printf("%u migration checks, 0 failed; all fixtures removed\n", checks);
    return 0;
}
