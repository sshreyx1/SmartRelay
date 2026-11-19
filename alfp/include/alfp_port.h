/*
 * alfp_port.h - what the ALFP library needs from the platform.
 *
 * The library never calls FatFs, the Pico SDK or POSIX directly. The Pico build supplies
 * storage_fatfs.c; the laptop build supplies storage_posix.c. That is what lets the exact
 * same protocol code be tested on a laptop and then run unchanged on the Pico.
 */
#ifndef ALFP_PORT_H
#define ALFP_PORT_H

#include <stdint.h>

/* File storage. Paths are plain strings ("outbox/q00000012.job"). A handle is a small int >= 0.
 * Every function returns < 0 on error. ctx is passed back unchanged. */
typedef struct alfp_storage_s
{
    void* ctx;
    int (*open_read)(void* ctx, const char* path);
    int (*open_write)(void* ctx, const char* path);                                       /* create or truncate */
    int (*read_at)(void* ctx, int h, uint32_t offset, uint8_t* buf, uint16_t len);        /* bytes read */
    int (*write_at)(void* ctx, int h, uint32_t offset, const uint8_t* buf, uint16_t len); /* bytes written */
    int32_t (*size)(void* ctx, int h);
    int (*close)(void* ctx, int h);
    int (*remove)(void* ctx, const char* path);
    int (*rename)(void* ctx, const char* from, const char* to); /* replaces 'to' if it exists */
    /* Calls fn(name, user) for each regular file in dir (name only, no directory part). */
    int (*list)(void* ctx, const char* dir, void (*fn)(const char* name, void* user), void* user);
    int (*mkdir)(void* ctx, const char* dir); /* ok if it already exists */
} alfp_storage_t;

/* Clock and randomness. */
typedef struct alfp_platform_s
{
    void* ctx;
    uint32_t (*now_ms)(void* ctx);           /* ms since boot, wraps after ~49 days (handled) */
    uint32_t (*rand32)(void* ctx);           /* only used for session ids and jitter */
    void (*log)(void* ctx, const char* msg); /* may be NULL */
} alfp_platform_t;

#endif
