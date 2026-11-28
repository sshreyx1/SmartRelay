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
static bool gb_mounted;

/* Diagnostics for 's' (storage_fatfs_status): the last FatFs error of any call except a missing
 * file on open-for-read (an expected answer, e.g. "does this name exist yet?"). */
static FRESULT g_last_err = FR_OK;
static const char* gp_last_err_op = "";
static char g_last_err_path[64];
static uint32_t g_err_count;

static void note_err(const char* p_op_name, FRESULT res, const char* p_path)
{
    if (FR_OK == res)
    {
        return;
    }
    g_last_err = res;
    gp_last_err_op = p_op_name;
    snprintf(g_last_err_path, sizeof(g_last_err_path), "%s", p_path ? p_path : "");
    g_err_count++;
}

static int take(void)
{
    for (int idx = 0; idx < MAX_OPEN; idx++)
    {
        if (!g_used[idx])
        {
            g_used[idx] = true;
            return idx;
        }
    }
    return -1;
}

static FIL* fil(int handle)
{
    return ((handle >= 0) && (handle < MAX_OPEN) && (g_used[handle])) ? &g_fil[handle] : NULL;
}

static int f_openmode(const char* p_path, BYTE mode)
{
    int handle = take();
    if (handle < 0)
    {
        note_err("open (no free handle)", FR_TOO_MANY_OPEN_FILES, p_path);
        return -1;
    }
    FRESULT res = f_open(&g_fil[handle], p_path, mode);
    if (FR_OK != res)
    {
        if (!((FA_READ == mode) && ((FR_NO_FILE == res) || (FR_NO_PATH == res))))
        {
            note_err(FA_READ == mode ? "open_r" : "open_w", res, p_path);
        }
        g_used[handle] = false;
        return -1;
    }
    return handle;
}

static int s_open_read(void* p_ctx, const char* p_path)
{
    (void)p_ctx;
    return f_openmode(p_path, FA_READ);
}

static int s_open_write(void* p_ctx, const char* p_path)
{
    (void)p_ctx;
    return f_openmode(p_path, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
}

static int s_read_at(void* p_ctx, int handle, uint32_t off, uint8_t* p_buf, uint16_t len)
{
    (void)p_ctx;
    FIL* p_file = fil(handle);
    UINT got = 0;
    FRESULT res = p_file ? f_lseek(p_file, off) : FR_INVALID_OBJECT;
    if (FR_OK == res)
    {
        res = f_read(p_file, p_buf, len, &got);
    }
    if (FR_OK != res)
    {
        note_err("read", res, "");
        return -1;
    }
    return (int)got;
}

static int s_write_at(void* p_ctx, int handle, uint32_t off, const uint8_t* p_buf, uint16_t len)
{
    (void)p_ctx;
    FIL* p_file = fil(handle);
    UINT put = 0;
    /* f_lseek past the end of a file opened for writing extends it, which is exactly what
     * out-of-order fragments need. */
    FRESULT res = p_file ? f_lseek(p_file, off) : FR_INVALID_OBJECT;
    if (FR_OK == res)
    {
        res = f_write(p_file, p_buf, len, &put);
    }
    if (FR_OK != res)
    {
        note_err("write", res, "");
        return -1;
    }
    return (int)put;
}

static int32_t s_size(void* p_ctx, int handle)
{
    (void)p_ctx;
    FIL* p_file = fil(handle);
    return p_file ? (int32_t)f_size(p_file) : -1;
}

static int s_close(void* p_ctx, int handle)
{
    (void)p_ctx;
    FIL* p_file = fil(handle);
    if (!p_file)
    {
        return -1;
    }
    FRESULT res = f_close(p_file);
    note_err("close", res, "");
    g_used[handle] = false;
    return FR_OK == res ? 0 : -1;
}

static int s_remove(void* p_ctx, const char* p_path)
{
    (void)p_ctx;
    return FR_OK == f_unlink(p_path) ? 0 : -1;
}

static int s_rename(void* p_ctx, const char* p_from, const char* p_to_path)
{
    (void)p_ctx;
    f_unlink(p_to_path); /* FatFs refuses to rename onto an existing file */
    FRESULT res = f_rename(p_from, p_to_path);
    note_err("rename", res, p_to_path);
    return FR_OK == res ? 0 : -1;
}

static int s_list(void* p_ctx, const char* p_dir, void (*p_callback)(const char*, void*), void* p_user)
{
    (void)p_ctx;
    DIR dir_handle;
    FILINFO file_info;
    if (FR_OK != f_opendir(&dir_handle, p_dir))
    {
        return -1;
    }
    while ((FR_OK == f_readdir(&dir_handle, &file_info)) && (file_info.fname[0]))
    {
        if (!(file_info.fattrib & AM_DIR))
        {
            p_callback(file_info.fname, p_user);
        }
    }
    f_closedir(&dir_handle);
    return 0;
}

static int s_mkdir(void* p_ctx, const char* p_dir)
{
    (void)p_ctx;
    FRESULT res = f_mkdir(p_dir);
    if (FR_EXIST != res)
    {
        note_err("mkdir", res, p_dir);
    }
    return ((FR_OK == res) || (FR_EXIST == res)) ? 0 : -1;
}

bool storage_fatfs_mount(void)
{
    if (!gb_mounted)
    {
        FRESULT res = f_mount(&g_fs, "", 1);
        if (FR_OK != res)
        {
            printf("sd: mount failed (%d). Card inserted? FAT32 formatted?\n", (int)res);
            return false;
        }
        gb_mounted = true;
    }
    return true;
}

void storage_fatfs_status(char* p_out, size_t cap)
{
    int used = 0;
    for (int idx = 0; idx < MAX_OPEN; idx++)
    {
        used += g_used[idx] ? 1 : 0;
    }
    if (0 == g_err_count)
    {
        snprintf(p_out, cap, "sd handles in use %d/%d | no SD errors", used, MAX_OPEN);
    }
    else
    {
        snprintf(p_out, cap, "sd handles in use %d/%d | %lu SD errors, last: %s FR=%d on %s", used, MAX_OPEN,
                 (unsigned long)g_err_count, gp_last_err_op, (int)g_last_err, g_last_err_path);
    }
}

const alfp_storage_t* storage_fatfs(void)
{
    static const alfp_storage_t store = {
        .p_ctx = NULL,
        .p_open_read = s_open_read,
        .p_open_write = s_open_write,
        .p_read_at = s_read_at,
        .p_write_at = s_write_at,
        .p_size = s_size,
        .p_close = s_close,
        .p_remove = s_remove,
        .p_rename = s_rename,
        .p_list = s_list,
        .p_mkdir = s_mkdir,
    };
    return &store;
}
