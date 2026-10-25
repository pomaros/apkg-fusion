/* Opening an installed package's drawer on the desktop: `apkg open` and
 * PkgManager's Open folder button.
 *
 * The drawer comes from the registry entry ("dir", relative to the root),
 * never from the index or from the id: the registry is what says where this
 * installation is. The desktop is asked through workbench.library's
 * OpenWorkbenchObjectA, which takes the path as a string -- no Shell command
 * is built, so spaces and other characters in the path need no quoting --
 * and hands a drawer to whichever program registered as the desktop
 * (Wanderer on AROS One), not to a file manager named here.
 *
 * OpenWorkbenchObjectA starts a program when given a file, and searches the
 * command path when given a bare name (AROS source, openworkbenchobjecta.c).
 * So it is only called with a full path to an existing drawer. Nothing in
 * the registry or the drawer is written. */
#include "internal.h"
#include "json.h"
#include "util.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/workbench.h>
#include <workbench/workbench.h>
#include <stdlib.h>
#include <string.h>

pkg_status pkg_open_folder(pkg_ctx *c, const char *id, char *where, size_t n, pkg_err *e)
{
    char regpath[PKG_MAXPATH], dir[PKG_MAXID] = "";
    char *txt;
    size_t len;
    js_tok *t;
    int ntok;
    struct Library *WorkbenchBase;
    BOOL ok;

    if (n) where[0] = 0;
    registry_path(c, id, regpath, sizeof regpath);
    if (!(txt = u_read_all(regpath, &len)))
        return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", "", id);
    ntok = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
    if (ntok > 0) js_str(txt, t, js_member(txt, t, ntok, 0, "dir"), dir, sizeof dir);
    free(t); free(txt);
    /* Entries before the field existed were installed as <root>/<id>. */
    u_join(where, n, c->root, dir[0] ? dir : id);

    if (!u_isdir(where))
        return pkg_fail(e, PKG_E_NOT_FOUND, "the drawer recorded for it is not there",
                        "It was moved or deleted outside apkg.", where);
    if (!strpbrk(where, ":"))                   /* a bare name would search the path */
        return pkg_fail(e, PKG_E_IO, "the recorded location is not a full path", "", where);

    WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 44);
    if (!WorkbenchBase)
        return pkg_fail(e, PKG_E_IO, "there is no desktop to open it with",
                        "workbench.library 44 is not available.", where);
    ok = OpenWorkbenchObjectA((STRPTR)where, NULL);
    CloseLibrary(WorkbenchBase);
    if (!ok)
        return pkg_fail(e, PKG_E_IO, "the desktop did not open the drawer",
                        "Is Wanderer (or another desktop) running?", where);
    return PKG_OK;
}
