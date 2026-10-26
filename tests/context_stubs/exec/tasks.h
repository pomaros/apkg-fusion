/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Host-only declarations for isolated tests. */
#ifndef TEST_TASKS_H
#define TEST_TASKS_H
#include <stdint.h>
typedef uintptr_t IPTR;
struct Node { char *ln_Name; };
struct Task { struct Node tc_Node; };
#endif
