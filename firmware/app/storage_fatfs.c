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
        return -1;
    }
    if (f_open(&g_fil[h], path, mode) != FR_OK)
    {
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
    if (!f || f_lseek(f, off) != FR_OK || f_read(f, buf, len, &got) != FR_OK)
    {
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
    if (!f || f_lseek(f, off) != FR_OK || f_write(f, buf, len, &put) != FR_OK)
    {
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
    return f_rename(from, to) == FR_OK ? 0 : -1;
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
