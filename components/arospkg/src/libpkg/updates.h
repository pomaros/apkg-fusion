#ifndef PKG_UPDATES_H
#define PKG_UPDATES_H
#include "pkg.h"
#include <stddef.h>

/* The pure half of pkg_updates_get: no AROS calls, so the host tests link
 * the code the guest runs. The registry is read by the caller (updates_io.c)
 * into these records; the catalogue is passed as its text. */
typedef struct {
    char id[PKG_MAXID];
    char version[64];
    long revision;
    char arch[16];       /* "" in entries that did not record it */
    char abi[8];         /* "" in entries written before 0.3 */
} upd_installed;

/* Fills out->rows (one per installed record, in the order given) and the
 * counts. `json` NULL means there is no catalogue; a catalogue that does not
 * parse sets PKG_CAT_UNREADABLE. Either way every row is still produced, as
 * PKG_UPD_NO_CATALOGUE. `abi_force` is apkg's --abi test knob (NULL or "").
 * Returns 0, or -1 when out of memory. Requirements are probed through
 * req_probe_all, which host tests replace with req_set_probe. */
int updates_compute(const char *json, size_t len,
                    const upd_installed *inst, int ninst,
                    const char *my_arch, const char *my_abi, const char *abi_force,
                    int probe, pkg_updates *out);
#endif
