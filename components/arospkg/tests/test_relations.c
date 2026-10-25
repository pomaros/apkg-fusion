/* Relations between packages (src/libpkg/relations.c) on the host: the
 * install plan for every case the guide names, the removal guard, the
 * states show describes, and the registry's copy read back. */
#include "../src/libpkg/relations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); bad++; } else printf("ok   %s\n", msg); } while (0)

#define E(id, ver, arch, abi, extra) \
    "{\"id\":\"" id "\",\"version\":\"" ver "\",\"arch\":\"" arch "\",\"abi\":\"" abi "\"," \
    "\"url\":\"https://x/" id "-" arch "-" abi ".zip\",\"size\":1,\"sha256\":\"00\"" extra "}"

static const char *CAT =
    "{\"schema\":1,\"packages\":["
    E("plain",   "1.0",   "x86_64", "v11", "") ","
    E("app",     "1.0",   "x86_64", "v11", ",\"depends\":[\"lib1 >= 1.2\"],\"client_features\":[\"relations\"]") ","
    E("lib1",    "1.3",   "x86_64", "v11", ",\"depends\":[\"lib2\"],\"client_features\":[\"relations\"]") ","
    E("lib2",    "2.0",   "x86_64", "v11", "") ","
    E("old",     "1.0",   "x86_64", "v11", ",\"depends\":[\"lib1 >= 9\"],\"client_features\":[\"relations\"]") ","
    E("cyc1",    "1",     "x86_64", "v11", ",\"depends\":[\"cyc2\"],\"client_features\":[\"relations\"]") ","
    E("cyc2",    "1",     "x86_64", "v11", ",\"depends\":[\"cyc1\"],\"client_features\":[\"relations\"]") ","
    E("nodep",   "1",     "x86_64", "v11", ",\"depends\":[\"ghost\"],\"client_features\":[\"relations\"]") ","
    E("otherabi","1",     "x86_64", "v11", ",\"depends\":[\"v1only\"],\"client_features\":[\"relations\"]") ","
    E("v1only",  "1",     "x86_64", "v1",  "") ","
    E("amb",     "1",     "x86_64", "v11", ",\"depends\":[\"twice\"],\"client_features\":[\"relations\"]") ","
    E("twice",   "1",     "x86_64", "v11", "") ","
    E("twice",   "2",     "x86_64", "v11", "") ","
    E("enemy",   "1",     "x86_64", "v11", ",\"conflicts\":[\"plain\"],\"client_features\":[\"relations\"]") ","
    E("victim",  "1",     "x86_64", "v11", "") ","
    E("bishop",  "0.3.0", "x86_64", "v11", ",\"replaces\":[\"hardcode\"],\"recommends\":[{\"id\":\"limpet\",\"min_version\":\"0.4.1\",\"reason\":\"full screen\"}],\"client_features\":[\"relations\"]") ","
    E("limpet",  "0.5.0", "x86_64", "v11", "") ","
    E("future",  "1",     "x86_64", "v11", ",\"client_features\":[\"teleport\"]") ","
    E("pair",    "1",     "x86_64", "v11", ",\"depends\":[\"lib2\",\"foe\"],\"client_features\":[\"relations\"]") ","
    E("foe",     "1",     "x86_64", "v11", ",\"conflicts\":[\"lib2\"],\"client_features\":[\"relations\"]") ","
    E("newer",   "1",     "x86_64", "v11", ",\"depends\":[\"taker\"],\"client_features\":[\"relations\"]") ","
    E("taker",   "1",     "x86_64", "v11", ",\"replaces\":[\"hardcode\"],\"client_features\":[\"relations\"]") ","
    E("both",    "1",     "x86_64", "v11", ",\"depends\":[\"usesold\"],\"replaces\":[\"hardcode\"],\"client_features\":[\"relations\"]") ","
    E("usesold", "1",     "x86_64", "v11", ",\"depends\":[\"hardcode\"],\"client_features\":[\"relations\"]") ","
    E("pairrep", "1",     "x86_64", "v11", ",\"replaces\":[\"olda\",\"oldb\"],\"client_features\":[\"relations\"]")
    "]}";

static rel_inst I(const char *id, const char *v, const char *abi, const char *dep)
{
    rel_inst r; memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "%s", id); snprintf(r.version, sizeof r.version, "%s", v);
    snprintf(r.arch, sizeof r.arch, "x86_64"); snprintf(r.abi, sizeof r.abi, "%s", abi);
    snprintf(r.reason, sizeof r.reason, "explicit");
    r.rel_known = 1;
    if (dep) { snprintf(r.rel.depends[0].id, PKG_MAXID, "%s", dep); r.rel.ndepends = 1; }
    return r;
}

static rel_plan P;
static int plan(const rel_inst *in, int n, const char *id, int upgrade)
{
    rel_plan_install(CAT, strlen(CAT), in, n, id, "x86_64", "v11", upgrade, &P);
    return P.ok;
}

static void order(const char *want, const char *msg)
{
    char got[300] = ""; int i;
    for (i = 0; i < P.nops; i++) {
        strcat(got, i ? " " : "");
        strcat(got, P.ops[i].kind == RELOP_DEP ? "+" : P.ops[i].kind == RELOP_TARGET ? "=" : "-");
        strcat(got, P.ops[i].id);
    }
    if (strcmp(got, want)) printf("     got [%s] want [%s] why [%s]\n", got, want, P.why);
    CHECK(P.ok && !strcmp(got, want), msg);
}

static void refused(const char *has, pkg_status st, const char *msg)
{
    if (P.ok || !strstr(P.why, has)) printf("     ok=%d why [%s]\n", P.ok, P.why);
    CHECK(!P.ok && strstr(P.why, has) && P.status == st, msg);
}

int main(void)
{
    rel_inst in[4];
    char who[200];

    plan(NULL, 0, "plain", 0);                   order("=plain", "no relations: the package alone");
    plan(NULL, 0, "app", 0);                     order("+lib2 +lib1 =app", "dependencies first, deepest first");
    CHECK(P.extra == 2, "two operations beyond the one asked for");
    in[0] = I("lib2", "2.0", "v11", NULL);
    plan(in, 1, "app", 0);                       order("+lib1 =app", "an installed dependency is not installed again");
    in[0] = I("lib1", "1.0", "v11", NULL);
    plan(in, 1, "app", 0);                       refused("Upgrade it first", PKG_E_REQUIRES, "installed dependency too old");
    in[0] = I("lib1", "1.3", "v1", NULL);
    plan(in, 1, "app", 0);                       refused("another system", PKG_E_REQUIRES, "installed dependency for another ABI");
    plan(NULL, 0, "old", 0);                     refused("the catalogue has 1.3", PKG_E_REQUIRES, "catalogue version too old");
    plan(NULL, 0, "cyc1", 0);                    refused("a cycle", PKG_E_REQUIRES, "cycle");
    plan(NULL, 0, "nodep", 0);                   refused("does not list", PKG_E_NOT_FOUND, "dependency not in the catalogue");
    plan(NULL, 0, "otherabi", 0);                refused("only for other systems", PKG_E_NOT_FOUND, "dependency only for another ABI");
    plan(NULL, 0, "amb", 0);                     refused("more than one build", PKG_E_CONFLICT, "ambiguous dependency");
    plan(NULL, 0, "future", 0);                  refused("newer apkg", PKG_E_REQUIRES, "unknown client feature");

    in[0] = I("plain", "1.0", "v11", NULL);
    plan(in, 1, "enemy", 0);                     refused("enemy says it conflicts with plain", PKG_E_CONFLICT, "conflict declared by the new package");
    in[0] = I("enemy", "1", "v11", NULL);
    in[0].rel_known = 1; snprintf(in[0].rel.conflicts[0].id, PKG_MAXID, "plain"); in[0].rel.nconflicts = 1;
    plan(in, 1, "plain", 0);                     refused("the installed enemy says it conflicts", PKG_E_CONFLICT, "conflict declared by the installed package");
    in[0] = I("enemy", "1", "v11", NULL); in[0].rel_known = 0;     /* written before 0.6: the catalogue's word */
    plan(in, 1, "plain", 0);                     refused("the installed enemy says it conflicts", PKG_E_CONFLICT, "conflict of an installed package from before 0.6, from the catalogue");
    plan(NULL, 0, "pair", 0);                    refused("conflicts with lib2", PKG_E_CONFLICT, "conflict inside the plan");

    in[0] = I("hardcode", "0.3.0-test1", "v11", NULL);
    plan(in, 1, "bishop", 0);                    order("=bishop -hardcode", "replacement: install, then remove the old one");
    CHECK(strstr(P.ops[1].note, "files you changed") != NULL, "the plan says changed files are kept");
    plan(NULL, 0, "bishop", 0);                  order("=bishop", "nothing to replace: a fresh install");
    in[1] = I("addon", "1", "v11", "hardcode");
    plan(in, 2, "bishop", 0);                    refused("still needs it", PKG_E_CONFLICT, "no replacement while something depends on the old one");
    in[0] = I("hardcodex", "1", "v11", NULL);
    plan(in, 1, "bishop", 0);                    order("=bishop", "a similar name is not replaced");

    in[0] = I("hardcode", "0.3.0-test1", "v11", NULL);
    plan(in, 1, "newer", 0);                     refused("by itself first", PKG_E_CONFLICT, "a dependency may not replace: one replacement per plan");
    in[1] = I("hardcode", "x", "v11", NULL);    /* placeholder, overwritten */
    plan(in, 1, "both", 0);                      refused("still needs it", PKG_E_CONFLICT, "no replacement of what a dependency in the plan needs");
    in[0] = I("olda", "1", "v11", NULL); in[1] = I("oldb", "1", "v11", "olda");
    plan(in, 2, "pairrep", 0);                   order("=pairrep -oldb -olda", "two replaced packages that depend on each other: dependent first");

    {   /* rollback */
        rel_decl rd; char rwhy[300];
        memset(&rd, 0, sizeof rd);
        in[0] = I("app", "1.0", "v11", NULL);
        snprintf(in[0].rel.depends[0].id, PKG_MAXID, "lib1"); snprintf(in[0].rel.depends[0].min_version, 64, "1.2"); in[0].rel.ndepends = 1;
        CHECK(rel_check_decl(&rd, "lib1", "1.0", in, 1, "x86_64", "v11", rwhy, sizeof rwhy) != 0 && strstr(rwhy, "would break it"),
              "rollback below what an installed dependent needs is refused");
        CHECK(rel_check_decl(&rd, "lib1", "1.3", in, 1, "x86_64", "v11", rwhy, sizeof rwhy) == 0, "rollback that still meets it is allowed");
        snprintf(rd.depends[0].id, PKG_MAXID, "ghost"); rd.ndepends = 1;
        CHECK(rel_check_decl(&rd, "lib1", "1.3", in, 1, "x86_64", "v11", rwhy, sizeof rwhy) != 0, "rollback to a revision needing a missing package is refused");
    }

    in[0] = I("plain", "1.0", "v11", NULL);
    plan(in, 1, "app", 1);                       refused("Install it first", PKG_E_REQUIRES, "an upgrade installs no dependency");

    in[0] = I("app", "1.0", "v11", "lib1"); in[1] = I("lib1", "1.3", "v11", "lib2"); in[2] = I("lib2", "2", "v11", NULL);
    CHECK(rel_required_by(in, 3, "lib1", NULL, who, sizeof who) == 1 && !strcmp(who, "app"), "lib1 is needed by app");
    CHECK(rel_required_by(in, 3, "app", NULL, who, sizeof who) == 0, "nothing needs app");
    CHECK(rel_required_by(in, 3, "lib2", "lib1", who, sizeof who) == 0, "except the one being removed with it");

    {   /* show */
        rel_decl d; rel_row rows[8]; int n; char why[100];
        memset(&d, 0, sizeof d);
        snprintf(d.recommends[0].id, PKG_MAXID, "limpet"); snprintf(d.recommends[0].min_version, 64, "0.4.1");
        snprintf(d.recommends[0].reason, 160, "full screen"); d.nrecommends = 1;
        snprintf(d.replaces[0].id, PKG_MAXID, "hardcode"); d.nreplaces = 1;
        n = rel_rows(CAT, strlen(CAT), &d, NULL, 0, "x86_64", "v11", rows, 8);
        CHECK(n == 2 && rows[0].kind == REL_RECOMMENDS && !rows[0].installed && rows[0].compatible
              && strstr(rows[0].state, "apkg install limpet"), "recommendation absent: available, how to install");
        in[0] = I("limpet", "0.4.0", "v11", NULL);
        n = rel_rows(CAT, strlen(CAT), &d, in, 1, "x86_64", "v11", rows, 8);
        CHECK(rows[0].installed && !rows[0].satisfied && strstr(rows[0].state, "at least 0.4.1"), "recommendation too old");
        in[0] = I("limpet", "0.5.0", "v11", NULL);
        n = rel_rows(CAT, strlen(CAT), &d, in, 1, "x86_64", "v11", rows, 8);
        CHECK(rows[0].satisfied, "recommendation present");
        CHECK(rows[1].kind == REL_REPLACES && !rows[1].listed && !strcmp(rows[1].state, "not installed"),
              "replaces: the old id need not be in the catalogue");

        {   /* the registry's copy reads back as written */
            char buf[2000], txt[2100]; js_out o; js_tok *t; int nt; rel_decl back;
            snprintf(d.depends[0].id, PKG_MAXID, "lib1"); snprintf(d.depends[0].min_version, 64, "1.2"); d.ndepends = 1;
            snprintf(d.conflicts[0].id, PKG_MAXID, "enemy"); d.nconflicts = 1;
            js_out_init(&o, buf, sizeof buf); rel_emit(&o, &d, 1);
            snprintf(txt, sizeof txt, "{ \"x\": 1%s }", buf);
            nt = js_parse_alloc(txt, strlen(txt), &t, 1000);
            CHECK(nt > 0 && rel_read(txt, t, nt, 0, &back, why, sizeof why) == 0
                  && back.ndepends == 1 && !strcmp(back.depends[0].min_version, "1.2")
                  && back.nconflicts == 1 && back.nreplaces == 1 && back.nrecommends == 1
                  && !strcmp(back.recommends[0].reason, "full screen"), "registry relations round trip");
            if (nt > 0) free(t);
        }
        {
            const char *badd = "{\"depends\":[\"lib1 > 1\"]}";
            js_tok *t; int nt = js_parse_alloc(badd, strlen(badd), &t, 100);
            CHECK(rel_read(badd, t, nt, 0, &d, why, sizeof why) != 0, "a constraint this apkg cannot read refuses");
            free(t);
        }
    }
    printf(bad ? "\n%d FAILED\n" : "\nall passed\n", bad);
    return bad != 0;
}
