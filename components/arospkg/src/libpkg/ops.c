#include "limits.h"
#include "internal.h"
#include "app.h"
#include "entries.h"

static void net_prog_shim(void *user, unsigned long done, unsigned long total, int *cancel);
#include "util.h"
#include "json.h"
#include "sha256.h"
#include "arc.h"
#include "net.h"
#include "req.h"
#include "icon.h"
#include "listing.h"
#include "slow.h"

#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define STOP(want) do { if (stop == (want)) \
    return pkg_fail(e, PKG_E_INTERRUPTED, "stopped on purpose", #want, id); } while (0)

void now_iso(char out[32])
{
    time_t t = time(NULL);
    struct tm *g = gmtime(&t);
    if (g) strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", g);
    else   snprintf(out, 32, "unknown");
}

void txn_id(char out[64], const char *id)
{
    snprintf(out, 64, "%lx-%s", (unsigned long)time(NULL), id);
}

/* ------------------------------------------------------------- registry io */

void registry_path(pkg_ctx *c, const char *id, char *out, size_t n)
{ snprintf(out, n, "%s/%s.json", c->installed, id); }

char *registry_text(const char *id, const char *version, long revision,
                    const char *arch, const char *abi, const char *subdir,
                    const char *url, const char *sha, long size,
                    inv_ent *inv, int n, req_ent *reqs, int nreq,
                    const char *icon_state, const char *icon_sha, long icon_size,
                    const char *icon_member,
                    const char *prev_registry, const char *prev_archive,
                    const char *notes, const pkg_launch *launch,
                    const char *reason, const char *rel_json)
{
    size_t cap = 4096 + REL_JSON_MAX + PKG_NOTES_MAX * 2 + (size_t)n * 400 + (size_t)nreq * 400;
    char *buf = (char *)malloc(cap);
    js_out o; char when[32]; int i;
    if (!buf) return NULL;
    now_iso(when);
    js_out_init(&o, buf, cap);
    js_raw(&o, "{\n  "); js_key(&o, "schema"); js_vlong(&o, 1);
    js_raw(&o, ",\n  "); js_key(&o, "name");    js_vstr(&o, id);
    js_raw(&o, ",\n  "); js_key(&o, "version"); js_vstr(&o, version);
    /* Our revision of that upstream version; 0 when the index stated none.
       An upgrade needs it to know what it is replacing. */
    js_raw(&o, ",\n  "); js_key(&o, "revision"); js_vlong(&o, revision);
    js_raw(&o, ",\n  "); js_key(&o, "arch");    js_vstr(&o, arch);
    /* The target installed, so an upgrade stays on it rather than crossing
       to another build of the same program. Absent in entries written
       before 0.3; readers then take this client's ABI. */
    js_raw(&o, ",\n  "); js_key(&o, "abi");     js_vstr(&o, abi ? abi : "");
    /* Which part of the archive is the package. A rollback re-extracts the
       previous archive and must cut it the same way the install did. */
    js_raw(&o, ",\n  "); js_key(&o, "subdir");  js_vstr(&o, subdir ? subdir : "");
    js_raw(&o, ",\n  "); js_key(&o, "installed"); js_vstr(&o, when);
    js_raw(&o, ",\n  "); js_key(&o, "origin");  js_vstr(&o, url);
    js_raw(&o, ",\n  "); js_key(&o, "archive_sha256"); js_vstr(&o, sha);
    js_raw(&o, ",\n  "); js_key(&o, "archive_size");   js_vlong(&o, size);
    /* "explicit": asked for; "dependency": installed because another
       package needs it. Kept across upgrades. */
    js_raw(&o, ",\n  "); js_key(&o, "reason");  js_vstr(&o, reason && *reason ? reason : "explicit");
    /* The relations as the catalogue declared them for this build, so a
       removal or a conflict check needs no catalogue. relations_schema
       says they were recorded, also when there are none; with rel_json
       NULL they are not known (a rollback to an entry from before 0.6), and
       readers take the catalogue's word instead. */
    if (rel_json) {
        js_raw(&o, ",\n  "); js_key(&o, "relations_schema"); js_vlong(&o, 1);
        js_raw(&o, rel_json);
    }
    js_raw(&o, ",\n  "); js_key(&o, "dir");     js_vstr(&o, id);
    /* The requirement verdicts are part of the record of this install, not of
     * the package: the same archive installed on another machine can produce
     * different ones, and an "undetermined" that is not written down here is
     * an install nobody can later account for. */
    js_raw(&o, ",\n  "); req_emit(&o, reqs, nreq);
    /* "installed" is the only state that makes the icon ours. "kept-existing"
     * records that one was already there and was deliberately left alone, so
     * removal must not touch it; "none" means the package brings no icon. */
    js_raw(&o, ",\n  "); js_key(&o, "icon"); js_raw(&o, " { ");
    js_key(&o, "state");  js_vstr(&o, icon_state); js_raw(&o, ", ");
    js_key(&o, "sha256"); js_vstr(&o, icon_sha);   js_raw(&o, ", ");
    js_key(&o, "size");   js_vlong(&o, icon_size);  js_raw(&o, ", ");
    js_key(&o, "member"); js_vstr(&o, icon_member ? icon_member : "");
    js_raw(&o, " }");
    /* What a rollback needs: the registry entry and the archive of the
       revision this one replaced. Empty when there is nothing to go back to. */
    js_raw(&o, ",\n  "); js_key(&o, "previous"); js_raw(&o, " { ");
    js_key(&o, "registry"); js_vstr(&o, prev_registry ? prev_registry : ""); js_raw(&o, ", ");
    js_key(&o, "archive");  js_vstr(&o, prev_archive ? prev_archive : "");
    js_raw(&o, " }");
    /* The notes as the index had them when this was installed: what the
       user was told, readable later without the index. Not a second place
       to maintain them; the index's current text is shown beside it. */
    if (notes && *notes) {
        const char *p = notes;
        js_raw(&o, ",\n  "); js_key(&o, "post_install_notes"); js_raw(&o, " [");
        for (i = 0; *p; i++) {
            char line[PKG_NOTES_LINE + 1]; size_t k = strcspn(p, "\n");
            snprintf(line, sizeof line, "%.*s", (int)k, p);
            js_raw(&o, i ? ", " : ""); js_vstr(&o, line);
            p += k; if (*p) p++;
        }
        js_raw(&o, "]");
    }
    /* How it is started, as the catalogue said at installation. The drawer's
       application.json is what starts it; this copy lets a rollback, which
       re-extracts an archive that may not carry the file, write it again. */
    if (launch && launch->mode != PKG_LAUNCH_NONE) { js_raw(&o, ",\n  "); launch_emit(&o, launch); }
    js_raw(&o, ",\n  "); js_key(&o, "contents"); js_raw(&o, " [");
    for (i = 0; i < n; i++) {
        js_raw(&o, i ? ",\n    { " : "\n    { ");
        js_key(&o, "path");   js_vstr(&o, inv[i].rel);  js_raw(&o, ", ");
        js_key(&o, "sha256"); js_vstr(&o, inv[i].sha);  js_raw(&o, ", ");
        js_key(&o, "size");   js_vlong(&o, inv[i].size);
        js_raw(&o, " }");
    }
    js_raw(&o, "\n  ]\n}\n");
    if (o.fail) { free(buf); return NULL; }
    return buf;
}

int app_stage(const char *staging, const char *id, const char *version, long revision,
              const char *arch, const char *abi, const req_ent *reqs, int nreq,
              const pkg_launch *l)
{
    char path[PKG_MAXPATH];
    char *txt;
    int rc;
    if (!l || l->mode == PKG_LAUNCH_NONE) return 0;
    u_join(path, sizeof path, staging, PKG_APP_FILE);
    /* The archive's own copy stands: apkg-pack wrote it from the same
       manifest the catalogue entry came from, and the catalogue's checks
       compare the two before an entry is published. */
    if (u_exists(path)) return 0;
    if (!(txt = app_text(id, version, revision, arch, abi, reqs, nreq, l))) return -1;
    rc = u_write_atomicish(path, txt);
    free(txt);
    return rc ? -1 : 0;
}

/* An EMPTY directory of the package's name is not an obstacle and must not be
 * treated as one. Removing a package whose program is still running leaves
 * exactly that: every file goes, but the drawer cannot be deleted while the
 * program holds it as its current directory. Measured on AROS One, 2026-09-20.
 * The publish deletes an empty directory on its way past, so only a directory
 * with something in it means the user has files here. */
static int dir_is_empty(const char *path)
{
    char **names; int n, i, empty = 1;
    if (!u_isdir(path)) return 0;
    if (u_listdir(path, &names, &n) != 0) return 0;
    for (i = 0; i < n; i++) empty = 0;
    u_freelist(names, n);
    return empty;
}


/* --keep-rejected. A download that failed verification is moved aside, not
 * deleted, so the bytes can be compared with the real file. It stays refused:
 * nothing installs from it, and <archive>.rejected is a name no lookup reads
 * as a cached archive, while the next download goes to <archive>.part as
 * before. A newer rejection replaces an older one, so there is only ever one
 * file to send. The hashes separate the suspects: the body in memory before
 * the write (network, TLS, memory), the file as verification read it, and a
 * second read of the same file (an unstable read path). Added 2026-10-08 for
 * a mismatch on a real Raspberry Pi that QEMU did not reproduce. */
static void reject_download(const char *part, const char *archive,
                            const char *want_sha, long want_size,
                            const char *first)
{
    char kept[PKG_MAXPATH], again[65] = "", one[65] = "";
    const char *mem = net_body_sha256();
    long n1 = -1, n2 = -1, sz;
    int r1 = 0, r2;
    if (!pkg_keep_rejected()) { u_delete(part); return; }
    snprintf(kept, sizeof kept, "%s.rejected", archive);
    if (u_publish(part, kept) != 0) {
        printf("Could not rename the rejected download; it is left as %s\n", part);
        snprintf(kept, sizeof kept, "%s", part);
    }
    sz = u_size(kept);
    if (!first) {
        r1 = sha256_file_count(kept, one, &n1);
        first = r1 == 0 ? one : "(read error)";
    }
    r2 = sha256_file_count(kept, again, &n2);
    printf("\nKept the rejected download for diagnosis. It was NOT installed.\n"
           "  file      %s\n"
           "  size      %ld bytes (index: %ld)\n"
           "  expected  %s\n"
           "  memory    %s\n"
           "  disk      %s\n"
           "  re-read   %s (%ld bytes read)\n\n",
           kept, sz, want_size, want_sha,
           mem && *mem ? mem : "(not computed)",
           first, r2 == 0 ? again : "(read error)", n2);
}

pkg_status fetch_verified(pkg_ctx *c, const char *id, const char *url,
                          const char *want_sha, long want_size,
                          const char *archive, pkg_err *e)
{
    pkg_status st = PKG_E_IO;
    {
        int present = u_exists(archive), hash_ok = 0, size_ok = 0;
        if (present) {
            char got[65];
            hash_ok = sha256_file(archive, got) == 0 && strcmp(got, want_sha) == 0;
            size_ok = want_size < 0 || u_size(archive) == want_size;
        }
        if (pkg_cache_decision(present, hash_ok, size_ok) == CACHE_DISCARD_AND_FETCH) {
            printf("The cached archive does not match the catalogue; downloading it again.\n");
            if (u_delete(archive) != 0) {
                return pkg_fail(e, PKG_E_IO, "cannot discard the stale cached copy",
                              "Delete it and try again.", archive);
            }
        }
    }
    if (!u_exists(archive)) {
        /* Download to <archive>.part, verify, and only then publish, so an
           interrupted download can never be picked up as a complete file. */
        char why[240], part[PKG_MAXPATH], got[65];
        if (net_open(why) != 0) {
            st = pkg_fail(e, PKG_E_NETWORK, "no network", why, url); return st;
        }
        if (pkg_verbose()) printf("  from %s\n", url);
        if (prog(c, "download", 0, want_size > 0 ? (unsigned long)want_size : 0, 1)) {
            net_close();
            st = pkg_fail(e, PKG_E_CANCELLED, "cancelled before the download", "", id); return st;
        }
        net_set_progress(c->progress ? net_prog_shim : NULL, c);
        if (net_fetch(url, archive, PKG_MAX_DOWNLOAD, why) != 0) {
            net_set_progress(NULL, NULL);
            net_close();
            st = strcmp(why, "cancelled") == 0
               ? pkg_fail(e, PKG_E_CANCELLED, "download cancelled",
                          "Nothing was written; the cache has no partial file.", id)
               : pkg_fail(e, PKG_E_NETWORK, "cannot fetch the archive", why, url);
            return st;
        }
        net_set_progress(NULL, NULL);
        net_close();
        snprintf(part, sizeof part, "%s.part", archive);
        if (want_size >= 0 && u_size(part) != want_size) {
            reject_download(part, archive, want_sha, want_size, NULL);
            st = pkg_fail(e, PKG_E_VERIFY, "the download is the wrong size",
                          "Nothing was added to the cache.", url); return st;
        }
        if (sha256_file(part, got) != 0) {
            reject_download(part, archive, want_sha, want_size, NULL);
            st = pkg_fail(e, PKG_E_IO, "cannot read the download back",
                          "Nothing was added to the cache.", part); return st;
        }
        if (strcmp(got, want_sha) != 0) {
            reject_download(part, archive, want_sha, want_size, got);
            st = pkg_fail(e, PKG_E_VERIFY, "the download does not match the index",
                          got, url); return st;
        }
        if (u_publish(part, archive) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot move the download into the cache",
                          "", archive); return st;
        }
        if (pkg_verbose()) printf("  verified and cached %s\n", archive);
    }
    {
        char got[65];
        long sz = u_size(archive);
        if (sha256_file(archive, got) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot hash the archive", "", archive); return st;
        }
        if (strcmp(got, want_sha) != 0) {
            st = pkg_fail(e, PKG_E_VERIFY, "the archive does not match the index",
                          got, archive); return st;
        }
        if (want_size >= 0 && sz != want_size) {
            st = pkg_fail(e, PKG_E_VERIFY, "the archive is the wrong size", "", archive); return st;
        }
    }
    return PKG_OK;
}

/* ----------------------------------------------------------------- install */

pkg_status pkg_install(pkg_ctx *c, const char *index_path, const char *id,
                       pkg_stop stop, pkg_err *e)
{
    c->progress_id = id;
    const char *abi_force = pkg_abi_override();
    char *idx = NULL, *plan = NULL, *reg = NULL, *rel_json = NULL;
    js_tok *t = NULL;
    int ntok, arr, i, found = -1;
    char version[64] = "", arch[16] = "", url[512] = "", want_sha[65] = "", want_abi[16] = "";
    char subdir[128] = "", archive[PKG_MAXPATH], staging[PKG_MAXPATH];
    char notes[PKG_NOTES_MAX] = "";
    pkg_launch launch;
    char dest[PKG_MAXPATH], regpath[PKG_MAXPATH], tid[64], why[160];
    long want_size = 0, revision = 0;
    inv_ent *inv = NULL;
    int ninv = 0;
    req_ent reqs[MAXREQ];
    int nreq = 0;
    char icon_src[128] = "", icon_dst[PKG_MAXPATH] = "", icon_stage[PKG_MAXPATH] = "";
    char icon_sha[65] = "";
    long icon_size = -1;
    int icon_managed = 0, icon_before = 0;
    size_t len;
    pkg_status st = PKG_E_IO;

    registry_path(c, id, regpath, sizeof regpath);
    if (u_exists(regpath))
        return pkg_fail(e, PKG_E_EXISTS, "already installed", "Remove it first if you want to install it again.", id);

    /* A directory of this name with no registry entry is refused BEFORE the
     * download. The usual way to get one is a removal that kept files the user
     * had changed -- which is exactly what removal promises to do -- and the
     * install would otherwise fail much later, after downloading and staging,
     * when the publish finds a non-empty directory it may not delete. This is
     * the honest limit of remove-then-install, and it is why that pair is not
     * an upgrade. */
    snprintf(dest, sizeof dest, "%s/%s", c->root, id);
    if (u_exists(dest) && !dir_is_empty(dest))
        return pkg_fail(e, PKG_E_CONFLICT, "a directory of that name is already there",
                        "Usually left by an earlier removal that kept files you "
                        "had changed. Nothing was changed. Move it away and "
                        "install again.", dest);

    idx = u_read_all(index_path, &len);
    if (!idx) return pkg_fail(e, PKG_E_NOT_FOUND, "cannot read the index",
                              "Run pkg update to fetch one.", index_path);
    ntok = js_parse_alloc(idx, len, &t, PKG_JSON_MAXTOK);
    if (ntok < 0) { st = index_unreadable(e, ntok, index_path); goto out; }

    arr = js_member(idx, t, ntok, 0, "packages");
    {
        int how, same;
        found = index_select_variant(idx, t, ntok, arr, id, pkg_arch(),
                                     abi_force && *abi_force ? abi_force : pkg_abi(), &how, &same);
        if (found >= 0 && how == VARIANT_SEVERAL) {
            snprintf(why, sizeof why, "The index lists %d entries of it for %s/%s. Refusing rather "
                     "than picking one.", same, pkg_arch(), abi_force && *abi_force ? abi_force : pkg_abi());
            st = pkg_fail(e, PKG_E_CONFLICT, "more than one build of this package for this system", why, id);
            goto out;
        }
    }
    if (found < 0) {
        st = pkg_fail(e, PKG_E_NOT_FOUND, "no such package in the index",
                      "The index lists only packages that have been reviewed. "
                      "Run pkg search to see them.", id);
        goto out;
    }

    js_str(idx, t, js_member(idx, t, ntok, found, "version"), version, sizeof version);
    js_str(idx, t, js_member(idx, t, ntok, found, "arch"),    arch,    sizeof arch);
    js_str(idx, t, js_member(idx, t, ntok, found, "url"),     url,     sizeof url);
    js_str(idx, t, js_member(idx, t, ntok, found, "sha256"),  want_sha, sizeof want_sha);
    js_str(idx, t, js_member(idx, t, ntok, found, "subdir"),  subdir,  sizeof subdir);
    want_size = js_long(idx, t, js_member(idx, t, ntok, found, "size"), -1);
    revision  = js_long(idx, t, js_member(idx, t, ntok, found, "revision"), 0);
    /* A launch table this client cannot use is left out: the package still
       installs, and Open folder still works, as for one that has none. */
    if (launch_from_object(idx, t, ntok, js_member(idx, t, ntok, found, "launch"),
                           &launch, why, sizeof why) != 1)
        memset(&launch, 0, sizeof launch);

    /* Relations: a declaration this client cannot read, or a feature it
       lacks, refuses the package -- installing it without what it needs and
       calling that a success is what older clients did. Outside a plan the
       package must need nothing else: pkg_plan_install says what. */
    {
        char rwhy[200];
        if (!(rel_json = (char *)malloc(REL_JSON_MAX))) { st = pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); goto out; }
        if (rel_fragment(idx, t, ntok, found, rel_json, REL_JSON_MAX, rwhy, sizeof rwhy) != 0) {
            st = pkg_fail(e, PKG_E_REQUIRES, "this package's relations are not readable by this apkg", rwhy, id);
            goto out;
        }
    }
    if (!c->in_plan) {
        pkg_status rs = rel_check_alone(c, index_path, id, 0, NULL, NULL, e);
        if (rs != PKG_OK) { st = rs; goto out; }
    }

    {
        pkg_status a;
        js_str(idx, t, js_member(idx, t, ntok, found, "abi"), want_abi, sizeof want_abi);
        a = target_check(id, arch, want_abi, abi_force, e);
        if (a != PKG_OK) { st = a; goto out; }
    }

    /* Beside the ABI gate, and for the same reason: both are properties of
     * this machine, both are decidable before a byte is downloaded, and a
     * package that fails either would install perfectly and then not start.
     * Checking after the download would still refuse, but it would have spent
     * the user's bandwidth to learn something it already knew. */
    {
        char rwhy[200];
        pkg_status rs;
        if (req_parse(idx, t, ntok, found, reqs, MAXREQ, &nreq,
                      rwhy, sizeof rwhy) != 0) {
            st = pkg_fail(e, PKG_E_REQUIRES,
                          "this package's system requirements are not readable",
                          rwhy, id);
            goto out;
        }
        rs = req_check(id, reqs, nreq, e);
        if (rs != PKG_OK) { st = rs; goto out; }
    }

    if (strlen(want_sha) != 64) {
        st = pkg_fail(e, PKG_E_VERIFY, "the index has no hash for this package",
                      "An unverified download is exactly what this refuses to do.", id);
        goto out;
    }

    /* The cached copy keeps the URL's extension, so a .lha stays a .lha on
     * disk. The reader does not care -- it looks at the bytes -- but a cache
     * anyone can inspect should not call an LHA a zip. */
    {
        const char *dot = strrchr(url, '.');
        char ext[8] = "bin";
        if (dot && strlen(dot + 1) < sizeof ext) {
            size_t k;
            for (k = 0; dot[1 + k]; k++) {
                char ch = dot[1 + k];
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) { k = 0; break; }
                ext[k] = ch;
            }
            if (k) ext[k] = 0; else snprintf(ext, sizeof ext, "bin");
        }
        snprintf(archive, sizeof archive, "%s/%s.%s", c->cache, id, ext);
    }
    st = fetch_verified(c, id, url, want_sha, want_size, archive, e);
    if (st != PKG_OK) goto out;
    /* The last point at which stopping costs nothing: the archive is verified
       and cached, and no transaction exists yet. */
    if (prog(c, "verify", 1, 1, 1)) {
        st = pkg_fail(e, PKG_E_CANCELLED, "cancelled after verification",
                      "The archive stays in the cache; nothing else was touched.", id);
        goto out;
    }

    /* The staging path is unique to the TRANSACTION, not to the package.
     *
     * Recovery treats "the staged tree is still present" as proof that the
     * publish never happened. That inference is only sound if the path cannot
     * belong to some other run: a per-package path made the proof depend on
     * the root lock to keep runs apart, and a lock constrains concurrency
     * without establishing whose leftovers these are after a crash. The id is
     * therefore minted first and the path recorded in the plan, so a staged
     * tree identifies exactly one transaction. */
    txn_id(tid, id);
    snprintf(staging, sizeof staging, "%s/%s/%s", c->tmp, tid, id);
    {
        char sroot[PKG_MAXPATH];
        snprintf(sroot, sizeof sroot, "%s/%s", c->tmp, tid);
        if (u_exists(sroot)) {
            st = pkg_fail(e, PKG_E_CONFLICT, "the staging path is already in use",
                          "It must never be reused; refusing rather than guessing.",
                          sroot);
            goto out;
        }
        if (u_mkdir(sroot) != 0 || u_mkdir(staging) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot create staging", "", staging); goto out;
        }
    }
    prog(c, "extract", 0, want_size > 0 ? (unsigned long)want_size : 0, 0);
    if (pkg_slow_ms > 0) Delay(pkg_slow_ticks(pkg_slow_ms)); /* testing only */
    if (arc_extract(archive, staging, subdir, PKG_MAX_MEMBER, PKG_MAX_TOTAL,
                    NULL, NULL, why) != 0) {
        st = pkg_fail(e, PKG_E_ARCHIVE, "the archive was rejected", why, archive);
        goto out;
    }
    prog(c, "extract", 1, 1, 0);
    if (app_stage(staging, id, version, revision, arch, want_abi, reqs, nreq, &launch) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot write the application description", "", staging); goto out;
    }

    inv = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    if (!inv) { st = pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); goto out; }
    if (inv_of_tree(staging, "", inv, &ninv, MAXINV) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot inventory the staged tree", "", staging); goto out;
    }

    /* The drawer icon is staged beside the tree, inside this transaction's own
     * tmp root, so the same "staged copy still present" proof covers it. An
     * index that names an icon the archive does not contain is refused: the
     * metadata is wrong, and installing without the icon would hide that. */
    js_str(idx, t, js_member(idx, t, ntok, found, "icon"), icon_src, sizeof icon_src);
    /* Notes the client cannot show as written are not recorded either. */
    if (notes_from_element(idx, t, ntok, found, notes, sizeof notes) < 0) notes[0] = 0;
    if (icon_src[0]) {
        snprintf(icon_dst, sizeof icon_dst, "%s/%s.info", c->root, id);
        snprintf(icon_stage, sizeof icon_stage, "%s/%s/%s.info", c->tmp, tid, id);
        if (arc_extract_member(archive, icon_src, icon_stage, 1L * 1024 * 1024, why) != 0) {
            /* Only a missing member is the index's fault; say which it was. */
            st = pkg_fail(e, PKG_E_ARCHIVE,
                          strncmp(why, "no member", 9) == 0
                              ? "the index names an icon the archive lacks"
                              : "cannot extract the drawer icon",
                          why, archive);
            goto out;
        }
        if (sha256_file(icon_stage, icon_sha) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot hash the icon", "", icon_stage); goto out;
        }
        icon_size = u_size(icon_stage);
        icon_before = u_exists(icon_dst);
        icon_managed = icon_manage_on_install(1, icon_before);
        if (!icon_managed)
            printf("note: %s was already there and is left as it is.\n"
                   "      It is not part of this package and removal will not touch it.\n",
                   icon_dst);
    }

    /* The plan carries the full inventory, which is possible only because
     * staging happens first -- and it is what lets recovery undo the publish
     * without consulting a registry that may not exist yet. */
    {
        char dir[PKG_MAXPATH];
        snprintf(dir, sizeof dir, "%s/%s", c->txns, tid);
        if (u_mkdir(dir) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot create the transaction", "", dir); goto out; }
    }
    {
        size_t cap = 4096 + (size_t)ninv * 400;
        js_out o;
        plan = (char *)malloc(cap);
        if (!plan) { st = pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); goto out; }
        js_out_init(&o, plan, cap);
        js_raw(&o, "{\n  "); js_key(&o, "op");      js_vstr(&o, "install");
        js_raw(&o, ",\n  "); js_key(&o, "package"); js_vstr(&o, id);
        js_raw(&o, ",\n  "); js_key(&o, "dir");     js_vstr(&o, dest);
        js_raw(&o, ",\n  "); js_key(&o, "dir_before"); js_vstr(&o, u_exists(dest) ? "present" : "absent");
        js_raw(&o, ",\n  "); js_key(&o, "staging");    js_vstr(&o, staging);
        js_raw(&o, ",\n  "); js_key(&o, "registry"); js_vstr(&o, regpath);
        js_raw(&o, ",\n  "); js_key(&o, "registry_before"); js_vstr(&o, u_exists(regpath) ? "present" : "absent");
        js_raw(&o, ",\n  "); js_key(&o, "icon"); js_raw(&o, " { ");
        js_key(&o, "path");    js_vstr(&o, icon_dst);   js_raw(&o, ", ");
        js_key(&o, "staged");  js_vstr(&o, icon_stage); js_raw(&o, ", ");
        js_key(&o, "sha256");  js_vstr(&o, icon_sha);   js_raw(&o, ", ");
        js_key(&o, "managed"); js_vlong(&o, icon_managed);
        js_raw(&o, " }");
        js_raw(&o, ",\n  "); js_key(&o, "contents"); js_raw(&o, " [");
        for (i = 0; i < ninv; i++) {
            js_raw(&o, i ? ",\n    { " : "\n    { ");
            js_key(&o, "path");   js_vstr(&o, inv[i].rel);  js_raw(&o, ", ");
            js_key(&o, "sha256"); js_vstr(&o, inv[i].sha);  js_raw(&o, ", ");
            js_key(&o, "size");   js_vlong(&o, inv[i].size);
            js_raw(&o, " }");
        }
        js_raw(&o, "\n  ]\n}\n");
        if (o.fail || txn_write_plan(c, tid, plan) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot write or re-read the plan",
                          "Nothing outside staging has been touched.", tid);
            goto out;
        }
    }

    STOP(PKG_STOP_AFTER_PLAN);

    if (stop == PKG_STOP_IN_PUBLISH) {
        /* Reproduce the one window u_publish cannot avoid: Rename() refuses to
           overwrite, so an existing destination is deleted first, and a crash
           here leaves the destination ABSENT with the staged tree still in
           tmp/. Recovery must read that from the plan, not guess. */
        if (u_exists(dest)) u_delete(dest);
        return pkg_fail(e, PKG_E_INTERRUPTED, "stopped on purpose",
                        "PKG_STOP_IN_PUBLISH", id);
    }
    prog(c, "publish", 0, 1, 0);
    if (u_publish(staging, dest) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot move the package into place", "", dest); goto out;
    }
    prog(c, "publish", 1, 1, 0);
    STOP(PKG_STOP_AFTER_DIR);
    txn_marker(c, tid, "001-dir-done");        /* audit only */
    STOP(PKG_STOP_AFTER_DIR_MARKER);

    if (icon_managed) {
        /* Checked again at the moment of writing: something may have put an
         * icon there since the plan. It is then not ours, and it is left. The
         * plan still says managed, which recovery resolves correctly -- the
         * staged copy is still in tmp/, proving this publish never happened. */
        if (u_exists(icon_dst)) {
            icon_managed = 0;
            printf("note: %s appeared during the install and is left as it is.\n", icon_dst);
        } else if (u_publish(icon_stage, icon_dst) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot put the drawer icon in place",
                          "The transaction is kept; the next run rolls it back.", icon_dst);
            goto out;
        } else {
            STOP(PKG_STOP_AFTER_ICON);
            txn_marker(c, tid, "002-icon-done");   /* audit only */
        }
    }

    reg = registry_text(id, version, revision, arch, want_abi, subdir, url, want_sha, want_size, inv, ninv,
                        reqs, nreq,
                        icon_src[0] ? (icon_managed ? "installed" : "kept-existing")
                                    : "none",
                        icon_managed ? icon_sha : "", icon_managed ? icon_size : -1,
                        icon_src, "", "", notes, &launch,
                        c->install_reason[0] ? c->install_reason : "explicit", rel_json);
    if (!reg || u_write_atomicish(regpath, reg) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot write the registry entry", "", regpath); goto out;
    }
    STOP(PKG_STOP_AFTER_REGISTRY);
    txn_marker(c, tid, "004-registry-written");   /* audit only */
    STOP(PKG_STOP_BEFORE_COMMIT);

    /* 005-commit is the one bit recovery reads. If it cannot be written the
     * install is NOT finished: keep the transaction so the next run rolls it
     * back, rather than reporting success over a journal we then delete. */
    if (txn_marker(c, tid, "005-commit") != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot record the commit",
                      "The transaction is kept; the next run will roll it back.", tid);
        goto out;
    }
    {
        char dir[PKG_MAXPATH];
        snprintf(dir, sizeof dir, "%s/%s", c->txns, tid);
        u_deltree(dir);
    }
    st = PKG_OK;
out:
    free(idx); free(t); free(plan); free(reg); free(inv); free(rel_json);
    return st;
}

/* The record of what a removal deliberately left behind. Written BEFORE the
 * registry is deleted, because the registry is the only other place that says
 * whose those files are. Shared by pkg_remove and by recovery's roll-forward of
 * an interrupted removal: the second used to delete the registry without it,
 * which made a kept file unattributable on exactly the path F2 exists to
 * protect -- found by the v0.1 release suite, not by reading. */
int leftovers_report(pkg_ctx *c, const char *id, const char *dest,
                     inv_ent *inv, int ninv, int removed, int kept,
                     const char *kept_icon)
{
    char rp[PKG_MAXPATH], when[32];
    js_out o; char buf[8192]; int i, emitted = 0;
    now_iso(when);
    snprintf(rp, sizeof rp, "%s/%s-%lx.json", c->doctor, id, (unsigned long)time(NULL));
    js_out_init(&o, buf, sizeof buf);
    js_raw(&o, "{ "); js_key(&o, "package"); js_vstr(&o, id);
    js_raw(&o, ", "); js_key(&o, "when");    js_vstr(&o, when);
    js_raw(&o, ", "); js_key(&o, "dir");     js_vstr(&o, dest);
    js_raw(&o, ", "); js_key(&o, "removed"); js_vlong(&o, removed);
    js_raw(&o, ", "); js_key(&o, "kept_modified"); js_vlong(&o, kept);
    js_raw(&o, ", "); js_key(&o, "kept_icon"); js_vstr(&o, kept_icon ? kept_icon : "");
    js_raw(&o, ", "); js_key(&o, "kept"); js_raw(&o, " [");
    for (i = 0; i < ninv; i++) {
        char p[PKG_MAXPATH];
        u_join(p, sizeof p, dest, inv[i].rel);
        if (!u_exists(p)) continue;
        /* The separator counts what was emitted, not the inventory index:
           keying it off `i` once produced `[, "README" ]`, which is not JSON. */
        if (emitted++) js_raw(&o, ", ");
        js_vstr(&o, inv[i].rel);
    }
    js_raw(&o, " ] }\n");
    if (pkg_injected_fault == PKG_FAIL_DOCTOR) o.fail = 1;
    return (o.fail || u_write_atomicish(rp, buf) != 0) ? -1 : 0;
}

/* ------------------------------------------------------------------ remove */

pkg_status pkg_remove(pkg_ctx *c, const char *id, pkg_stop stop, pkg_err *e)
{
    char regpath[PKG_MAXPATH], dest[PKG_MAXPATH], tid[64];
    char *regtext = NULL, *plan = NULL;
    inv_ent *inv = NULL;
    int ninv = 0, removed = 0, kept = 0;
    char icon_dst[PKG_MAXPATH], icon_state[24] = "", icon_sha[65] = "";
    int icon_managed = 0, icon_kept = 0;
    size_t len;
    pkg_status st = PKG_E_IO;

    memset(&c->removal, 0, sizeof c->removal);
    registry_path(c, id, regpath, sizeof regpath);
    if (!u_exists(regpath))
        return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", "", id);
    /* Not while another installed package depends on it: the other would
       stay installed and stop working. */
    if ((st = rel_check_remove(c, id, e)) != PKG_OK) return st;
    st = PKG_E_IO;

    regtext = u_read_all(regpath, &len);
    if (!regtext) return pkg_fail(e, PKG_E_IO, "cannot read the registry entry", "", regpath);
    inv = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
    if (!inv) { free(regtext); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); }
    if (inv_from_registry(regtext, len, inv, MAXINV, &ninv) != 0) {
        st = pkg_fail(e, PKG_E_IO, "the registry entry is not readable", "", regpath); goto out;
    }
    /* Only an icon the registry says WE installed is ours to consider. An entry
     * written before icons existed has no "icon" member and reads as none. */
    {
        js_tok *t = NULL;
        int nt = js_parse_alloc(regtext, len, &t, PKG_JSON_MAXTOK);
        if (nt > 0) {
            int ic = js_member(regtext, t, nt, 0, "icon");
            if (ic >= 0) {
                js_str(regtext, t, js_member(regtext, t, nt, ic, "state"),  icon_state, sizeof icon_state);
                js_str(regtext, t, js_member(regtext, t, nt, ic, "sha256"), icon_sha,   sizeof icon_sha);
            }
        }
        free(t);
        icon_managed = strcmp(icon_state, "installed") == 0 && strlen(icon_sha) == 64;
        snprintf(icon_dst, sizeof icon_dst, "%s/%s.info", c->root, id);
    }

    snprintf(dest, sizeof dest, "%s/%s", c->root, id);
    txn_id(tid, id);
    {
        char dir[PKG_MAXPATH];
        snprintf(dir, sizeof dir, "%s/%s", c->txns, tid);
        if (u_mkdir(dir) != 0) { st = pkg_fail(e, PKG_E_IO, "cannot create the transaction", "", dir); goto out; }
    }
    {
        /* remove is roll-forward only: a deleted file cannot be restored, so
         * the plan must be complete enough to finish without the registry,
         * which is what the last step destroys. */
        size_t cap = 4096 + (size_t)ninv * 400;
        js_out o; int i;
        plan = (char *)malloc(cap);
        if (!plan) { st = pkg_fail(e, PKG_E_NOMEM, "out of memory", "", id); goto out; }
        js_out_init(&o, plan, cap);
        js_raw(&o, "{\n  "); js_key(&o, "op");      js_vstr(&o, "remove");
        js_raw(&o, ",\n  "); js_key(&o, "package"); js_vstr(&o, id);
        js_raw(&o, ",\n  "); js_key(&o, "dir");     js_vstr(&o, dest);
        js_raw(&o, ",\n  "); js_key(&o, "registry"); js_vstr(&o, regpath);
        js_raw(&o, ",\n  "); js_key(&o, "icon"); js_raw(&o, " { ");
        js_key(&o, "path");    js_vstr(&o, icon_dst);   js_raw(&o, ", ");
        js_key(&o, "staged");  js_vstr(&o, "");         js_raw(&o, ", ");
        js_key(&o, "sha256");  js_vstr(&o, icon_sha);   js_raw(&o, ", ");
        js_key(&o, "managed"); js_vlong(&o, icon_managed);
        js_raw(&o, " }");
        js_raw(&o, ",\n  "); js_key(&o, "contents"); js_raw(&o, " [");
        for (i = 0; i < ninv; i++) {
            js_raw(&o, i ? ",\n    { " : "\n    { ");
            js_key(&o, "path");   js_vstr(&o, inv[i].rel);  js_raw(&o, ", ");
            js_key(&o, "sha256"); js_vstr(&o, inv[i].sha);  js_raw(&o, ", ");
            js_key(&o, "size");   js_vlong(&o, inv[i].size);
            js_raw(&o, " }");
        }
        js_raw(&o, "\n  ]\n}\n");
        if (o.fail || txn_write_plan(c, tid, plan) != 0) {
            st = pkg_fail(e, PKG_E_IO, "cannot write or re-read the plan", "", tid); goto out;
        }
    }
    STOP(PKG_STOP_AFTER_PLAN);

    inv_remove(dest, inv, ninv, &removed, &kept);
    prune_empty(dest);
    {
        char sha[65] = "";
        int present = u_exists(icon_dst);
        if (present && sha256_file(icon_dst, sha) != 0) sha[0] = 0;
        switch (icon_on_remove(icon_managed, present, sha, icon_sha)) {
        case ICON_DELETE:       u_delete(icon_dst); break;
        case ICON_KEEP_CHANGED:
            icon_kept = 1;
            snprintf(c->removal.icon, sizeof c->removal.icon, "%s", icon_dst);
            break;
        default: break;
        }
    }
    /* Said on the console too, not only in the report: a user who kept a
       file and reads "removed" alone would think it gone. */
    if (kept) {
        int i;
        c->removal.kept = kept;
        snprintf(c->removal.list_file, sizeof c->removal.list_file, "%s", c->doctor);
        for (i = 0; i < ninv && c->removal.named < 10; i++) {
            char p[PKG_MAXPATH], sha[65];
            u_join(p, sizeof p, dest, inv[i].rel);
            if (!u_exists(p)) continue;
            if (sha256_file(p, sha) == 0 && strcmp(sha, inv[i].sha) == 0) continue;
            snprintf(c->removal.names[c->removal.named++], sizeof c->removal.names[0], "%s", inv[i].rel);
        }
    }
    if (u_exists(dest)) {
        /* What else is still in the drawer: files that are not the
           package's at all. Named, not classified -- apkg does not know
           what they are, only that it did not put them there. */
        inv_ent *left = (inv_ent *)malloc(sizeof(inv_ent) * MAXINV);
        int nleft = 0, i, j;
        snprintf(c->removal.dir, sizeof c->removal.dir, "%s", dest);
        if (left && inv_of_tree(dest, "", left, &nleft, MAXINV) >= 0) {
            for (i = 0; i < nleft; i++) {
                int ours = 0;
                for (j = 0; j < ninv && !ours; j++) ours = !strcmp(left[i].rel, inv[j].rel);
                if (ours) continue;
                c->removal.others++;
                if (c->removal.others_named < 10)
                    snprintf(c->removal.other_names[c->removal.others_named++],
                             sizeof c->removal.other_names[0], "%s", left[i].rel);
            }
        }
        free(left);
    }
    STOP(PKG_STOP_AFTER_REMOVE_FILES);
    txn_marker(c, tid, "002-contents-done");   /* audit only */

    /* The registry is about to go, so anything deliberately left behind would
     * become unattributable. Record it first. */
    if (kept || icon_kept || u_exists(dest)) {
        /* The registry is about to be destroyed, so anything left behind
         * becomes unattributable if this report does not exist. It is
         * therefore a required step, not a courtesy: a failure here aborts
         * the removal with the registry still in place. */
        if (leftovers_report(c, id, dest, inv, ninv, removed, kept,
                             icon_kept ? icon_dst : "") != 0) {
            st = pkg_fail(e, PKG_E_IO,
                          "cannot record what the removal left behind",
                          "The registry was kept, so nothing has become "
                          "unattributable. Free some space and retry.", c->doctor);
            goto out;
        }
    }

    if (u_delete(regpath) != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot remove the registry entry", "", regpath); goto out;
    }
    txn_marker(c, tid, "004-registry-deleted");   /* audit only */
    STOP(PKG_STOP_BEFORE_COMMIT);
    if (txn_marker(c, tid, "005-commit") != 0) {
        st = pkg_fail(e, PKG_E_IO, "cannot record the commit",
                      "The transaction is kept; the next run will finish it.", tid);
        goto out;
    }
    {
        char dir[PKG_MAXPATH];
        snprintf(dir, sizeof dir, "%s/%s", c->txns, tid);
        u_deltree(dir);
    }
    st = PKG_OK;
out:
    free(regtext); free(plan); free(inv);
    return st;
}

/* --------------------------------------------------------------- progress */

void pkg_set_progress(pkg_ctx *c, pkg_progress cb, void *user)
{
    c->progress = cb; c->progress_user = user;
}

/* Returns 1 if the caller asked to stop AND stopping is allowed here. A call
 * site that passes can_cancel 0 gets 0 back whatever the caller set, so the
 * contract in pkg.h is enforced in one place rather than at each site. */
int prog(pkg_ctx *c, const char *phase, unsigned long done, unsigned long total, int can_cancel)
{
    int cancel = 0;
    pkg_progress_ev ev;
    if (!c->progress) return 0;
    ev.phase = phase; ev.id = c->progress_id ? c->progress_id : "";
    ev.done = done; ev.total = total; ev.can_cancel = can_cancel;
    c->progress(c->progress_user, &ev, &cancel);
    return can_cancel && cancel;
}

static void net_prog_shim(void *user, unsigned long done, unsigned long total, int *cancel)
{
    *cancel = prog((pkg_ctx *)user, "download", done, total, 1);
}

/* ---------------------------------------------------------------- listing */

pkg_status pkg_list(pkg_ctx *c, char **out, pkg_err *e)
{
    char **names; int n, i;
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", "");
    buf[0] = 0;
    if (u_listdir(c->installed, &names, &n) != 0) { *out = buf; return PKG_OK; }
    /* The directory's own order is whatever the filesystem returns --
       measured on AROS One as newest first -- which is neither stable nor
       what anyone looking for a package expects. */
    pkg_sort_names(names, n);
    for (i = 0; i < n; i++) {
        char *dot = strstr(names[i], ".json");
        size_t need;
        if (!dot) continue;
        *dot = 0;
        need = strlen(names[i]) + 2;
        if (len + need >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); if (!buf) break; }
        len += (size_t)sprintf(buf + len, "%s\n", names[i]);
    }
    u_freelist(names, n);
    *out = buf;
    return PKG_OK;
}

pkg_status pkg_info(pkg_ctx *c, const char *id, char **out, pkg_err *e)
{
    char p[PKG_MAXPATH];
    char *txt;
    registry_path(c, id, p, sizeof p);
    txt = u_read_all(p, NULL);
    if (!txt) return pkg_fail(e, PKG_E_NOT_FOUND, "not installed", "", id);
    *out = txt;
    return PKG_OK;
}

/* ------------------------------------------------------------------ doctor */

/* Reports; it does not repair. The one repair available is re-running
 * recovery, which `pkg doctor --retry` does by opening the root in
 * PKG_OPEN_DOCTOR mode -- the same code path every other run uses, not a
 * special one. There is deliberately no "force": the states that reach here
 * are the ones where something on disk could not be accounted for, and a
 * command that deletes them unseen would undo the only guarantee section 2
 * actually makes. */
pkg_status pkg_doctor(pkg_ctx *c, char **out, pkg_err *e)
{
    char **names; int n = 0, i, unresolved = 0;
    size_t cap = 16384, len = 0;
    char *buf = (char *)malloc(cap);

    if (!buf) return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", "");
    buf[0] = 0;

    len += (size_t)sprintf(buf + len, "root      %s\n", c->root);

    if (u_listdir(c->txns, &names, &n) == 0) {
        for (i = 0; i < n; i++) {
            char d[PKG_MAXPATH], planp[PKG_MAXPATH];
            char *plan; size_t plen;
            js_tok *t; int ntok;
            char op[16] = "?", pkgname[PKG_MAXID] = "?", dest[PKG_MAXPATH] = "";
            char staging[PKG_MAXPATH] = "";
            int committed;

            u_join(d, sizeof d, c->txns, names[i]);
            if (!u_isdir(d)) continue;
            unresolved++;
            snprintf(planp, sizeof planp, "%s/000-plan.json", d);
            snprintf(dest, sizeof dest, "%s/005-commit", d);
            committed = u_exists(dest);
            dest[0] = 0;

            len += (size_t)sprintf(buf + len, "\ntransaction %s\n", names[i]);

            plan = u_read_all(planp, &plen);
            if (!plan) {
                len += (size_t)sprintf(buf + len,
                    "  plan      UNREADABLE\n"
                    "  meaning   recovery cannot know what was intended, so it\n"
                    "            stops and keeps everything. This is the one\n"
                    "            state the design cannot resolve by itself.\n"
                    "  do        inspect %s by hand; nothing here was cleaned up\n", d);
                continue;
            }
            ntok = js_parse_alloc(plan, plen, &t, PKG_JSON_MAXTOK);
            if (ntok > 0) {
                js_str(plan, t, js_member(plan, t, ntok, 0, "op"), op, sizeof op);
                js_str(plan, t, js_member(plan, t, ntok, 0, "package"), pkgname, sizeof pkgname);
                js_str(plan, t, js_member(plan, t, ntok, 0, "dir"), dest, sizeof dest);
                js_str(plan, t, js_member(plan, t, ntok, 0, "staging"), staging, sizeof staging);
            }
            len += (size_t)sprintf(buf + len,
                "  operation %s %s\n  commit    %s\n",
                op, pkgname, committed ? "present (will be finished)"
                                       : (strcmp(op, "remove") == 0
                                          ? "absent (remove rolls FORWARD)"
                                          : "absent (rolls back)"));
            if (dest[0])
                len += (size_t)sprintf(buf + len, "  target    %s%s\n", dest,
                                       u_exists(dest) ? " (present)" : " (absent)");
            if (staging[0])
                len += (size_t)sprintf(buf + len, "  staged    %s%s\n", staging,
                    u_exists(staging) ? " (present: the publish never happened,"
                                        " so nothing at the target is ours)"
                                      : " (gone: the publish completed)");
            len += (size_t)sprintf(buf + len,
                "  do        pkg doctor --retry, then read this again\n");
            free(plan); free(t);
        }
        u_freelist(names, n);
    }

    if (!unresolved)
        len += (size_t)sprintf(buf + len, "\nno unresolved transactions\n");

    if (u_listdir(c->doctor, &names, &n) == 0) {
        if (n) len += (size_t)sprintf(buf + len, "\nremovals that left files behind:\n");
        for (i = 0; i < n; i++)
            len += (size_t)sprintf(buf + len, "  db/doctor/%s\n", names[i]);
        u_freelist(names, n);
    }
    *out = buf;
    return PKG_OK;
}

/* ------------------------------------------------------------ update, search */

/* The largest index `update` takes. The public one measured 874 bytes per
   package (2026-10-04), so 5000 packages are about 4.4 MB; this leaves room
   for three times that, and caps what a broken server can make us store. */
#define INDEX_MAX (16L * 1024 * 1024)

/* Refuse a package built for a different ABI.
 *
 * AROS x86_64 has two incompatible ABIs. A binary for one does not start on
 * the other -- it dies with "Illegal address access" and a PC inside a
 * Kickstart module, which reads like a broken program and is not. Measured on
 * eight archive packages and on our own binaries; see docs/reference/abi.md.
 *
 * Installing such a package would work perfectly and produce something that
 * cannot run, so the check belongs before the download, not after.
 *
 * An entry whose abi is empty or unrecognised is refused too. The catalogue's
 * filename convention is the only source for this and it has been misread
 * before; guessing that an unmarked entry is probably fine is how this project
 * spent two days on eight crashes. */
pkg_status target_check(const char *pkg_id, const char *arch, const char *abi,
                        const char *force, pkg_err *e)
{
    const char *mine = pkg_arch();
    char det[320];
    /* --abi is a diagnosis knob for the ABI; it does not make an ARM
       binary run on x86_64, so the CPU is checked all the same. */
    if (*mine && arch && *arch && strcmp(arch, mine)) {
        snprintf(det, sizeof det, "It is built for %s; this system is %s. It would install "
                 "and then not start.", arch, mine);
        return pkg_fail(e, PKG_E_CONFLICT, "wrong CPU for this system", det, pkg_id);
    }
    return abi_check(pkg_id, abi, force, e);
}

pkg_status abi_check(const char *pkg_id, const char *want,
                     const char *force, pkg_err *e)
{
    const char *mine = pkg_abi();
    char det[320];

    if (force && *force) {
        printf("WARNING: --abi %s overrides the check. This binary is %s, and a\n"
               "         package for another ABI installs correctly and then does\n"
               "         not start.\n", force, mine);
        return PKG_OK;
    }
    if (!pkg_abi_known(mine))
        return PKG_OK;              /* build recorded none; do not guess */

    if (!pkg_abi_known(want)) {
        snprintf(det, sizeof det,
                 "The index records abi=\"%s\", which this client does not "
                 "recognise. Refusing rather than guessing.",
                 (want && *want) ? want : "");
        return pkg_fail(e, PKG_E_CONFLICT, "unknown ABI for this package",
                        det, pkg_id);
    }
    if (strcmp(want, mine) != 0) {
        snprintf(det, sizeof det,
                 "It is built for ABI%s; this system is ABI%s. It would install "
                 "correctly and then fail to start. Use the ABI%s build of pkg, "
                 "or a package for ABI%s.", want, mine, want, mine);
        return pkg_fail(e, PKG_E_CONFLICT, "wrong ABI for this system",
                        det, pkg_id);
    }
    return PKG_OK;
}

static pkg_status update_fetch(pkg_ctx *c, const char *index_url, pkg_err *e);

pkg_status pkg_update(pkg_ctx *c, const char *index_url, pkg_err *e)
{
    /* The catalogue being replaced is read first: what is new is what the
       new one has and this one did not. The record is written only after
       the new catalogue is in place, so a failure or a cancel leaves both
       the catalogue and the record of the last refresh as they were. */
    news_prev prev;
    pkg_status st;
    news_before_update(c, &prev);
    st = update_fetch(c, index_url, e);
    if (st == PKG_OK && news_after_update(c, index_url, &prev) != 0 && pkg_verbose())
        printf("  (what is new could not be recorded)\n");
    news_prev_free(&prev);
    return st;
}

static pkg_status update_fetch(pkg_ctx *c, const char *index_url, pkg_err *e)
{
    char dest[PKG_MAXPATH], part[PKG_MAXPATH], why[240];
    if (net_open(why) != 0)
        return pkg_fail(e, PKG_E_NETWORK, "no network", why, index_url);

    snprintf(dest, sizeof dest, "%s/db/index.json", c->root);
    if (pkg_verbose()) printf("  from %s\n", index_url);
    /* Cancellable throughout: the catalogue in use is replaced only after
       the new one has arrived and parsed, so stopping the download changes
       nothing. */
    net_set_progress(c->progress ? net_prog_shim : NULL, c);
    if (net_fetch(index_url, dest, INDEX_MAX, why) != 0) {
        net_set_progress(NULL, NULL);
        net_close();
        if (strcmp(why, "cancelled") == 0)
            return pkg_fail(e, PKG_E_CANCELLED, "catalogue update cancelled",
                            "The catalogue you had is untouched.", "");
        return pkg_fail(e, PKG_E_NETWORK, "the index could not be fetched", why, index_url);
    }
    net_set_progress(NULL, NULL);
    net_close();

    snprintf(part, sizeof part, "%s.part", dest);
    {   /* An index that does not parse must not replace one that does. */
        char *txt; size_t len; js_tok *t; int n;
        txt = u_read_all(part, &len);
        if (!txt) return pkg_fail(e, PKG_E_IO, "cannot read the downloaded index", "", part);
        n = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
        if (n == JS_ELIMIT || n == JS_ENOMEM) {
            free(txt); u_delete(part);
            index_unreadable(e, n, part);
            snprintf(e->detail + strlen(e->detail), sizeof e->detail - strlen(e->detail),
                     " The one you had is untouched.");
            return e->status;
        }
        if (n < 0 || js_member(txt, t, n, 0, "packages") < 0) {
            free(txt); free(t); u_delete(part);
            /* Reached only for a 2xx response whose CONTENT is wrong. A 404
               never gets here; it failed above as an HTTP error. */
            return pkg_fail(e, PKG_E_IO, "the downloaded index is not a valid index",
                            "The server answered, but not with an index. "
                            "The one you had is untouched.", part);
        }
        free(txt); free(t);
    }
    if (u_publish(part, dest) != 0)
        return pkg_fail(e, PKG_E_IO, "cannot install the downloaded index", "", dest);
    if (pkg_verbose()) printf("  saved %s\n", dest);
    return PKG_OK;
}

pkg_status index_unreadable(pkg_err *e, int code, const char *path)
{
    char why[160];
    if (code == JS_ENOMEM)
        return pkg_fail(e, PKG_E_NOMEM, "not enough memory to read the index",
                        "Close other programs and try again.", path);
    if (code == JS_ELIMIT) {
        snprintf(why, sizeof why, "It holds more than %d JSON values, more than this "
                 "apkg reads from one file. A newer apkg may read it.", PKG_JSON_MAXTOK);
        return pkg_fail(e, PKG_E_VERIFY, "the index is too large", why, path);
    }
    return pkg_fail(e, PKG_E_VERIFY, "the index is not readable",
                    "It is not valid JSON, or has no packages array. Run apkg update "
                    "to fetch it again.", path);
}

pkg_status pkg_query(pkg_ctx *c, const char *index_path, const char *term,
                     pkg_entries *out, int *hidden, pkg_err *e)
{
    char *idx; size_t len; int i;

    memset(out, 0, sizeof *out);
    idx = u_read_all(index_path, &len);
    if (!idx) return pkg_fail(e, PKG_E_NOT_FOUND, "no index -- run apkg update first",
                              "", index_path);
    if ((i = entries_from_index_on(idx, len, term, pkg_arch(), pkg_abi(), pkg_abi_show_all(), out, hidden)) != 0) {
        free(idx); pkg_entries_free(out);
        return index_unreadable(e, i, index_path);
    }
    free(idx);
    for (i = 0; i < out->n; i++) {
        char p[PKG_MAXPATH]; char *txt; size_t rl; pkg_entry r;
        registry_path(c, out->v[i].id, p, sizeof p);
        out->v[i].installed = u_exists(p);
        /* The installed revision is what an Upgrade button compares against;
           it is the registry's to say, not the index's. */
        if (out->v[i].installed && (txt = u_read_all(p, &rl)) != NULL) {
            if (entry_from_registry(txt, rl, &r) == 0) {
                snprintf(out->v[i].installed_version, sizeof out->v[i].installed_version, "%s", r.installed_version);
                out->v[i].installed_revision = r.installed_revision;
            }
            free(txt);
        }
    }
    return PKG_OK;
}

pkg_status pkg_installed(pkg_ctx *c, const char *index_path,
                         pkg_entries *out, pkg_err *e)
{
    char **names; int n, i;
    pkg_entries all = {0};

    memset(out, 0, sizeof *out);
    if (u_listdir(c->installed, &names, &n) != 0) return PKG_OK;
    pkg_sort_names(names, n);
    for (i = 0; i < n; i++) {
        char p[PKG_MAXPATH], *txt; size_t len; pkg_entry ent;
        if (!strstr(names[i], ".json")) continue;
        snprintf(p, sizeof p, "%s/%s", c->installed, names[i]);
        txt = u_read_all(p, &len);
        if (!txt) continue;
        if (entry_from_registry(txt, len, &ent) == 0) {
            /* The registry records what was installed, not what the index
               said about it; and this binary installed it, so its ABI is
               this binary's. */
            snprintf(ent.abi, sizeof ent.abi, "%s", pkg_abi());
            ent.ours = 1;
            if (entries_push(out, &ent) != 0) { free(txt); u_freelist(names, n);
                return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", ""); }
        }
        free(txt);
    }
    u_freelist(names, n);

    /* Summary, category and kind are the index's to say. Without an index
       the rows are still correct, only terser. */
    if (index_path) {
        char *idx; size_t len;
        idx = u_read_all(index_path, &len);
        if (idx && entries_from_index(idx, len, NULL, "", 1, &all, NULL) == 0) {
            int j;
            for (i = 0; i < out->n; i++)
                for (j = 0; j < all.n; j++)
                    if (strcmp(out->v[i].id, all.v[j].id) == 0) {
                        memcpy(out->v[i].summary,  all.v[j].summary,  sizeof out->v[i].summary);
                        memcpy(out->v[i].category, all.v[j].category, sizeof out->v[i].category);
                        memcpy(out->v[i].kind,     all.v[j].kind,     sizeof out->v[i].kind);
                        break;
                    }
        }
        free(idx);
        pkg_entries_free(&all);
    }
    return PKG_OK;
}

pkg_status pkg_requirements(pkg_ctx *c, const char *index_path, const char *id,
                            char *out, size_t n, pkg_err *e)
{
    char *idx; size_t len; js_tok *t; int ntok, arr, i, found = -1, nreq = 0;
    req_ent reqs[MAXREQ]; char why[200]; size_t k = 0;
    (void)c;

    out[0] = 0;
    idx = u_read_all(index_path, &len);
    if (!idx) return pkg_fail(e, PKG_E_NOT_FOUND, "no index -- run apkg update first", "", index_path);
    ntok = js_parse_alloc(idx, len, &t, PKG_JSON_MAXTOK);
    if (ntok < 0) { free(idx); return index_unreadable(e, ntok, index_path); }
    arr = js_member(idx, t, ntok, 0, "packages");
    {
        int how;
        found = index_select_variant(idx, t, ntok, arr, id, pkg_arch(), pkg_abi(), &how, NULL);
    }
    if (found < 0) { free(t); free(idx); return pkg_fail(e, PKG_E_NOT_FOUND, "no such package in the index", "", id); }
    if (req_parse(idx, t, ntok, found, reqs, MAXREQ, &nreq, why, sizeof why) != 0) {
        free(t); free(idx);
        return pkg_fail(e, PKG_E_REQUIRES, "this package's system requirements are not readable", why, id);
    }
    free(t); free(idx);
    req_probe_all(reqs, nreq);
    for (i = 0; i < nreq && k + 1 < n; i++)
        k += (size_t)snprintf(out + k, n - k, "%s%s: %s (%s)", i ? "\n" : "",
                              reqs[i].id, req_statename(reqs[i].state), reqs[i].why);
    return PKG_OK;
}

pkg_status pkg_details_get(pkg_ctx *c, const char *index_path, const char *id,
                           pkg_details *d, pkg_err *e)
{
    char *idx, *txt; size_t len; char p[PKG_MAXPATH]; int rc = 1;

    memset(d, 0, sizeof *d);
    idx = u_read_all(index_path, &len);
    if (!idx) d->index_missing = 1;
    else {
        rc = details_from_index(idx, len, id, pkg_arch(), pkg_abi(), d);
        free(idx);
        if (rc < 0) return index_unreadable(e, rc, index_path);
    }
    registry_path(c, id, p, sizeof p);
    if ((txt = u_read_all(p, &len)) != NULL) {
        pkg_entry r;
        if (entry_from_registry(txt, len, &r) == 0) {
            js_tok *t = NULL;
            char dir[PKG_MAXID] = "", rabi[16] = "";
            if (!d->in_index) {
                /* Known to the registry alone: describe what is installed. */
                d->e = r;
                d->e.revision = r.installed_revision;
            }
            d->e.installed = 1;
            snprintf(d->e.installed_version, sizeof d->e.installed_version, "%s", r.installed_version);
            d->e.installed_revision = r.installed_revision;
            snprintf(d->installed_arch, sizeof d->installed_arch, "%s", r.arch);
            {
                int ntok = js_parse_alloc(txt, len, &t, PKG_JSON_MAXTOK);
                if (ntok > 0) {
                    js_str(txt, t, js_member(txt, t, ntok, 0, "dir"), dir, sizeof dir);
                    js_str(txt, t, js_member(txt, t, ntok, 0, "installed"), d->installed_when, sizeof d->installed_when);
                    js_str(txt, t, js_member(txt, t, ntok, 0, "abi"), rabi, sizeof rabi);
                    notes_from_element(txt, t, ntok, 0, d->notes_recorded, sizeof d->notes_recorded);
                }
                free(t);
            }
            snprintf(d->installed_dir, sizeof d->installed_dir, "%s/%s", c->root, dir[0] ? dir : id);
            snprintf(d->installed_abi, sizeof d->installed_abi, "%s", rabi);
        }
        free(txt);
    }
    if (!d->in_index && !d->e.installed)
        return pkg_fail(e, PKG_E_NOT_FOUND,
                        d->index_missing ? "no index, and no such package installed -- run apkg update first"
                                         : "no such package in the index, and none installed",
                        "", id);
    if (!d->in_index) {
        if (d->installed_abi[0])
            d->compat = pkg_compat_of(d->installed_arch, d->installed_abi, pkg_arch(), pkg_abi(),
                                      d->compat_why, sizeof d->compat_why);
        else {
            /* Written before 0.3: the registry has the CPU, not the ABI, and
               the install passed this client's ABI check. */
            d->compat = PKG_COMPAT_NATIVE;
            snprintf(d->compat_why, sizeof d->compat_why, "installed here for %s after the ABI check",
                     d->installed_arch[0] ? d->installed_arch : "this machine");
        }
    }
    /* The requirements are a question only for a package that is compatible,
       and only the index says what they are. */
    snprintf(d->requirements, sizeof d->requirements, "not checked");
    if (d->in_index && d->compat == PKG_COMPAT_NATIVE) {
        if (!d->e.requires[0]) snprintf(d->requirements, sizeof d->requirements, "none");
        else {
            char lines[1024]; pkg_err e2; memset(&e2, 0, sizeof e2);
            if (pkg_requirements(c, index_path, id, lines, sizeof lines, &e2) == PKG_OK) {
                const char *w = strstr(lines, ": missing") ? "missing"
                              : strstr(lines, ": undetermined") ? "undetermined" : "satisfied";
                snprintf(d->requirements, sizeof d->requirements, "%s", w);
            }
        }
    }
    if (d->in_index) details_catalogue(c, d);
    return PKG_OK;
}

/* The Shell's table, formatted from the same rows a front end gets. */
pkg_status pkg_search(pkg_ctx *c, const char *index_path, const char *term,
                      char **out, pkg_err *e)
{
    pkg_entries es;
    int hidden = 0, i;
    size_t cap = 16384, olen = 0;
    char *buf;
    pkg_status st = pkg_query(c, index_path, term, &es, &hidden, e);
    if (st != PKG_OK) return st;

    buf = (char *)malloc(cap);
    if (!buf) { pkg_entries_free(&es); return pkg_fail(e, PKG_E_NOMEM, "out of memory", "", ""); }
    buf[0] = 0;
    for (i = 0; i < es.n; i++) {
        const pkg_entry *p = &es.v[i];
        if (olen + 700 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); if (!buf) break; }
        /* Says why a listed entry is not for this machine: the CPU is
           checked before the ABI, as everywhere else. */
        const char *tag = p->ours ? ""
            : (*pkg_arch() && p->arch[0] && strcmp(p->arch, pkg_arch())) ? "[other CPU] "
            : !pkg_abi_known(p->abi) ? "[unknown ABI] " : "[other ABI] ";
        olen += (size_t)sprintf(buf + olen, "%-20s %-10s %-9s %s%s\n",
                                p->id, p->version, p->arch, tag, p->summary);
    }
    if (!es.n) olen += (size_t)sprintf(buf + olen, "nothing matches\n");
    if (hidden)
        olen += (size_t)sprintf(buf + olen,
            "\n%d package(s) hidden: built for another CPU or ABI, or for one this\n"
            "client cannot judge, so not known to run here. apkg show <id> says\n"
            "why; --all-abi lists them.\n", hidden);
    pkg_entries_free(&es);
    *out = buf;
    return PKG_OK;
}
