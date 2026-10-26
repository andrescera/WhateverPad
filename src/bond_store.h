/* GPL-3.0-or-later. Version 1 durable, adapter-bound controller credentials. */
#ifndef WHATEVERPAD_BOND_STORE_H
#define WHATEVERPAD_BOND_STORE_H

#include <stddef.h>
#include <stdint.h>

#define BOND_MAX 16
#define BOND_HEADER_SIZE 64
#define BOND_RECORD_SIZE 64
#define BOND_FILE_MAX (BOND_HEADER_SIZE + BOND_MAX * BOND_RECORD_SIZE)

typedef struct {
    uint8_t addr[6];
    uint16_t vid, pid;
} bond_adapter;

typedef struct {
    uint8_t addr[6], key[16], type;
    uint16_t vid, pid;
    uint8_t le, addr_type;
    uint16_t ediv;
    uint8_t rand[8], irk[16], has_irk;
    uint8_t complete, sc, repair_needed;
} db_rec;

typedef struct {
    bond_adapter adapter;
    db_rec records[BOND_MAX];
    int count;
    uint8_t local_irk[16];
    int has_irk;
} bond_store;

typedef enum {
    BOND_COMMITTED, BOND_ERROR, BOND_UNCERTAIN, BOND_MISSING,
    BOND_INVALID, BOND_VERSION, BOND_ADAPTER, BOND_CAPACITY
} bond_result;

/* No output mutation on error. MISSING initializes an empty bound snapshot.
 * Zero VID/PID mean unavailable transport metadata, not wildcard matches. */
bond_result bond_store_decode(const uint8_t *bytes, size_t len,
                              const bond_adapter *adapter, bond_store *out);
bond_result bond_store_encode(const bond_store *s, uint8_t bytes[BOND_FILE_MAX], size_t *len);
bond_result bond_store_load(const char *path, const bond_adapter *adapter, bond_store *out);
/* Validates the existing file before replacement; never imports legacy data.
 * A missing destination never replaces a concurrently created file.
 * Owned temporary cleanup is retried before any later save.
 * UNCERTAIN means publication succeeded but directory durability is unconfirmed. */
bond_result bond_store_save(const char *path, const bond_store *s);
db_rec *bond_store_find(bond_store *s, uint8_t transport, uint8_t type, const uint8_t addr[6]);
/* Stages a validated record in memory; only save establishes durable commit. */
bond_result bond_store_put(bond_store *s, const db_rec *record);
const char *bond_store_result_name(bond_result result);

typedef enum {
    BOND_STAGE_WRITE = 1, BOND_STAGE_FLUSH, BOND_STAGE_FILE_FSYNC,
    BOND_STAGE_RENAME, BOND_STAGE_DIR_FSYNC
} bond_stage;
#ifdef BOND_STORE_TEST
/* Test-only failure injection; not compiled into the payload. */
int bond_store_test_fault(bond_stage stage);
#endif

#endif
