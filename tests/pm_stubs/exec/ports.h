/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef PM_HOST_PORTS_H
#define PM_HOST_PORTS_H
#include <stddef.h>
struct MsgPort { int unused; };
struct Message { size_t mn_Length; struct MsgPort *mn_ReplyPort; };
#endif
