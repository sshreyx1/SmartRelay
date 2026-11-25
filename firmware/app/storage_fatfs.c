/*
 * storage_fatfs.c - alfp_storage_t on the microSD card via FatFs (carlk3 library).   OWNER: Member 2
 *
 * The Maker Pi Pico's microSD slot is wired to SPI1: GP10 SCK, GP11 MOSI, GP12 MISO, GP15 CS
 * (see hw_config.c). FatFs is built with FF_FS_REENTRANT = 0, i.e. NOT thread-safe, so only the
 * comms task may call these functions. That is a deliberate design rule, not an accident.
 */
#include "storage_fatfs.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#define MAX_OPEN 8

static FATFS g_fs;
static FIL g_fil[MAX_OPEN];
static bool g_used[MAX_OPEN];
static bool g_mounted;

/* Diagnostics for 's' (storage_fatfs_status): the last FatFs error of any call except a missing
 * file on open-for-read (an expected answer, e.g. "does this name exist yet?"). */
static FRESULT g_last_err = FR_OK;
static const char* g_last_err_op = "";
static char g_last_err_path[64];
static uint32_t g_err_count;

static void note_err(const char* op, FRESULT r, const char* path)
{
    if (r == FR_OK)
    {
        return;
    }
    g_last_err = r;
    g_last_err_op = op;
    snprintf(g_last_err_path, sizeof(g_last_err_path), "%s", path ? path : "");
    g_err_count++;
}

static int take(void)
{
    for (int i = 0; i < MAX_OPEN; i++)
    {
        if (!g_used[i])
        {
            g_used[i] = true;
            return i;
        }
    }
    return -1;
}

static FIL* fil(int h)
{
    return (h >= 0 && h < MAX_OPEN && g_used[h]) ? &g_fil[h] : NULL;
}

static int f_openmode(const char* path, BYTE mode)
{
    int h = take();
    if (h < 0)
    {
        note_err("open (no free handle)", FR_TOO_MANY_OPEN_FILES, path);
        return -1;
    }
    FRESULT r = f_open(&g_fil[h], path, mode);
    if (r != FR_OK)
    {
        if (!(mode == FA_READ && (r == FR_NO_FILE || r == FR_NO_PATH)))
        {
            note_err(mode == FA_READ ? "open_r" : "open_w", r, path);
        }
        g_used[h] = false;
        return -1;
    }
    return h;
}

static int s_open_read(void* c, const char* path)
{
    (void)c;
    return f_openmode(path, FA_READ);
}

static int s_open_write(void* c, const char* path)
{
    (void)c;
    return f_openmode(path, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
}

static int s_read_at(void* c, int h, uint32_t off, uint8_t* buf, uint16_t len)
{
    (void)c;
    FIL* f = fil(h);
    UINT got = 0;
    FRESULT r = f ? f_lseek(f, off) : FR_INVALID_OBJECT;
    if (r == FR_OK)
    {
        r = f_read(f, buf, len, &got);
    }
    if (r != FR_OK)
    {
        note_err("read", r, "");
        return -1;
    }
    return (int)got;
}

static int s_write_at(void* c, int h, uint32_t off, const uint8_t* buf, uint16_t len)
{
    (void)c;
    FIL* f = fil(h);
    UINT put = 0;
    /* f_lseek past the end of a file opened for writing extends it, which is exactly what
     * out-of-order fragments need. */
    FRESULT r = f ? f_lseek(f, off) : FR_INVALID_OBJECT;
    if (r == FR_OK)
    {
        r = f_write(f, buf, len, &put);
    }
    if (r != FR_OK)
    {
        note_err("write", r, "");
        return -1;
    }
    return (int)put;
}

static int32_t s_size(void* c, int h)
{
    (void)c;
    FIL* f = fil(h);
    return f ? (int32_t)f_size(f) : -1;
}

static int s_close(void* c, int h)
{
    (void)c;
    FIL* f = fil(h);
    if (!f)
    {
        return -1;
    }
    FRESULT r = f_close(f);
    note_err("close", r, "");
    g_used[h] = false;
    return r == FR_OK ? 0 : -1;
}

static int s_remove(void* c, const char* path)
{
    (void)c;
    return f_unlink(path) == FR_OK ? 0 : -1;
}

static int s_rename(void* c, const char* from, const char* to)
{
    (void)c;
    f_unlink(to); /* FatFs refuses to rename onto an existing file */
    FRESULT r = f_rename(from, to);
    note_err("rename", r, to);
    return r == FR_OK ? 0 : -1;
}

static int s_list(void* c, const char* dir, void (*fn)(const char*, void*), void* user)
{
    (void)c;
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, dir) != FR_OK)
    {
        return -1;
    }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
    {
        if (!(fi.fattrib & AM_DIR))
        {
            fn(fi.fname, user);
        }
    }
    f_closedir(&d);
    return 0;
}

static int s_mkdir(void* c, const char* dir)
{
    (void)c;
    FRESULT r = f_mkdir(dir);
    if (r != FR_EXIST)
    {
        note_err("mkdir", r, dir);
    }
    return (r == FR_OK || r == FR_EXIST) ? 0 : -1;
}

bool storage_fatfs_mount(void)
{
    if (!g_mounted)
    {
        FRESULT r = f_mount(&g_fs, "", 1);
        if (r != FR_OK)
        {
            printf("sd: mount failed (%d). Card inserted? FAT32 formatted?\n", (int)r);
            return false;
        }
        g_mounted = true;
    }
    return true;
}

void storage_fatfs_status(char* out, size_t cap)
{
    int used = 0;
    for (int i = 0; i < MAX_OPEN; i++)
    {
        used += g_used[i] ? 1 : 0;
    }
    if (g_err_count == 0)
    {
        snprintf(out, cap, "sd handles in use %d/%d | no SD errors", used, MAX_OPEN);
    }
    else
    {
        snprintf(out, cap, "sd handles in use %d/%d | %lu SD errors, last: %s FR=%d on %s", used, MAX_OPEN,
                 (unsigned long)g_err_count, g_last_err_op, (int)g_last_err, g_last_err_path);
    }
}

const alfp_storage_t* storage_fatfs(void)
{
    static const alfp_storage_t st = {
        .ctx = NULL,
        .open_read = s_open_read,
        .open_write = s_open_write,
        .read_at = s_read_at,
        .write_at = s_write_at,
        .size = s_size,
        .close = s_close,
        .remove = s_remove,
        .rename = s_rename,
        .list = s_list,
        .mkdir = s_mkdir,
    };
    return &st;
}
