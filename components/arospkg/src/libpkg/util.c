#include "util.h"
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void u_join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la && (a[la-1] == '/' || a[la-1] == ':'))
        snprintf(out, n, "%s%s", a, b);
    else
        snprintf(out, n, "%s/%s", a, b);
}

int u_exists(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!l) return 0;
    UnLock(l);
    return 1;
}

int u_isdir(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock fib;
    int r = 0;
    if (!l) return 0;
    if (Examine(l, &fib)) r = (fib.fib_DirEntryType > 0);
    UnLock(l);
    return r;
}

int u_mkdir(const char *path)
{
    BPTR l;
    if (u_exists(path)) return 0;
    l = CreateDir((CONST_STRPTR)path);
    if (!l) return -1;
    UnLock(l);
    return 0;
}

int u_mkpath(const char *path)
{
    char buf[512];
    size_t i;
    strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (i = 0; buf[i]; i++) {
        if (buf[i] == '/' && i > 0) {
            buf[i] = 0;
            if (buf[strlen(buf)-1] != ':') u_mkdir(buf);
            buf[i] = '/';
        }
    }
    return u_mkdir(buf);
}

int u_delete(const char *path) { return DeleteFile((CONST_STRPTR)path) ? 0 : -1; }

long u_size(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock fib;
    long r = -1;
    if (!l) return -1;
    if (Examine(l, &fib) && fib.fib_DirEntryType < 0) r = fib.fib_Size;
    UnLock(l);
    return r;
}

int u_listdir(const char *path, char ***names, int *count)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock fib;
    char **v = NULL; int n = 0, cap = 0;
    *names = NULL; *count = 0;
    if (!l) return -1;
    if (!Examine(l, &fib)) { UnLock(l); return -1; }
    while (ExNext(l, &fib)) {
        if (n == cap) {
            char **nv;
            cap = cap ? cap * 2 : 16;
            nv = (char **)realloc(v, (size_t)cap * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        v[n] = strdup((const char *)fib.fib_FileName);
        if (!v[n]) break;
        n++;
    }
    UnLock(l);
    *names = v; *count = n;
    return 0;
}

void u_freelist(char **names, int count)
{
    int i;
    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}

int u_deltree(const char *path)
{
    char **names; int n, i, bad = 0;
    if (!u_isdir(path)) return u_delete(path);
    if (u_listdir(path, &names, &n) == 0) {
        for (i = 0; i < n; i++) {
            char child[512];
            u_join(child, sizeof child, path, names[i]);
            if (u_deltree(child) != 0) bad = 1;
        }
        u_freelist(names, n);
    }
    if (u_delete(path) != 0) bad = 1;
    return bad ? -1 : 0;
}

int u_copy(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    char buf[8192]; size_t n;
    if (!a) return -1;
    b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { fclose(a); fclose(b); return -1; }
    fclose(a);
    return fclose(b) == 0 ? 0 : -1;
}

int u_publish(const char *tmp, const char *dest)
{
    /* Rename() returns ERROR_OBJECT_EXISTS rather than overwriting -- measured,
     * rom/filesys/fat/ops.c:574 -- so the destination must go first. Between the
     * delete and the rename the file is ABSENT, which is a state recovery knows
     * how to resolve from the plan. It is not a state we can avoid. */
    if (u_exists(dest) && u_delete(dest) != 0) return -1;
    return Rename((CONST_STRPTR)tmp, (CONST_STRPTR)dest) ? 0 : -1;
}

int u_write_atomicish(const char *path, const char *text)
{
    char tmp[512];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fputs(text, f) == EOF) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    return u_publish(tmp, path);
}

char *u_read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf; long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[sz] = 0;
    if (len) *len = (size_t)sz;
    return buf;
}
