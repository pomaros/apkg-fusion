/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Object identity for bindings and moved trees.
 *
 * The engine proves that a directory it renamed is still the one it recorded,
 * and that a root after a restart is the root its journal names, by comparing
 * st_dev/st_ino. POSIX hosts supply both. On AROS the wide
 * Examine packet supplies a stable object number on supporting handlers,
 * including AFS+. RAM handlers on older systems use the lock key. Mix the
 * volume name and creation date because a DOS list node can move at reboot.
 * Zero keys refuse mutation. Handler identity across rename/remount still
 * requires target qualification; RAM contents do not survive power loss. */
#ifndef __AROS__
#define _POSIX_C_SOURCE 200809L
#endif
#include "internal.h"
#include <errno.h>
#include <string.h>
#ifdef __AROS__
#include <dos/dos.h>
#ifdef FIBEXTF_OBJECTKEY
#include <dos/dos64.h>
#endif
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
static void native(const char *path, ak_identity *st) {
    struct InfoData *info = AllocVec(sizeof *info, MEMF_ANY | MEMF_CLEAR);
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    st->st_ino = 0;
    st->st_dev = 0;
    struct FileLock *object = lock ? BADDR(lock) : NULL;
    if (object && info && Info(lock, info) &&
        info->id_VolumeNode) {
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
        st->st_dev = h ? h : 1;
        st->st_ino = (uint64_t)object->fl_Key;
#ifdef FIBEXTF_OBJECTKEY
        /* Some handlers keep a different private address in each lock.
         * Wide Examine reports the persistent filesystem object number. */
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        if (fib) {
            fib->fib_ActExtFlags = 0;
            fib->fib_ReqExtFlags = 0;
            if (DoPkt(object->fl_Task, ACTION_EXAMINE_OBJECT64, (SIPTR)lock,
                      (SIPTR)MKBADDR(fib), 0, 0, 0) &&
                (fib->fib_ActExtFlags & FIBEXTF_OBJECTKEY))
                st->st_ino = fib->fib_ObjectKey;
            FreeDosObject(DOS_FIB, fib);
        }
#endif
    }
    if (lock)
        UnLock(lock);
    FreeVec(info);
}
int ak_stat(const char *path, ak_identity *st) {
    if (stat(path, &st->metadata))
        return -1;
    native(path, st);
    return 0;
}
int ak_lstat(const char *path, ak_identity *st) {
    if (lstat(path, &st->metadata))
        return -1;
    /* A soft link has no object of its own to name; callers refuse links
     * from st_mode before they use its identity. */
    st->st_dev = st->st_ino = 0;
    if (!S_ISLNK(st->metadata.st_mode))
        native(path, st);
    return 0;
}
#else
static void host(ak_identity *st) {
    st->st_dev = (uint64_t)st->metadata.st_dev;
    st->st_ino = (uint64_t)st->metadata.st_ino;
}
int ak_stat(const char *path, ak_identity *st) {
    int result = stat(path, &st->metadata);
    if (!result) host(st);
    return result;
}
int ak_lstat(const char *path, ak_identity *st) {
    int result = lstat(path, &st->metadata);
    if (!result) host(st);
    return result;
}
#endif
int ak_identity_known(const ak_identity *st) { return st->st_ino != 0; }
