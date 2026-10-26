/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include <exec/ports.h>
#define MEMF_PUBLIC 1
#define MEMF_CLEAR 2
void *AllocVec(size_t, unsigned);
void FreeVec(void *);
void PutMsg(struct MsgPort *, struct Message *);
