/*
 * storage_posix.c - see storage_posix.h. Mirrors what storage_fatfs.c does on the Pico.
 */
#define _POSIX_C_SOURCE 200809L
#include "float_types.h"
#include "storage_posix.h"

#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static void full(storage_posix_t* p_posix, const char* p_path, char* p_out, size_t cap)
{
    snprintf(p_out, cap, "%s/%s", p_posix->root, p_path);
}

static int slot(storage_posix_t* p_posix)
{
    for (int idx = 0; idx < SP_MAX_FILES; idx++)
    {
        if (!p_posix->files[idx])
        {
            return idx;
        }
    }
    return -1;
}

static int p_open(void* p_ctx, const char* p_path, const char* p_mode)
{
    storage_posix_t* p_posix = p_ctx;
    char path[512];
    full(p_posix, p_path, path, sizeof(path));
    int handle = slot(p_posix);
    if (handle < 0)
    {
        return -1;
    }
    p_posix->files[handle] = fopen(path, p_mode);
    if (!p_posix->files[handle])
    {
        return -1;
    }
    p_posix->open_count++;
    return handle;
}

static int p_open_read(void* p_ctx, const char* p_path)
{
    return p_open(p_ctx, p_path, "rb");
}

static int p_open_write(void* p_ctx, const char* p_path)
{
    return p_open(p_ctx, p_path, "w+b"); /* read + write, like FA_READ|FA_WRITE|FA_CREATE_ALWAYS */
}

static FILE* fh(storage_posix_t* p_posix, int handle)
{
    return ((handle >= 0) && (handle < SP_MAX_FILES)) ? p_posix->files[handle] : NULL;
}

static int p_read_at(void* p_ctx, int handle, uint32_t off, uint8_t* p_buf, uint16_t len)
{
    FILE* p_file = fh(p_ctx, handle);
    if ((!p_file) || (0 != fseek(p_file, (long)off, SEEK_SET)))
    {
        return -1;
    }
    return (int)fread(p_buf, 1, len, p_file);
}

static int p_write_at(void* p_ctx, int handle, uint32_t off, const uint8_t* p_buf, uint16_t len)
{
    storage_posix_t* p_posix = p_ctx;
    FILE* p_file = fh(p_posix, handle);
    if ((!p_file) || (0 != fseek(p_file, (long)off, SEEK_SET)))
    {
        return -1;
    }
    size_t written;
    p_posix->writes++;
    bool b_fault = (p_posix->fault_write_number) && (p_posix->writes == p_posix->fault_write_number);
    if ((p_posix->write_fault_rate > 0) && (p_posix->p_rand_fn) && ((float32_t)(p_posix->p_rand_fn() % 1000000u) / 1e6f < p_posix->write_fault_rate))
    {
        b_fault = true;
    }
    if (b_fault && len && (p_posix->p_rand_fn))
    {
        uint8_t tmp[512];
        memcpy(tmp, p_buf, len);
        tmp[p_posix->p_rand_fn() % len] ^= 0x5A;
        written = fwrite(tmp, 1, len, p_file);
    }
    else
    {
        written = fwrite(p_buf, 1, len, p_file);
    }
    fflush(p_file);
    return (int)written;
}

static int32_t p_size(void* p_ctx, int handle)
{
    FILE* p_file = fh(p_ctx, handle);
    if ((!p_file) || (0 != fseek(p_file, 0, SEEK_END)))
    {
        return -1;
    }
    return (int32_t)ftell(p_file);
}

static int p_close(void* p_ctx, int handle)
{
    storage_posix_t* p_posix = p_ctx;
    FILE* p_file = fh(p_posix, handle);
    if (!p_file)
    {
        return -1;
    }
    fclose(p_file);
    p_posix->files[handle] = NULL;
    p_posix->open_count--;
    return 0;
}

static int p_remove(void* p_ctx, const char* p_path)
{
    char path[512];
    full(p_ctx, p_path, path, sizeof(path));
    return 0 == remove(path) ? 0 : -1;
}

static int p_rename(void* p_ctx, const char* p_old_path, const char* p_new_path)
{
    char path_a[512], path_b[512];
    full(p_ctx, p_old_path, path_a, sizeof(path_a));
    full(p_ctx, p_new_path, path_b, sizeof(path_b));
    return 0 == rename(path_a, path_b) ? 0 : -1;
}

static int p_list(void* p_ctx, const char* p_dir, void (*p_callback)(const char*, void*), void* p_user)
{
    char path[512];
    full(p_ctx, p_dir, path, sizeof(path));
    DIR* p_dir_handle = opendir(path);
    if (!p_dir_handle)
    {
        return -1;
    }
    struct dirent* p_entry;
    while ((p_entry = readdir(p_dir_handle)))
    {
        if ('.' == p_entry->d_name[0])
        {
            continue;
        }
        char full_path[1024];
        struct stat info;
        snprintf(full_path, sizeof(full_path), "%s/%s", path, p_entry->d_name);
        if ((0 == stat(full_path, &info)) && (S_ISREG(info.st_mode)))
        {
            p_callback(p_entry->d_name, p_user);
        }
    }
    closedir(p_dir_handle);
    return 0;
}

static int mkdirs(const char* p_path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", p_path);
    for (char* p_str = tmp + 1; *p_str; p_str++)
    {
        if ('/' == *p_str)
        {
            *p_str = 0;
            mkdir(tmp, 0755);
            *p_str = '/';
        }
    }
    return ((0 == mkdir(tmp, 0755)) || (EEXIST == errno)) ? 0 : -1;
}

static int p_mkdir(void* p_ctx, const char* p_dir)
{
    char path[512];
    full(p_ctx, p_dir, path, sizeof(path));
    return mkdirs(path);
}

void storage_posix_init(storage_posix_t* p_posix, const char* p_root, alfp_storage_t* p_out)
{
    memset(p_posix, 0, sizeof(*p_posix));
    snprintf(p_posix->root, sizeof(p_posix->root), "%s", p_root);
    mkdirs(p_root);
    *p_out = (alfp_storage_t){
        .p_ctx = p_posix,
        .p_open_read = p_open_read,
        .p_open_write = p_open_write,
        .p_read_at = p_read_at,
        .p_write_at = p_write_at,
        .p_size = p_size,
        .p_close = p_close,
        .p_remove = p_remove,
        .p_rename = p_rename,
        .p_list = p_list,
        .p_mkdir = p_mkdir,
    };
}

void storage_posix_close_all(storage_posix_t* p_posix)
{
    for (int idx = 0; idx < SP_MAX_FILES; idx++)
    {
        if (p_posix->files[idx])
        {
            fclose(p_posix->files[idx]);
            p_posix->files[idx] = NULL;
        }
    }
    p_posix->open_count = 0;
}
