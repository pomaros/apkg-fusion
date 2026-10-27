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
 * On AROS use the lock's object key, with the volume's name and creation date.
 * fib_DiskKey is an enumeration cursor: on RAM: it changes when a child is
 * added. It cannot identify the directory. Keep the full pointer-sized lock
 * key outside POSIX ino_t, which is only 32 bits on AArch64 AROS.
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
    if (object && object->fl_Key && info && Info(lock, info) &&
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
