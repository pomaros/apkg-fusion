/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tomasz Staniak */
/* Whether something still uses a live package tree.
 *
 * A change renames the live tree into the journal. On AROS a lock follows its
 * object through a rename, so an assign, a current directory or an open file
 * that names the tree goes on naming the old one inside the history archive.
 * The engine therefore refuses while the tree is held.
 *
 * The test is an exclusive lock: DOS grants it only when nobody else holds a
 * lock on the object, and it is released at once. It is tried on the drawer,
 * on every drawer below it and on every file. Nothing is changed. */
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef __AROS__
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

/* 0 free, 1 held by somebody, -1 any other failure. */
static int probe(const char *path) {
    BPTR lock = Lock((CONST_STRPTR)path, EXCLUSIVE_LOCK);
    if (lock) {
        UnLock(lock);
        return 0;
    }
    if (IoErr() == ERROR_OBJECT_IN_USE)
        return 1;
    errno = EIO;
    return -1;
}
/* The drawer this program was started from is held by this process itself for
 * as long as it runs. Updating the package that carries the running client is
 * an ordinary change, so that one lock does not count. */
static int own_program_drawer(const char *path) {
    BPTR mine = GetProgramDir(), lock;
    int same;
    if (!mine)
        return 0;
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!lock)
        return 0;
    same = SameLock(lock, mine) == LOCK_SAME;
    UnLock(lock);
    return same;
}
/* path is one buffer of AK_PATH bytes, extended and cut back in place. */
static int walk(char *path, char *held, size_t size) {
    struct FileInfoBlock *fib;
    BPTR lock;
    size_t length = strlen(path);
    int result = probe(path);
    if (result == 1 && own_program_drawer(path))
        result = 0;
    if (result) {
        if (result == 1)
            snprintf(held, size, "%s", path);
        return result;
    }
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!lock || !fib || !Examine(lock, fib)) {
        if (fib)
            FreeDosObject(DOS_FIB, fib);
        if (lock)
            UnLock(lock);
        errno = EIO;
        return -1;
    }
    while (!result && ExNext(lock, fib)) {
        path[length] = 0;
        if (!AddPart((STRPTR)path, (CONST_STRPTR)fib->fib_FileName, AK_PATH)) {
            errno = ENAMETOOLONG;
            result = -1;
        } else if (fib->fib_DirEntryType == ST_USERDIR)
            result = walk(path, held, size);
        else if (fib->fib_DirEntryType < 0) {
            result = probe(path);
            if (result == 1)
                snprintf(held, size, "%s", path);
        }
        /* Links are not followed: a package tree holds files and drawers. */
    }
    if (!result && IoErr() != ERROR_NO_MORE_ENTRIES) {
        errno = EIO;
        result = -1;
    }
    path[length] = 0;
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return result;
}
int ak_in_use(const char *tree, char *held, size_t size) {
    char path[AK_PATH];
    if (!tree || !held || !size || strlen(tree) >= sizeof path) {
        errno = EINVAL;
        return -1;
    }
    held[0] = 0;
    strcpy(path, tree);
    return walk(path, held, size);
}
#else
/* A host lets an open file follow its tree without harm to the next reader
 * of the name, and has no lock that asks the question. The refusal and its
 * recovery are exercised there through the fault points of the caller. */
int ak_in_use(const char *tree, char *held, size_t size) {
    if (!tree || !held || !size) {
        errno = EINVAL;
        return -1;
    }
    held[0] = 0;
    return 0;
}
#endif
