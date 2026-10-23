/* GPL-3.0-or-later. Conservative legacy configuration, never credentials. */
#ifndef WHATEVERPAD_MIGRATE_H
#define WHATEVERPAD_MIGRATE_H

#define MIGRATE_FILE_MAX 65536
#define MIGRATE_MAPS_MAX 128

typedef enum {
    MIGRATE_COPIED, MIGRATE_FILTERED, MIGRATE_PROTECTED,
    MIGRATE_CONFLICT, MIGRATE_INVALID, MIGRATE_SYMLINK,
    MIGRATE_IO_ERROR, MIGRATE_UNCERTAIN
} migrate_status;

typedef void (*migrate_report)(void *ud, const char *relative_path, migrate_status status);

/* Paths name distinct roots, with existing parents; only destination's final
 * directory is created. No symlink component is followed. Reports are required,
 * contain paths/status only, and include every conflict, even on repeated runs.
 * Returns the number of conflicts/errors (FILTERED/PROTECTED are informational).
 * Copies config.ini, ps_combos.txt, ban.txt and flat VVVV_PPPP.map files only.
 * Config is validated in full and rewritten without LAN/developer opt-ins.
 *
 * Private .whp-migrate-<filename>/ directories in destination/maps are reserved
 * scratch space: pending/ready are restartable migration-owned files. Unknown
 * entries, owners, permissions or symlinks there are refused, never cleaned.
 * A complete file is fsynced and renamed inside scratch, then linked into the
 * absent destination atomically: unlike POSIX rename, link cannot overwrite a
 * concurrently created user file. Directory fsync completes durability.
 * UNCERTAIN means a complete file was published but durability is unconfirmed.
 * Originals and all unrecognized/adapter-unbound keys remain untouched.
 * Runtime startup wiring belongs to the caller, not this module. */
int migrate_legacy(const char *source, const char *destination,
                   migrate_report report, void *ud);
const char *migrate_status_name(migrate_status status);

typedef enum {
    MIGRATE_STAGE_WRITE = 1, MIGRATE_STAGE_FILE_FSYNC,
    MIGRATE_STAGE_RENAME, MIGRATE_STAGE_PUBLISH, MIGRATE_STAGE_DIR_FSYNC
} migrate_stage;
#ifdef MIGRATE_TEST
/* Test-only I/O fault or real process interruption at an exact transaction step. */
int migrate_test_fault(migrate_stage stage);
#endif

#endif
