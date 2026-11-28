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
    void* p_ctx;
    int (*p_open_read)(void* p_ctx, const char* p_path);
    int (*p_open_write)(void* p_ctx, const char* p_path);                                       /* create or truncate */
    int (*p_read_at)(void* p_ctx, int handle, uint32_t offset, uint8_t* p_buf, uint16_t len);        /* bytes read */
    int (*p_write_at)(void* p_ctx, int handle, uint32_t offset, const uint8_t* p_buf, uint16_t len); /* bytes written */
    int32_t (*p_size)(void* p_ctx, int handle);
    int (*p_close)(void* p_ctx, int handle);
    int (*p_remove)(void* p_ctx, const char* p_path);
    int (*p_rename)(void* p_ctx, const char* p_from, const char* p_to_path); /* replaces 'to' if it exists */
    /* Calls fn(name, user) for each regular file in dir (name only, no directory part). */
    int (*p_list)(void* p_ctx, const char* p_dir, void (*p_callback)(const char* p_name, void* p_user), void* p_user);
    int (*p_mkdir)(void* p_ctx, const char* p_dir); /* ok if it already exists */
} alfp_storage_t;

/* Clock and randomness. */
typedef struct alfp_platform_s
{
    void* p_ctx;
    uint32_t (*p_now_ms)(void* p_ctx);           /* ms since boot, wraps after ~49 days (handled) */
    uint32_t (*p_rand32)(void* p_ctx);           /* only used for session ids and jitter */
    void (*p_log)(void* p_ctx, const char* p_msg); /* may be NULL */
} alfp_platform_t;

#endif
