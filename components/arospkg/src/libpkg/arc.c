/* Which reader, decided by the file's own first bytes.
 *
 * Before this existed, pkg read ZIP only, and an LHA -- the Amiga's own
 * archive format -- was reported as "short central directory": the ZIP reader
 * scanned for an end-of-directory record, found something that looked like one
 * in compressed data, and failed deep inside. A user reading that would have
 * gone looking for a damaged download. Now the format is identified first and
 * an unknown one is named as such.
 */
#include "limits.h"
#include "arc.h"
#include "zip.h"
#include "lha.h"
#include <stdio.h>
#include <string.h>

const char *arc_kind(const char *archive)
{
    FILE *f = fopen(archive, "rb");
    unsigned char h[8];
    size_t n;
    if (!f) return NULL;
    n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n >= 4 && h[0] == 'P' && h[1] == 'K' &&
        (h[2] == 3 || h[2] == 5 || h[2] == 7)) return "zip";
    /* Every LHA header level carries the method id at offset 2, "-lh5-" and
     * friends, so the dashes at 2 and 6 identify the format. */
    if (n >= 7 && h[2] == '-' && h[6] == '-' &&
        (h[3] == 'l' || h[3] == 'L')) return "lha";
    return NULL;
}

int arc_extract(const char *archive, const char *dest, const char *strip,
                long max_file, long max_total,
                arc_cb cb, void *user, char why[160])
{
    const char *kind = arc_kind(archive);
    if (!kind) {
        snprintf(why, 160, "not a ZIP or LHA archive");
        return -1;
    }
    /* The LHA reader holds the archive and each member in memory. */
    if (!strcmp(kind, "lha"))
        return lha_extract(archive, dest, strip,
                           max_file > PKG_MAX_MEMBER_BUFFERED ? PKG_MAX_MEMBER_BUFFERED : max_file, max_total,
                           (lha_cb)cb, user, why);
    return zip_extract(archive, dest, strip, max_file, max_total,
                       (zip_cb)cb, user, why);
}

int arc_extract_member(const char *archive, const char *member,
                       const char *dest_file, long max_file, char why[160])
{
    const char *kind = arc_kind(archive);
    if (!kind) {
        snprintf(why, 160, "not a ZIP or LHA archive");
        return -1;
    }
    if (!strcmp(kind, "lha"))
        return lha_extract_member(archive, member, dest_file,
                                  max_file > PKG_MAX_MEMBER_BUFFERED ? PKG_MAX_MEMBER_BUFFERED : max_file, why);
    return zip_extract_member(archive, member, dest_file, max_file, why);
}
