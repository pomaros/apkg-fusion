/* The updates model (src/libpkg/updates.c) on the host: every state the
 * list, the window and the widget show, from one catalogue text and one set
 * of registry records. The requirement probe is replaced, as in test_req. */
#include "../src/libpkg/updates.h"
#include "../src/libpkg/req.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); bad++; } else printf("ok   %s\n", msg); } while (0)

#define E(id, ver, rev, arch, abi, extra) \
    "{\"id\":\"" id "\",\"version\":\"" ver "\",\"revision\":" #rev ",\"arch\":\"" arch \
    "\",\"abi\":\"" abi "\",\"url\":\"https://x/" id ".zip\",\"size\":1000," \
    "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"" extra "}"

static const char *CAT =
    "{\"schema\":2,\"packages\":["
    E("newver",  "1.10",  1, "x86_64", "v11", "") ","
    E("newrev",  "2.0",   3, "x86_64", "v11", "") ","
    E("same",    "1.0",   1, "x86_64", "v11", "") ","
    E("older",   "1.2",   1, "x86_64", "v11", "") ","
    E("oddver",  "1.3b",  1, "x86_64", "v11", "") ","
    E("spelled", "1.0.0", 1, "x86_64", "v11", "") ","
    E("twice",   "2.0",   1, "x86_64", "v11", "") ","
    E("twice",   "2.1",   1, "x86_64", "v11", "") ","
    E("otherabi","3.0",   1, "x86_64", "v1",  "") ","
    E("otherabi","3.0",   1, "i386",   "v0",  "") ","
    E("needs",   "5.0",   1, "x86_64", "v11", ",\"requires_system\":[{\"type\":\"library\",\"id\":\"missing.library\"}]") ","
    E("unsure",  "5.0",   1, "x86_64", "v11", ",\"requires_system\":[{\"type\":\"library\",\"id\":\"unsure.library\"}]") ","
    "{\"id\":\"nohash\",\"version\":\"9\",\"revision\":1,\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"https://x/n.zip\",\"size\":1}"
    "]}";

static void probe(req_ent *r)
{
    if (strstr(r->id, "missing")) { r->state = REQ_MISSING; snprintf(r->why, sizeof r->why, "not here"); }
    else if (strstr(r->id, "unsure")) { r->state = REQ_UNDETERMINED; snprintf(r->why, sizeof r->why, "cannot tell"); }
    else r->state = REQ_SATISFIED;
}

static upd_installed I(const char *id, const char *v, long rev, const char *abi)
{
    upd_installed r; memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "%s", id); snprintf(r.version, sizeof r.version, "%s", v);
    r.revision = rev; snprintf(r.abi, sizeof r.abi, "%s", abi);
    if (*abi) snprintf(r.arch, sizeof r.arch, "x86_64");
    return r;
}

static const pkg_update_row *row(const pkg_updates *u, const char *id) { return pkg_updates_find(u, id); }

static void expect(const pkg_updates *u, const char *id, pkg_upd_state s, const char *msg)
{
    const pkg_update_row *r = row(u, id);
    char m[200];
    snprintf(m, sizeof m, "%s: %s (got %s%s%s)", id, msg, r ? pkg_upd_word(r->state) : "no row",
             r && r->why[0] ? ", " : "", r ? r->why : "");
    CHECK(r && r->state == s, m);
}

int main(int argc, char **argv)
{
    upd_installed in[14];
    int n = 0;
    pkg_updates u;
    char *js;

    req_set_probe(probe);
    in[n++] = I("newver",  "1.9",  5, "v11");
    in[n++] = I("newrev",  "2.0",  2, "v11");
    in[n++] = I("same",    "1.0",  1, "");      /* pre-0.3 entry: no target recorded */
    in[n++] = I("older",   "1.3",  1, "v11");
    in[n++] = I("oddver",  "1.3a", 1, "v11");
    in[n++] = I("spelled", "1.0",  1, "v11");
    in[n++] = I("twice",   "1.0",  1, "v11");
    in[n++] = I("otherabi","2.0",  1, "v11");
    in[n++] = I("gone",    "1.0",  1, "v11");
    in[n++] = I("needs",   "4.0",  1, "v11");
    in[n++] = I("unsure",  "4.0",  1, "v11");
    in[n++] = I("nohash",  "8",    1, "v11");

    memset(&u, 0, sizeof u);
    CHECK(updates_compute(CAT, strlen(CAT), in, n, "x86_64", "v11", NULL, 1, &u) == 0, "computes");
    CHECK(u.catalogue == PKG_CAT_OK, "catalogue ok");
    expect(&u, "newver", PKG_UPD_AVAILABLE, "1.9 -> 1.10 is newer (numbers, not text)");
    expect(&u, "newrev", PKG_UPD_AVAILABLE, "same version, revision 2 -> 3");
    expect(&u, "same", PKG_UPD_CURRENT, "same version and revision; no target recorded");
    expect(&u, "older", PKG_UPD_NEWER_INSTALLED, "catalogue behind: no downgrade");
    expect(&u, "oddver", PKG_UPD_INCOMPARABLE, "1.3a vs 1.3b not guessed");
    expect(&u, "spelled", PKG_UPD_INCOMPARABLE, "1.0 vs 1.0.0 refused like apkg upgrade");
    expect(&u, "twice", PKG_UPD_AMBIGUOUS, "two entries for x86_64/v11");
    expect(&u, "otherabi", PKG_UPD_OTHER_TARGET, "only v1 and v0 builds listed");
    expect(&u, "gone", PKG_UPD_NOT_IN_INDEX, "id absent");
    expect(&u, "needs", PKG_UPD_BLOCKED, "requirement missing blocks");
    expect(&u, "unsure", PKG_UPD_AVAILABLE, "undetermined requirement does not block");
    expect(&u, "nohash", PKG_UPD_BLOCKED, "no hash blocks");
    CHECK(row(&u, "unsure") && !strcmp(row(&u, "unsure")->requirements, "undetermined"), "undetermined is reported");
    CHECK(row(&u, "newver") && !strcmp(row(&u, "newver")->available_version, "1.10")
          && row(&u, "newver")->available_revision == 1, "target version recorded");
    CHECK(row(&u, "same") && !strcmp(row(&u, "same")->abi, "v11"), "missing target taken as this client's");
    CHECK(u.newer == 5 && u.ready == 3 && u.blocked == 2, "counts: 5 newer, 3 ready, 2 blocked");
    CHECK(row(&u, "otherabi") && strstr(row(&u, "otherabi")->why, "x86_64/v1"), "other-target names the builds");

    js = pkg_updates_json(&u);
    /* With a path, the full answer is written there for a real JSON parser
       (run-host-tests.sh feeds it to Python's). */
    if (argc > 1 && js) { FILE *f = fopen(argv[1], "w"); if (f) { fputs(js, f); fclose(f); } }
    CHECK(js && strstr(js, "\"state\": \"ok\"") && strstr(js, "\"ready\": 3")
          && strstr(js, "\"id\": \"newver\"") && !strchr(js, '\033'), "JSON: counts, rows, no escapes");
    free(js);
    pkg_updates_free(&u);

    /* Without probing, requirements cannot block; they are reported unchecked. */
    memset(&u, 0, sizeof u);
    updates_compute(CAT, strlen(CAT), in, n, "x86_64", "v11", NULL, 0, &u);
    expect(&u, "needs", PKG_UPD_AVAILABLE, "unprobed requirement does not block");
    CHECK(row(&u, "needs") && !strcmp(row(&u, "needs")->requirements, "not checked"), "unprobed is 'not checked'");
    pkg_updates_free(&u);

    /* The same installed set on mainline: every v11 record keeps its target. */
    memset(&u, 0, sizeof u);
    updates_compute(CAT, strlen(CAT), in, n, "x86_64", "v1", NULL, 0, &u);
    /* The v11 entry is still the one chosen (the installed target), but it
       cannot run on this client, as apkg upgrade would refuse it there. */
    expect(&u, "newver", PKG_UPD_BLOCKED, "installed target chosen, blocked on a v1 client");
    expect(&u, "same", PKG_UPD_OTHER_TARGET, "unrecorded target is the client's: v1 not listed");
    pkg_updates_free(&u);

    /* No catalogue, and one that is not an index: rows still come, counted as nothing. */
    memset(&u, 0, sizeof u);
    updates_compute(NULL, 0, in, n, "x86_64", "v11", NULL, 1, &u);
    CHECK(u.catalogue == PKG_CAT_MISSING && u.n == n && u.newer == 0, "missing catalogue: rows, no count");
    expect(&u, "newver", PKG_UPD_NO_CATALOGUE, "no catalogue state");
    js = pkg_updates_json(&u);
    CHECK(js && strstr(js, "\"state\": \"missing\""), "JSON says missing");
    free(js);
    pkg_updates_free(&u);

    memset(&u, 0, sizeof u);
    updates_compute("{\"packages\": [", 14, in, n, "x86_64", "v11", NULL, 1, &u);
    CHECK(u.catalogue == PKG_CAT_UNREADABLE && u.catalogue_error[0], "truncated catalogue: unreadable");
    pkg_updates_free(&u);
    memset(&u, 0, sizeof u);
    updates_compute("{\"x\":1}", 7, in, n, "x86_64", "v11", NULL, 1, &u);
    CHECK(u.catalogue == PKG_CAT_UNREADABLE, "object without packages: unreadable");
    pkg_updates_free(&u);

    /* Nothing installed. */
    memset(&u, 0, sizeof u);
    updates_compute(CAT, strlen(CAT), in, 0, "x86_64", "v11", NULL, 1, &u);
    CHECK(u.n == 0 && u.newer == 0 && u.catalogue == PKG_CAT_OK, "nothing installed");
    js = pkg_updates_json(&u);
    CHECK(js && strstr(js, "\"packages\": []"), "JSON with no rows");
    free(js);
    pkg_updates_free(&u);

    printf(bad ? "\nFAILED: %d\n" : "\nall update-model checks pass\n", bad);
    return bad != 0;
}
