/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Object identity for bindings and moved trees.
 *
 * The engine proves that a directory it renamed is still the one it recorded,
 * and that a root after a restart is the root its journal names, by comparing
 * st_dev/st_ino. POSIX hosts supply both. AROS posixc does not: st_ino is a
 * hash of the full path, so every rename changes it, and st_dev is the
 * address of the volume's DOS list node, which a reboot can change.
 *
 * On AROS the identity therefore comes from the filesystem itself: the object
 * key Examine() reports (the header block on FFS, the object node on SFS),
 * which a rename within the volume keeps, and the volume's name and creation
 * date. A handler that reports no object key gives no identity (st_ino 0);
 * the callers refuse mutation without one. */
#include "internal.h"
#include <errno.h>
#include <string.h>
#ifdef __AROS__
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

static uint64_t mix(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n--)
        h = (h ^ *b++) * 1099511628211ull; /* FNV-1a */
    return h;
}
static void native(const char *path, struct stat *st) {
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    struct InfoData *info = AllocVec(sizeof *info, MEMF_ANY | MEMF_CLEAR);
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    st->st_ino = 0;
    st->st_dev = 0;
    if (lock && fib && info && Examine(lock, fib) && Info(lock, info) &&
        fib->fib_DiskKey && info->id_VolumeNode) {
        struct DeviceList *volume = BADDR(info->id_VolumeNode);
        const unsigned char *name =
            (const unsigned char *)AROS_BSTR_ADDR(volume->dl_Name);
        size_t length;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-sign" /* the SDK's own macro */
        length = (size_t)AROS_BSTR_strlen(volume->dl_Name);
#pragma GCC diagnostic pop
        uint64_t h = 14695981039346656037ull;
        h = mix(h, name, length);
        h = mix(h, &volume->dl_VolumeDate, sizeof volume->dl_VolumeDate);
        st->st_dev = (dev_t)(h ? h : 1);
        st->st_ino = (ino_t)(uint64_t)fib->fib_DiskKey;
    }
    if (lock)
        UnLock(lock);
    FreeVec(info);
    if (fib)
        FreeDosObject(DOS_FIB, fib);
}
int ak_stat(const char *path, struct stat *st) {
    if (stat(path, st))
        return -1;
    native(path, st);
    return 0;
}
int ak_lstat(const char *path, struct stat *st) {
    if (lstat(path, st))
        return -1;
    /* A soft link has no object of its own to name; callers refuse links
     * from st_mode before they use its identity. */
    if (!S_ISLNK(st->st_mode))
        native(path, st);
    return 0;
}
#else
int ak_stat(const char *path, struct stat *st) { return stat(path, st); }
int ak_lstat(const char *path, struct stat *st) { return lstat(path, st); }
#endif
int ak_identity_known(const struct stat *st) { return st->st_ino != 0; }
