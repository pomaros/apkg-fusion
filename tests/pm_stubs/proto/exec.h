/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef PM_HOST_EXEC_H
#define PM_HOST_EXEC_H
#include <exec/ports.h>
#include <stdint.h>
typedef uintptr_t IPTR;
typedef uintptr_t BPTR;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
struct Task;
#define MEMF_ANY 0
#define MEMF_PUBLIC 1
#define MEMF_CLEAR 2
#ifndef TRUE
#define TRUE 1
#endif
void *AllocVec(size_t, unsigned);
void FreeVec(void *);
void PutMsg(struct MsgPort *, struct Message *);
void Forbid(void);
void Permit(void);
struct Task *FindTask(STRPTR);
#endif
