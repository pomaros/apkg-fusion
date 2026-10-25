/* pkg_err and pkg_status, kept apart from core.c for one reason: core.c needs
 * proto/exec.h and proto/dos.h, which do not exist on the host, and the host
 * is where the unit tests run. Nothing in here touches the platform, so the
 * tests link the same code the AROS build does rather than a copy of it that
 * can drift. */
#include "internal.h"
#include <stdio.h>

const char *pkg_strstatus(pkg_status s)
{
    switch (s) {
    case PKG_OK:               return "ok";
    case PKG_E_LOCKED:         return "locked by another instance";
    case PKG_E_STALE_LOCK:     return "stale lock";
    case PKG_E_UNRESOLVED_TXN: return "unresolved transaction";
    case PKG_E_NOT_FOUND:      return "not found";
    case PKG_E_EXISTS:         return "already installed";
    case PKG_E_VERIFY:         return "verification failed";
    case PKG_E_CONFLICT:       return "conflict";
    case PKG_E_ARCHIVE:        return "bad archive";
    case PKG_E_IO:             return "i/o error";
    case PKG_E_NOMEM:          return "out of memory";
    case PKG_E_INTERRUPTED:    return "interrupted on purpose";
    case PKG_E_NETWORK:        return "network error";
    case PKG_E_REQUIRES:       return "system requirement not met";
    case PKG_E_CANCELLED:      return "cancelled";
    case PKG_E_LAUNCH:         return "cannot be started";
    case PKG_E_REQUIRED:       return "needed by another package";
    }
    return "?";
}

pkg_status pkg_fail(pkg_err *e, pkg_status s, const char *sum,
                       const char *det, const char *subj)
{
    if (e) {
        e->status = s;
        snprintf(e->summary, sizeof e->summary, "%s", sum ? sum : pkg_strstatus(s));
        snprintf(e->detail,  sizeof e->detail,  "%s", det ? det : "");
        snprintf(e->subject, sizeof e->subject, "%s", subj ? subj : "");
    }
    return s;
}
