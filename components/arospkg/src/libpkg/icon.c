/* The drawer icon: <root>/<id>.info, a sibling of the package directory,
 * because that is where Workbench looks for a drawer's icon.
 *
 * It is the only file this client writes outside a package's own directory,
 * and it is deliberately NOT built on a general external-file mechanism:
 * LIBS: and Fonts: need backups of what was there before, and an icon does
 * not. The rules below are the whole of it.
 *
 * An icon that was already there is never ours. Not overwritten, not backed
 * up, not removed -- even when its bytes are identical to the one in the
 * archive. Matching content proves nothing about who put it there; an earlier
 * rollback in this project deleted fifty identical files that were not its own
 * on exactly that reasoning.
 *
 * An icon we installed and that has since CHANGED is kept on removal. On this
 * platform that is not exotic: Workbench writes a drawer's window position and
 * size back into its .info when the user snapshots it. Keeping it leaves an
 * icon behind with no drawer, which is untidy; deleting it would destroy
 * something the user did. Untidy is the lesser failure, and it is reported.
 */
#include "icon.h"
#include <string.h>

int icon_manage_on_install(int package_has_icon, int present_before)
{
    return package_has_icon && !present_before;
}

icon_act icon_on_remove(int managed, int present,
                        const char *disk_sha, const char *ours)
{
    if (!managed || !present) return ICON_NOTHING;
    if (disk_sha && ours && ours[0] && strcmp(disk_sha, ours) == 0)
        return ICON_DELETE;
    return ICON_KEEP_CHANGED;
}

/* The icon is published AFTER the directory, from its own staged copy. So a
 * staged copy still in tmp/ proves the icon publish never happened -- the same
 * disk-observable proof the directory uses -- and nothing at the target is
 * ours. With the staged copy gone, content decides: our bytes are removed;
 * anything else is a file this transaction cannot account for, and recovery
 * stops rather than guess. */
icon_act icon_on_rollback(int managed, int staged_present, int present,
                          const char *disk_sha, const char *ours)
{
    if (!managed || staged_present || !present) return ICON_NOTHING;
    if (disk_sha && ours && ours[0] && strcmp(disk_sha, ours) == 0)
        return ICON_DELETE;
    return ICON_UNKNOWN;
}
