/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
/* Primitive observations in a fresh, explicit RAM drawer. Not a durability test. */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void observed(const char *path) {
    struct stat st;
    memset(&st, 0, sizeof st);
    errno = 0;
    int result = stat(path, &st), error = errno;
    printf("stat rc=%d errno=%d mode=%o path=%s\n", result, error,
           (unsigned)(st.st_mode & 07777), path);
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (lock && fib && Examine(lock, fib))
        printf("DOS key=%llu cursor=%llu protection=%lu path=%s\n",
               (unsigned long long)((struct FileLock *)BADDR(lock))->fl_Key,
               (unsigned long long)fib->fib_DiskKey,
               (unsigned long)fib->fib_Protection, path);
    else
        printf("DOS inspect failed ioerr=%ld\n", (long)IoErr());
    if (fib) FreeDosObject(DOS_FIB, fib);
    if (lock) UnLock(lock);
}

int main(int argc, char **argv) {
    char file[256];
    if (argc != 2 || strncmp(argv[1], "RAM:APKGProbe", 13) ||
        strlen(argv[1]) > 200 || strchr(argv[1], '/') ||
        strchr(argv[1] + 4, ':'))
        return 20;
    errno = 0;
    int result = mkdir(argv[1], 0700), error = errno;
    printf("mkdir rc=%d errno=%d\n", result, error);
    if (result) return 20; /* Never operate on an existing drawer. */
    observed(argv[1]);
    snprintf(file, sizeof file, "%s/file", argv[1]);
    errno = 0;
    int fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
    error = errno;
    printf("open-write fd=%d errno=%d\n", fd, error);
    if (fd < 0) return 20;
    errno = 0; result = (int)write(fd, "body", 4); error = errno;
    printf("write rc=%d errno=%d\n", result, error);
    errno = 0; result = fsync(fd); error = errno;
    printf("fsync-write rc=%d errno=%d\n", result, error);
    errno = 0; result = close(fd); error = errno;
    printf("close-write rc=%d errno=%d\n", result, error);
    observed(argv[1]); observed(file);
    errno = 0; result = chmod(file, 0644); error = errno;
    printf("chmod-file rc=%d errno=%d\n", result, error);
    observed(file);
    errno = 0; fd = open(file, O_RDONLY); error = errno;
    printf("open-read fd=%d errno=%d\n", fd, error);
    if (fd < 0) return 20;
    errno = 0; result = fsync(fd); error = errno;
    printf("fsync-read rc=%d errno=%d\n", result, error);
    errno = 0; result = close(fd); error = errno;
    printf("close-read rc=%d errno=%d\n", result, error);
    errno = 0; result = chmod(argv[1], 0755); error = errno;
    printf("chmod-drawer rc=%d errno=%d\n", result, error);
    observed(argv[1]);
    return 0; /* Observation completion only: inspect every recorded result. */
}
