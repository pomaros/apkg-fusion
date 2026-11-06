/* SPDX-License-Identifier: MIT */
/* zk_linktext: the Linktext class compiled into a program, so that it needs
 * no class file on the system. linktext.h has the attributes, methods and
 * rules. Compile src/zk_linktext.c, src/linktext_class.c and
 * src/linktext_core.c with -DLINKTEXT_EMBEDDED. */
#ifndef ZK_LINKTEXT_H
#define ZK_LINKTEXT_H

#include <exec/types.h>
#include <intuition/classusr.h>
#include <utility/tagitem.h>
#include "linktext.h"

/* The class; 0 on failure. Once before the first object, zk_linktext_exit()
 * after the last one is disposed. */
int zk_linktext_init(void);
void zk_linktext_exit(void);

/* A Linktext object in *view; what the window tree takes is returned. With
 * `scroller` that is a group of the view and a scrollbar, without it the
 * view. extra (may be NULL) are more tags for the view. NULL on failure. */
Object *zk_linktext_new(Object **view, int scroller, const struct TagItem *extra);

#endif
