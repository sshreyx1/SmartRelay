/*
 * storage_posix.h - alfp_storage_t on a laptop directory (stands in for the SD card).
 */
#ifndef STORAGE_POSIX_H
#define STORAGE_POSIX_H

#include <stdio.h>

#include "alfp_port.h"

#define SP_MAX_FILES 16

typedef struct
{
    char root[256];
    FILE* f[SP_MAX_FILES];
    float write_fault_rate; /* test hook: chance a write silently flips a byte (SD fault) */
    int fault_write_number; /* test hook: corrupt exactly the Nth write (1-based), 0 = off */
    int writes;
    unsigned (*rand_fn)(void);
    unsigned open_count; /* files currently open (leak check) */
} storage_posix_t;

/* Creates root if needed and fills *out with function pointers bound to sp. */
void storage_posix_init(storage_posix_t* sp, const char* root, alfp_storage_t* out);
void storage_posix_close_all(storage_posix_t* sp);

#endif
