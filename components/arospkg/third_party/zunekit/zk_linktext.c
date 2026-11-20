/* SPDX-License-Identifier: MIT */
#define MUIMASTER_YES_INLINE_STDARG
#include <libraries/mui.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>
#include "zk_linktext.h"

extern ULONG Linktext_DataSize(void);
extern APTR Linktext_Entry(void);

static struct MUI_CustomClass *mcc;

int zk_linktext_init(void)
{
    if (!mcc) mcc = MUI_CreateCustomClass(NULL, (ClassID)MUIC_Area, NULL, Linktext_DataSize(), Linktext_Entry());
    return mcc != NULL;
}

void zk_linktext_exit(void)
{
    if (mcc) MUI_DeleteCustomClass(mcc);
    mcc = NULL;
}

Object *zk_linktext_new(Object **view, int scroller, const struct TagItem *extra)
{
    Object *bar = NULL, *v, *g;
    struct TagItem tags[] = {
        { MUIA_Frame, MUIV_Frame_ReadList },
        { MUIA_Background, MUII_ReadListBack },
        { MUIA_Font, MUIV_Font_List },
        { MUIA_CycleChain, 1 },
        { MUIA_Linktext_Scroller, 0 },
        { TAG_MORE, (IPTR)extra },
    };
    if (!mcc) return NULL;
    if (scroller && !(bar = ScrollbarObject, End)) return NULL;
    tags[4].ti_Data = (IPTR)bar;
    if (!extra) tags[5].ti_Tag = TAG_DONE;
    v = NewObjectA(mcc->mcc_Class, NULL, tags);
    if (!v) { if (bar) MUI_DisposeObject(bar); return NULL; }
    if (view) *view = v;
    if (!bar) return v;
    g = HGroup, MUIA_Group_Spacing, 0, Child, (IPTR)v, Child, (IPTR)bar, End;
    return g;
}
