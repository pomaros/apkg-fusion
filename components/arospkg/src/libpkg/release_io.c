/* The AROS half of release.h: this build's target and the network fetch. */
#include "internal.h"
#include "release.h"
#include "net.h"
#include "util.h"

#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *pkg_release_target(void)
{
    const char *a = pkg_arch(), *b = pkg_abi();
#ifdef PKG_TARGET
    /* A build that names its own archive: the native Raspberry Pi build is
       aarch64 like the hosted one, but whether either runs on the other's
       system is not established, so neither may take the other's. */
    (void)a; (void)b;
    return PKG_TARGET;
#endif
    if (!strcmp(a, "x86_64") && !strcmp(b, "v11")) return "x86_64-aros-v11";
    if (!strcmp(a, "x86_64") && !strcmp(b, "v1"))  return "x86_64-aros-v1";
    if (!strcmp(a, "i386")   && !strcmp(b, "v0"))  return "i386-aros-v0";
    if (!strcmp(a, "aarch64"))                     return "aarch64-aros";
    return NULL;
}

pkg_status pkg_release_check(const char *base, const char *sums_file, const char *current,
                             pkg_release *out, pkg_err *e)
{
    char url[600], dest[PKG_MAXPATH], part[PKG_MAXPATH + 8], why[240];
    char *list;
    const char *t = pkg_release_target();

    memset(out, 0, sizeof *out);
    if (!t) return pkg_fail(e, PKG_E_NOT_FOUND, "this build's CPU and ABI have no release archive",
                            "Update it by hand from " PKG_RELEASE_PAGE, "");
    if (sums_file) list = u_read_all(sums_file, NULL);
    else {
        snprintf(dest, sizeof dest, "%sarospkg-release.sums", u_exists("T:") ? "T:" : "RAM:");
        snprintf(part, sizeof part, "%s.part", dest);
        snprintf(url, sizeof url, "%sSHA256SUMS", base ? base : PKG_RELEASE_LATEST);
        if (net_open(why) != 0) return pkg_fail(e, PKG_E_NETWORK, "no network", why, url);
        if (net_fetch(url, dest, 65536, why) != 0) {
            net_close(); u_delete(part);
            return pkg_fail(e, PKG_E_NETWORK, "cannot fetch the release's checksums", why, url);
        }
        net_close();
        list = u_read_all(part, NULL);
        u_delete(part);
    }
    if (!list) return pkg_fail(e, PKG_E_IO, "cannot read the release's checksums", "", sums_file ? sums_file : "");
    pkg_release_pick(list, t, out);
    free(list);
    if (out->found == 1) out->cmp = pkg_release_vercmp(current, out->version);
    return PKG_OK;
}
