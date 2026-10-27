/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef PM_HOST_DOS_H
#define PM_HOST_DOS_H
#include <proto/exec.h>
struct Process;
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
void Delay(unsigned long);
BPTR Open(CONST_STRPTR, long);
int Close(BPTR);
struct Process *CreateNewProcTags(IPTR, ...);
#endif
