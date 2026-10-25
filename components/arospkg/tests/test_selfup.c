/* The decisions of arospkg's self-update (src/libpkg/selfup.c) on the host:
 * which installation a program belongs to, what is replaced, the record of
 * copies, the journal and how an interrupted update is settled. The AROS
 * side (selfup_io.c) is checked on a guest. */
#include "../src/libpkg/selfup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); bad++; } else printf("ok   %s\n", m); } while (0)
#define SA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SC "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

static void versions(void)
{
    char v[40];
    static const char pm[] = "xx\0$VER: PkgManager 0.7.0 (24.10.2026)\0yy";
    static const char ap[] = "zz\0$VER: apkg 0.7.0-sutest2 (24.10.2026)\0";
    static const char old[] = "\0apkg %s\0apkg 0.6.0\0apkg self-update\0";
    static const char none[] = "\0apkg x\0apkg 0.6.0 is\0";
    CHECK(su_version_in(pm, sizeof pm, "PkgManager", v, sizeof v) == 0 && !strcmp(v, "0.7.0"), "$VER of PkgManager");
    CHECK(su_version_in(ap, sizeof ap, "apkg", v, sizeof v) == 0 && !strcmp(v, "0.7.0-sutest2"), "$VER of a labelled test apkg");
    CHECK(su_version_in(old, sizeof old, "apkg", v, sizeof v) == 0 && !strcmp(v, "0.6.0"),
          "an apkg before 0.7: its \"apkg 0.6.0\" string, not \"apkg %s\"");
    CHECK(su_version_in(none, sizeof none, "apkg", v, sizeof v) != 0 && !v[0], "no version string: unknown, not guessed");
    CHECK(su_version_in(old, sizeof old, "PkgManager", v, sizeof v) != 0, "the fallback is apkg's only");
}

static su_facts facts(void)
{
    su_facts f;
    memset(&f, 0, sizeof f);
    f.started_is_apkg = 1;
    strcpy(f.started, "System:C/apkg");
    strcpy(f.dir, "System:C");
    return f;
}

static void where(void)
{
    su_facts f;
    su_where w;

    f = facts(); strcpy(f.started, "Work:Packages/arospkg/apkg"); strcpy(f.dir, "Work:Packages/arospkg");
    f.dir_managed = f.dir_is_home = 1; strcpy(f.dir_root, "Work:Packages");
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_HOME && w.home == SU_HOME_MANAGED && !strcmp(w.root, "Work:Packages"), "managed home: its root");

    f = facts(); strcpy(f.dir, "Work:My Apps/arospkg-0.6.0"); f.dir_is_home = 1;
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_HOME && w.home == SU_HOME_DRAWER && !w.root[0], "drawer unpacked by hand");

    f = facts(); f.recorded = 1; strcpy(f.recorded_drawer, "Work:arospkg"); f.recorded_ok = 1;
    strcpy(f.cand[0], "SYS:Packages/arospkg"); f.ncand = 1;
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_COPY && !strcmp(w.drawer, "Work:arospkg") && !w.record,
          "a recorded copy goes with its drawer, whatever else is around");

    f = facts(); f.recorded = 1; f.recorded_drawer[0] = 0; strcpy(f.cand[0], "SYS:Packages/arospkg"); f.ncand = 1;
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_ALONE, "recorded as on its own: stays on its own");

    f = facts(); f.recorded = 1; strcpy(f.recorded_drawer, "Work:gone");
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_BROKEN && strstr(w.why, "Work:gone"), "a record naming a drawer that is no longer one: refused, said");

    f = facts(); f.chose = 1; strcpy(f.chosen, "Work:arospkg"); f.chosen_ok = 1; f.recorded = 1; f.recorded_drawer[0] = 0;
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_COPY && w.record && !strcmp(w.drawer, "Work:arospkg"), "--drawer overrides the record and is recorded");

    f = facts(); f.chose = 1; strcpy(f.chosen, "Work:nothing");
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_BROKEN && !w.record, "--drawer naming no arospkg drawer: refused, nothing recorded");

    f = facts(); f.chose = 1; f.chosen[0] = 0;
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_ALONE && w.record, "--apkg-only: on its own, recorded");

    f = facts();
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_ALONE && !w.record && strstr(w.why, "--drawer"), "no record, no candidate: apkg alone, with a hint");

    f = facts(); strcpy(f.cand[0], "SYS:Packages/arospkg"); strcpy(f.cand_how[0], "the package arospkg in SYS:Packages");
    strcpy(f.cand[1], "Work:arospkg"); strcpy(f.cand_how[1], "PkgManager is running from it"); f.ncand = 2;
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_CHOOSE && strstr(w.why, "SYS:Packages/arospkg") && strstr(w.why, "Work:arospkg"),
          "two candidates and no record: asked, never guessed");

    f = facts(); strcpy(f.cand[0], "SYS:Packages/arospkg"); f.ncand = 1;
    su_decide_where(&f, &w);
    CHECK(w.kind == SU_WHERE_CHOOSE, "even one candidate is asked about");
}

static su_install inst(void)
{
    su_install in;
    memset(&in, 0, sizeof in);
    in.home = SU_HOME_DRAWER;
    strcpy(in.drawer, "Work:arospkg");
    in.n = 4;
    in.f[0].role = SU_APKG;       strcpy(in.f[0].path, "Work:arospkg/apkg");       strcpy(in.f[0].version, "0.6.0"); strcpy(in.f[0].sha, SA);
    in.f[1].role = SU_PKGMANAGER; strcpy(in.f[1].path, "Work:arospkg/PkgManager"); strcpy(in.f[1].version, "0.6.0"); strcpy(in.f[1].sha, SA);
    in.f[2].role = SU_BUILD;      strcpy(in.f[2].path, "Work:arospkg/BUILD.txt");  strcpy(in.f[2].sha, SA);
    in.f[3].role = SU_APKG; in.f[3].copy = 1; strcpy(in.f[3].path, "System:C/apkg"); strcpy(in.f[3].version, "0.6.0"); strcpy(in.f[3].sha, SA);
    return in;
}

static void plan(void)
{
    su_install in = inst();
    char why[300];
    int i;
    for (i = 0; i < in.n; i++) strcpy(in.f[i].want_sha, SB);
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == 4 && in.f[3].act == SU_REPLACE, "every file of the installation, copy included");

    in = inst();
    strcpy(in.f[0].want_sha, SA); strcpy(in.f[0].version, "0.7.0");
    strcpy(in.f[1].want_sha, SB); strcpy(in.f[2].want_sha, SB); strcpy(in.f[3].want_sha, SA);
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == 2 && in.f[0].act == SU_KEEP && in.f[1].act == SU_REPLACE,
          "an apkg already updated by an older self-update stays; PkgManager follows it");

    in = inst();
    for (i = 0; i < in.n; i++) strcpy(in.f[i].want_sha, SB);
    strcpy(in.f[1].version, "0.8.0-test1");
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == -1 && strstr(why, "newer"), "never a downgrade of a newer build");

    in = inst();
    for (i = 0; i < in.n; i++) strcpy(in.f[i].want_sha, SB);
    in.f[1].sha[0] = 0;
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == -1 && strstr(why, "cannot be read"),
          "a file whose checksum cannot be read is never replaced: it could not be put back");

    in = inst();
    in.f[1].want_sha[0] = 0; strcpy(in.f[0].want_sha, SB);
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == 1 && in.f[1].act == SU_KEEP, "a file the release does not have stays");

    in = inst(); in.home = SU_HOME_MANAGED; strcpy(in.root, "SYS:Packages"); strcpy(in.managed_version, "0.7.0");
    for (i = 0; i < in.n; i++) strcpy(in.f[i].want_sha, SB);
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == 1 && in.f[3].act == SU_REPLACE && in.f[1].act == SU_KEEP
          && !su_managed_behind(&in, "0.7.0"),
          "a managed home at the release: its changed files are verify's business; a stale copy is updated");
    strcpy(in.managed_version, "0.6.0");
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == 4 && su_managed_behind(&in, "0.7.0"), "a managed home behind: replaced");
    strcpy(in.managed_version, "0.9.0");
    CHECK(su_plan(&in, "0.7.0", why, sizeof why) == -1, "a managed home ahead of the release: refused");

    in = inst();
    for (i = 0; i < in.n; i++) strcpy(in.f[i].version, in.f[i].role == SU_BUILD ? "" : "0.7.0");
    CHECK(su_all_current(&in, "0.7.0"), "every program at the release: nothing to download");
    strcpy(in.f[1].version, "0.6.0");
    CHECK(!su_all_current(&in, "0.7.0"), "PkgManager behind: download");
    strcpy(in.f[1].version, "0.7.0"); in.home = SU_HOME_MANAGED; strcpy(in.managed_version, "0.6.0");
    CHECK(!su_all_current(&in, "0.7.0"), "a managed record behind: download, so the record follows");
    in.f[0].version[0] = 0; in.home = SU_HOME_DRAWER;
    CHECK(!su_all_current(&in, "0.7.0"), "an unreadable version: download and compare the files");

    in = inst(); strcpy(in.f[0].want_sha, SB);
    CHECK(!strcmp(su_state_word(&in.f[0], "0.7.0"), "older") && !strcmp(su_state_word(&in.f[0], "0.6.0"), "current")
          && !strcmp(su_state_word(&in.f[0], "0.5.0"), "newer"), "--check words");
    in.f[0].version[0] = 0;
    CHECK(!strcmp(su_state_word(&in.f[0], "0.7.0"), "unknown"), "--check: an unreadable version is unknown");
}

static void record(void)
{
    char d[256], *a, *b, *c;
    a = su_copies_set(NULL, "System:C/apkg", "Work:arospkg");
    CHECK(a && su_copies_lookup(a, "system:c/APKG", d, sizeof d) == 1 && !strcmp(d, "Work:arospkg"),
          "recorded, looked up as DOS compares names (case does not matter)");
    b = su_copies_set(a, "Work:Tools/apkg", "");
    CHECK(b && su_copies_lookup(b, "Work:Tools/apkg", d, sizeof d) == 1 && !d[0]
          && su_copies_lookup(b, "System:C/apkg", d, sizeof d) == 1, "a second copy, on its own; the first kept");
    c = su_copies_set(b, "System:C/apkg", "Work:My \"odd\" drawer");
    CHECK(c && su_copies_lookup(c, "System:C/apkg", d, sizeof d) == 1 && !strcmp(d, "Work:My \"odd\" drawer"),
          "changed in place; quotes survive");
    CHECK(su_copies_lookup(c, "Work:other", d, sizeof d) == 0, "not recorded");
    CHECK(su_copies_lookup("not json", "x", d, sizeof d) == 0, "a damaged record reads as empty");
    free(a); free(b); free(c);
}

static su_journal jr(void)
{
    su_journal j;
    memset(&j, 0, sizeof j);
    strcpy(j.from_version, "0.6.0"); strcpy(j.to_version, "0.7.0");
    j.n = 2;
    strcpy(j.f[0].path, "System:C/apkg");          strcpy(j.f[0].from_sha, SA); strcpy(j.f[0].to_sha, SB);
    strcpy(j.f[1].path, "Work:arospkg/PkgManager"); strcpy(j.f[1].from_sha, SC); strcpy(j.f[1].to_sha, SB);
    return j;
}

static void journal(void)
{
    su_journal j = jr(), k;
    char *t = su_journal_text(&j), steps[600];
    CHECK(t && su_journal_parse(t, &k) == 0 && k.n == 2 && !strcmp(k.f[1].path, j.f[1].path)
          && !strcmp(k.f[0].to_sha, SB) && !strcmp(k.to_version, "0.7.0"), "journal round trip");
    free(t);
    CHECK(su_journal_parse("{\"format\":2}", &k) != 0 && su_journal_parse("junk", &k) != 0, "an unknown journal is not read");

    /* No managed home: forward when every new file is there. */
    j = jr(); j.f[0].now = SU_J_TO; j.f[1].now = SU_J_FROM; j.f[1].have_new = 1;
    CHECK(su_recover_decide(&j, -1, steps, sizeof steps) == SU_REC_FORWARD, "stopped between apkg and PkgManager: finished");
    j = jr(); j.f[0].now = SU_J_TO; j.f[0].have_old = 1; j.f[1].now = SU_J_ABSENT; j.f[1].have_old = 1;
    CHECK(su_recover_decide(&j, -1, steps, sizeof steps) == SU_REC_BACKWARD,
          "PkgManager's new file lost, its old one there: back to the previous set");
    j = jr(); j.f[0].now = SU_J_TO; j.f[1].now = SU_J_TO;
    CHECK(su_recover_decide(&j, -1, steps, sizeof steps) == SU_REC_DONE, "all new: nothing to do");
    j = jr(); j.f[0].now = SU_J_FROM; j.f[1].now = SU_J_FROM;
    CHECK(su_recover_decide(&j, -1, steps, sizeof steps) == SU_REC_DONE, "nothing renamed yet: nothing to do");
    j = jr(); j.f[0].now = SU_J_TO; j.f[1].now = SU_J_ABSENT;
    CHECK(su_recover_decide(&j, -1, steps, sizeof steps) == SU_REC_STUCK && strstr(steps, "PkgManager") && strstr(steps, "archive"),
          "neither way possible: stuck, with what to do by hand");
    j = jr(); j.f[0].now = SU_J_OTHER; j.f[1].now = SU_J_TO;
    CHECK(su_recover_decide(&j, -1, steps, sizeof steps) == SU_REC_STUCK && strstr(steps, "neither"),
          "a file that is neither version is never overwritten");
    j = jr(); j.f[0].now = SU_J_ABSENT; j.f[0].have_old = 1; j.f[1].now = SU_J_FROM;
    CHECK(su_recover_decide(&j, -1, steps, sizeof steps) == SU_REC_BACKWARD, "stopped between the two renames of one file");

    /* A managed home decides. */
    j = jr(); j.n = 1; j.f[0].now = SU_J_TO;
    CHECK(su_recover_decide(&j, 0, steps, sizeof steps) == SU_REC_STUCK, "package rolled back, the copy new with no old: stuck");
    j.f[0].have_old = 1;
    CHECK(su_recover_decide(&j, 0, steps, sizeof steps) == SU_REC_BACKWARD, "package rolled back: the copy goes back too");
    CHECK(su_recover_decide(&j, 1, steps, sizeof steps) == SU_REC_DONE, "package committed and the copy new: done");
    j.f[0].now = SU_J_FROM; j.f[0].have_new = 1;
    CHECK(su_recover_decide(&j, 1, steps, sizeof steps) == SU_REC_FORWARD, "package committed: the copy follows");
}

static void targets(void)
{
    const char *sums = SA "  arospkg-0.7.0.aarch64-aros-raspi.zip\n" SB "  arospkg-0.7.0.x86_64-aros-v11.zip\n";
    pkg_release r;
    CHECK(pkg_release_pick(sums, "aarch64-aros", &r) == 0, "a hosted aarch64 build never takes the Pi's archive");
    CHECK(pkg_release_pick(sums, "aarch64-aros-raspi", &r) == 1 && !strcmp(r.sha256, SA), "the Pi takes its own");
    CHECK(pkg_release_pick(sums, "x86_64-aros-v1", &r) == 0 && pkg_release_pick(sums, "x86_64-aros-v11", &r) == 1,
          "mainline and ABIv11 are different targets");
}

static void escapes(void)
{
    const char *js = "{\"a\":\"say \\\"hi\\\" C:\\\\x\\nend\"}";
    js_tok t[8]; char b[64], small[8];
    int n = js_parse(js, strlen(js), t, 8);
    CHECK(su_jstr(js, t, js_member(js, t, n, 0, "a"), small, sizeof small) != 0 && !small[0],
          "a value longer than the buffer is refused, not cut into another path");
    CHECK(n > 0 && su_jstr(js, t, js_member(js, t, n, 0, "a"), b, sizeof b) == 0 && !strcmp(b, "say \"hi\" C:\\x\nend"),
          "escapes undone");
}

int main(void)
{
    versions(); where(); plan(); record(); journal(); targets(); escapes();
    printf(bad ? "\nFAILED %d\n" : "\nall self-update checks pass\n", bad);
    return bad != 0;
}
