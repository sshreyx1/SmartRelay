/*
 * storage_posix.c - see storage_posix.h. Mirrors what storage_fatfs.c does on the Pico.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage_posix.h"

#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static void full(storage_posix_t* sp, const char* path, char* out, size_t cap)
{
    snprintf(out, cap, "%s/%s", sp->root, path);
}

static int slot(storage_posix_t* sp)
{
    for (int i = 0; i < SP_MAX_FILES; i++)
    {
        if (!sp->f[i])
        {
            return i;
        }
    }
    return -1;
}

static int p_open(void* c, const char* path, const char* mode)
{
    storage_posix_t* sp = c;
    char p[512];
    full(sp, path, p, sizeof(p));
    int h = slot(sp);
    if (h < 0)
    {
        return -1;
    }
    sp->f[h] = fopen(p, mode);
    if (!sp->f[h])
    {
        return -1;
    }
    sp->open_count++;
    return h;
}

static int p_open_read(void* c, const char* path)
{
    return p_open(c, path, "rb");
}

static int p_open_write(void* c, const char* path)
{
    return p_open(c, path, "w+b"); /* read + write, like FA_READ|FA_WRITE|FA_CREATE_ALWAYS */
}

static FILE* fh(storage_posix_t* sp, int h)
{
    return (h >= 0 && h < SP_MAX_FILES) ? sp->f[h] : NULL;
}

static int p_read_at(void* c, int h, uint32_t off, uint8_t* buf, uint16_t len)
{
    FILE* f = fh(c, h);
    if (!f || fseek(f, (long)off, SEEK_SET) != 0)
    {
        return -1;
    }
    return (int)fread(buf, 1, len, f);
}

static int p_write_at(void* c, int h, uint32_t off, const uint8_t* buf, uint16_t len)
{
    storage_posix_t* sp = c;
    FILE* f = fh(sp, h);
    if (!f || fseek(f, (long)off, SEEK_SET) != 0)
    {
        return -1;
    }
    size_t n;
    sp->writes++;
    bool fault = sp->fault_write_number && sp->writes == sp->fault_write_number;
    if (sp->write_fault_rate > 0 && sp->rand_fn && (float)(sp->rand_fn() % 1000000u) / 1e6f < sp->write_fault_rate)
    {
        fault = true;
    }
    if (fault && len && sp->rand_fn)
    {
        uint8_t tmp[512];
        memcpy(tmp, buf, len);
        tmp[sp->rand_fn() % len] ^= 0x5A;
        n = fwrite(tmp, 1, len, f);
    }
    else
    {
        n = fwrite(buf, 1, len, f);
    }
    fflush(f);
    return (int)n;
}

static int32_t p_size(void* c, int h)
{
    FILE* f = fh(c, h);
    if (!f || fseek(f, 0, SEEK_END) != 0)
    {
        return -1;
    }
    return (int32_t)ftell(f);
}

static int p_close(void* c, int h)
{
    storage_posix_t* sp = c;
    FILE* f = fh(sp, h);
    if (!f)
    {
        return -1;
    }
    fclose(f);
    sp->f[h] = NULL;
    sp->open_count--;
    return 0;
}

static int p_remove(void* c, const char* path)
{
    char p[512];
    full(c, path, p, sizeof(p));
    return remove(p) == 0 ? 0 : -1;
}

static int p_rename(void* c, const char* a, const char* b)
{
    char pa[512], pb[512];
    full(c, a, pa, sizeof(pa));
    full(c, b, pb, sizeof(pb));
    return rename(pa, pb) == 0 ? 0 : -1;
}

static int p_list(void* c, const char* dir, void (*fn)(const char*, void*), void* user)
{
    char p[512];
    full(c, dir, p, sizeof(p));
    DIR* d = opendir(p);
    if (!d)
    {
        return -1;
    }
    struct dirent* e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
        {
            continue;
        }
        char fp[1024];
        struct stat s;
        snprintf(fp, sizeof(fp), "%s/%s", p, e->d_name);
        if (stat(fp, &s) == 0 && S_ISREG(s.st_mode))
        {
            fn(e->d_name, user);
        }
    }
    closedir(d);
    return 0;
}

static int mkdirs(const char* p)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", p);
    for (char* s = tmp + 1; *s; s++)
    {
        if (*s == '/')
        {
            *s = 0;
            mkdir(tmp, 0755);
            *s = '/';
        }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int p_mkdir(void* c, const char* dir)
{
    char p[512];
    full(c, dir, p, sizeof(p));
    return mkdirs(p);
}

void storage_posix_init(storage_posix_t* sp, const char* root, alfp_storage_t* out)
{
    memset(sp, 0, sizeof(*sp));
    snprintf(sp->root, sizeof(sp->root), "%s", root);
    mkdirs(root);
    *out = (alfp_storage_t){
        .ctx = sp,
        .open_read = p_open_read,
        .open_write = p_open_write,
        .read_at = p_read_at,
        .write_at = p_write_at,
        .size = p_size,
        .close = p_close,
        .remove = p_remove,
        .rename = p_rename,
        .list = p_list,
        .mkdir = p_mkdir,
    };
}

void storage_posix_close_all(storage_posix_t* sp)
{
    for (int i = 0; i < SP_MAX_FILES; i++)
    {
        if (sp->f[i])
        {
            fclose(sp->f[i]);
            sp->f[i] = NULL;
        }
    }
    sp->open_count = 0;
}
