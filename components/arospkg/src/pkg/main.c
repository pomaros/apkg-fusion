/* apkg -- the command line front end. Presentation only: every decision is in
 * libpkg, so that PkgManager can make the same ones without reimplementing
 * them. Errors are printed as libpkg wrote them rather than re-phrased here. */

#include "../libpkg/pkg.h"
#include "../libpkg/tls.h"
#include "../libpkg/sha256.h"
#include "../libpkg/verify.h"
#include "../libpkg/entries.h"
#include "../libpkg/json.h"
#include "present.h"
#include "hint.h"
#include "selfupdate.h"
#include "../libpkg/selfup.h"
#include "../about.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>

/* APKG_TEST_VERSION lets a self-update test build call itself older than the
   release it updates to; a release never sets it. */
#ifdef APKG_TEST_VERSION
static const char *VERSION = "apkg " APKG_TEST_VERSION;
static const char VERSTAG[] __attribute__((used)) = "$VER: apkg " APKG_TEST_VERSION " (24.10.2026)";
#else
static const char *VERSION = "apkg 0.7.2";
/* What a self-update reads to tell this file's version without running it. */
static const char VERSTAG[] __attribute__((used)) = "$VER: apkg 0.7.2 (30.10.2026)";
#endif

/* `show`: the library's account of one package, laid out for a person. The
 * facts come from pkg_details_get and the probes behind the window's panel;
 * nothing is decided here. */
/* A verdict's colour; the word itself is printed in every mode. */
static void verdict(const char *w)
{
    int c = -1;
    if (!strcmp(w, "native") || !strcmp(w, "satisfied")) c = PR_GREEN;
    else if (!strcmp(w, "undetermined")) c = PR_YELLOW;
    else if (!strcmp(w, "incompatible") || !strcmp(w, "missing")) c = PR_RED;
    if (c < 0) fputs(w, stdout); else pr_word(c, w);
}

static void print_notes(const pkg_details *d, int after_install);
static void print_relations(pkg_ctx *c, const char *index, const char *id, int rich);

static const char *hint_root;
/* Names per kind in the summary after apkg update. */
#define NEWS_SUMMARY_NAMES 5

static void show_rich(pkg_ctx *c, const char *index, const char *id, const pkg_details *d)
{
    char version[80], text[1200], why[240];
    rev_label(version, sizeof version, d->e.version, d->e.revision);
    snprintf(text, sizeof text, "%s %s", d->e.id, version);
    pr_heading(text);
    if (*d->e.summary) pr_text(d->e.summary, 2);
    putchar('\n');
    if (d->e.installed) {
        rev_label(version, sizeof version, d->e.installed_version, d->e.installed_revision);
        snprintf(text, sizeof text, "installed (%s)", version);
        pr_field("State", text, PR_GREEN);
        pr_field("Location", d->installed_dir, -1);
        open_hint(text, sizeof text, hint_root, d->e.id);
        pr_field("Open folder", text, -1);
        if (*d->installed_when) pr_field("Installed on", d->installed_when, -1);
        if (*d->installed_arch) {
            snprintf(text, sizeof text, "%s / %s", d->installed_arch,
                     *d->installed_abi ? d->installed_abi : "ABI not recorded");
            pr_field("Installed target", text, -1);
        }
    } else pr_field("State", "not installed", -1);
    if (d->index_missing) pr_field("Catalogue", "missing -- run apkg update", PR_YELLOW);
    else if (!d->in_index) pr_field("Catalogue", "no longer lists this package", PR_YELLOW);
    if (d->ambiguous) pr_field("Selection", "multiple matching builds; installation refused", PR_RED);
    if (*d->e.category) pr_field("Category", d->e.category, -1);
    /* Before the technical sections: "needs the original game data" is
       what decides whether to install at all. */
    print_notes(d, 0);

    putchar('\n'); pr_heading("Compatibility and requirements");
    if (d->in_index) {
        snprintf(text, sizeof text, "%s / %s", d->e.arch, d->e.abi);
        pr_field("Target", text, -1);
        if (d->nvariants > 1) pr_field("Other builds", d->variants, -1);
    }
    snprintf(text, sizeof text, "%s -- %s", pkg_compat_word(d->compat), d->compat_why);
    pr_field("Compatibility", text, d->compat == PKG_COMPAT_NATIVE ? PR_GREEN :
             d->compat == PKG_COMPAT_INCOMPATIBLE ? PR_RED : PR_YELLOW);
    if (d->in_index) {
        if (d->compat != PKG_COMPAT_NATIVE)
            pr_field("Requirements", "not checked for this target", PR_YELLOW);
        else if (!*d->e.requires) pr_field("Requirements", "none stated", -1);
        else {
            pkg_err err; memset(&err, 0, sizeof err);
            pr_field("Requirements", d->requirements, !strcmp(d->requirements, "satisfied") ? PR_GREEN :
                     !strcmp(d->requirements, "missing") ? PR_RED : PR_YELLOW);
            if (pkg_requirements(c, index, id, text, sizeof text, &err) == PKG_OK)
                pr_text(text, 4);
            else { pr_text(d->e.requires, 4); pr_text(err.summary, 4); }
        }
    }
    if (d->e.installed) {
        putchar('\n'); pr_heading("Installed package actions");
        if (d->in_index) {
            int yes = pkg_can_upgrade(&d->e, why, sizeof why);
            pr_field("Upgrade", yes ? "available" : why, yes ? PR_GREEN : -1);
        }
        {
            int yes = pkg_can_rollback(c, id, why, sizeof why);
            pr_field("Roll back", yes ? "available" : why, yes ? PR_GREEN : -1);
        }
    }
    putchar('\n'); pr_heading("Download and provenance");
    if (d->in_index) {
        if (d->e.size >= 0) {
            snprintf(text, sizeof text, "%ld KiB (%ld bytes)", d->e.size / 1024, d->e.size);
            pr_field("Download", text, -1);
        }
        pr_field("URL", d->url, -1);
        if (*d->sha256) pr_field("SHA-256", d->sha256, -1);
    }
    if (*d->source) pr_field("Source", d->source, -1);
    if (*d->license) pr_field("License", d->license, -1);
    if (d->in_index) {
        int i;
        putchar('\n'); pr_heading("Review and tests");
        pkg_review_text(d, text, sizeof text);
        pr_field("Status", text, !strcmp(d->review, "reviewed") ? PR_GREEN : -1);
        if (!d->ntests) pr_field("Runtime tests", "none recorded for this archive", -1);
        for (i = 0; i < d->ntests && i < PKG_TESTS_MAX; i++)
            pr_field(i ? "" : "Runtime tests", d->tests[i], -1);
        if (d->ntests > PKG_TESTS_MAX) {
            snprintf(text, sizeof text, "and %d more", d->ntests - PKG_TESTS_MAX);
            pr_field("", text, -1);
        }
        pkg_added_text(d, text, sizeof text);
        pr_field("Added", text, -1);
        pr_field("Catalogue", d->catalogue[0] ? d->catalogue : "the catalogue file on this machine", -1);
    }
    print_relations(c, index, id, 1);
}

static pkg_status show_package(pkg_ctx *c, const char *index, const char *id, pkg_err *e)
{
    pkg_details d; pkg_status st; char a[80], b[80], why[240];
    st = pkg_details_get(c, index, id, &d, e);
    if (st != PKG_OK) return st;
    if (pr_rich()) { show_rich(c, index, id, &d); return PKG_OK; }
    rev_label(a, sizeof a, d.e.version, d.e.revision);
    printf("%s%s %s%s\n", pr_bold(), d.e.id, a, pr_off());
    if (d.e.summary[0]) printf("  %s\n", d.e.summary);
    if (d.index_missing) printf("  index:        none on this machine -- apkg update fetches it\n");
    else if (!d.in_index) printf("  index:        does not list it any more\n");
    if (d.ambiguous) printf("  ambiguous:    the index lists %d builds of it for this system; install refuses\n", d.ambiguous);
    if (d.e.category[0]) printf("  category:     %s\n", d.e.category);
    if (d.in_index) {
        printf("  target:       %s, ABI %s", d.e.arch[0] ? d.e.arch : "?", d.e.abi[0] ? d.e.abi : "?");
        if (d.nvariants > 1) printf(" (the index lists: %s)", d.variants);
        printf("\n");
    }
    printf("  compatibility: "); verdict(pkg_compat_word(d.compat)); printf(" -- %s\n", d.compat_why);
    if (d.in_index) {
        if (d.e.size >= 0) printf("  download:     %ld KB from %s\n", (d.e.size + 1023) / 1024, d.url);
        else printf("  download:     %s\n", d.url);
        if (d.sha256[0]) printf("  sha256:       %s\n", d.sha256);
    }
    if (d.source[0])  printf("  source:       %s\n", d.source);
    if (d.license[0]) printf("  license:      %s\n", d.license);
    if (d.in_index) {
        char text[600]; int i;
        pkg_review_text(&d, text, sizeof text);
        printf("  status:       %s\n", text);
        if (!d.ntests) printf("  runtime tests: none recorded for this archive\n");
        for (i = 0; i < d.ntests && i < PKG_TESTS_MAX; i++)
            printf("  %s %s\n", i ? "              " : "runtime tests:", d.tests[i]);
        if (d.ntests > PKG_TESTS_MAX) printf("                and %d more\n", d.ntests - PKG_TESTS_MAX);
        pkg_added_text(&d, text, sizeof text);
        printf("  added:        %s\n", text);
        printf("  catalogue:    %s\n", d.catalogue[0] ? d.catalogue : "the catalogue file on this machine");
    }
    print_relations(c, index, id, 0);
    if (d.in_index) {
        if (d.compat != PKG_COMPAT_NATIVE)
            printf("  requirements: not checked -- it is not compatible with this machine\n");
        else if (!d.e.requires[0])
            printf("  requirements: none stated\n");
        else {
            char reqs[1024]; pkg_err e2; memset(&e2, 0, sizeof e2);
            if (pkg_requirements(c, index, id, reqs, sizeof reqs, &e2) == PKG_OK) {
                char *line = reqs, *nl;
                printf("  requirements: "); verdict(d.requirements); printf(", probed now:\n");
                while (line && *line) {
                    char *colon, *sp;
                    nl = strchr(line, '\n'); if (nl) *nl = 0;
                    /* "crt.library: satisfied (...)": the verdict in colour */
                    colon = strstr(line, ": ");
                    sp = colon ? strchr(colon + 2, ' ') : NULL;
                    if (colon && sp) {
                        *sp = 0;
                        printf("    %.*s: ", (int)(colon - line), line); verdict(colon + 2); printf(" %s\n", sp + 1);
                    } else if (colon) {
                        printf("    %.*s: ", (int)(colon - line), line); verdict(colon + 2); printf("\n");
                    } else printf("    %s\n", line);
                    line = nl ? nl + 1 : NULL;
                }
            } else printf("  requirements: %s (%s)\n", d.e.requires, e2.summary);
        }
    }
    if (!d.e.installed) printf("  installed:    no\n");
    else {
        rev_label(b, sizeof b, d.e.installed_version, d.e.installed_revision);
        printf("  installed:    %s%s%s%s%s%s, in %s%s%s\n", pr_color(PR_GREEN), b, pr_off(),
               d.installed_arch[0] ? " (" : "", d.installed_arch, d.installed_arch[0] ? ")" : "",
               d.installed_dir, d.installed_when[0] ? ", since " : "", d.installed_when);
        if (d.in_index) {
            if (pkg_can_upgrade(&d.e, why, sizeof why)) printf("  upgrade:      available, %s -> %s\n", b, a);
            else printf("  upgrade:      no -- %s\n", why);
        }
        if (pkg_can_rollback(c, id, why, sizeof why)) printf("  roll back:    possible\n");
        else printf("  roll back:    no -- %s\n", why);
    }
    print_notes(&d, 0);
    return PKG_OK;
}

/* Rich search consumes the same rows as JSON. The renderer wraps all fields
 * and switches to blocks when the table cannot fit; no description is lost. */
static void search_table(const pkg_entries *es, int hidden)
{
    int i;
    char text[160];
    snprintf(text, sizeof text, "%d package%s", es->n, es->n == 1 ? "" : "s");
    pr_heading(text);
    if (!es->n) pr_text("Nothing matches this search.", 0);
    else pr_search_heading();
    for (i = 0; i < es->n; i++) {
        const pkg_entry *p = &es->v[i];
        char version[80], note[100];
        snprintf(note, sizeof note, "%s/%s", p->arch, p->abi);
        if (!p->ours) {
            const char *why = (*pkg_arch() && *p->arch && strcmp(p->arch, pkg_arch())) ? "other CPU" :
                              !pkg_abi_known(p->abi) ? "unknown ABI" : "other ABI";
            snprintf(note, sizeof note, "%s: %s/%s", why, p->arch, p->abi);
        }
        rev_label(version, sizeof version, p->version, p->revision);
        pr_search_row(p->id, version, p->installed ? "installed" : "not installed", p->summary, note);
    }
    if (hidden) {
        snprintf(text, sizeof text, "%d hidden: other CPU/ABI or unknown compatibility. Use --all-abi to list them.", hidden);
        pr_text(text, 0);
    }
    pr_text("Details: apkg show <id>", 0);
}

static void usage(void)
{
    printf("%s -- Find and install software for AROS\n\n", VERSION);
    printf("Usage: apkg [options] <command>\n\n"
           "Commands:\n"
           "  search [term]     Search available packages\n"
           "  show <id>         Show package details and requirements\n"
           "  install <id>      Install a package\n"
           "  list              List installed packages\n"
           "  update            Refresh the package catalogue\n"
           "  outdated          List installed packages with updates\n"
           "  recent            List applications recently added to the catalogue\n"
           "  upgrade <id>      Install a newer version or revision\n"
           "  upgrade --all     Upgrade every package that can be upgraded now\n"
           "  rollback <id>     Restore the previous package revision\n"
           "  remove <id>       Remove a package; keep locally modified files\n"
           "  verify <id>       Check installed files for changes\n"
           "  info <id>         Show the installed package record\n"
           "  doctor            Report installation and recovery problems\n"
           "  run <id> [file]   Start an installed application (optionally with a file)\n"
           "  run --check <id>  Say whether and how it would start, without starting it\n"
           "  open <id>         Open an installed package's drawer on the desktop\n"
           "  self-update       Update apkg and its PkgManager to the latest stable release\n"
           "  self-update --check  Say what is installed and whether there is a newer one\n"
           "  self-update --drawer <dir>  Update this apkg with the installation in <dir>\n"
           "  self-update --apkg-only     Update this apkg on its own, without PkgManager\n"
           "  self-update --recover       Finish or undo an update that was interrupted\n\n"
           "Options:\n"
           "  --root <dir>      Package directory (default: SYS:Packages)\n"
           "  --plain           Disable formatting and progress output\n"
           "  --color=<mode>    Colour output: auto, always or never\n"
           "  --json            Output search, list, outdated and recent as JSON\n"
           "  --all             With outdated: every installed package and its state\n"
           "  --yes             Carry out an install plan that changes other packages\n"
           "  --limit <n>       With recent: at most n (default 10; 0 = all)\n"
           "  --days <n>        With recent: added in the last n days\n"
           "  --since <date>    With recent: added on or after YYYY-MM-DD\n"
           "  --builds          With recent: also new builds of older applications\n"
           "  --since-last-update  With recent: what the last apkg update brought\n"
           "  --verbose         Also show download addresses and file locations\n"
           "  --version         Show version and build information\n"
           "  --about           Show author, project and licence information\n"
           "  --help            Show this help\n"
           "  --help-all        Include advanced and testing options\n"
           "  --help-testing    Show testing options\n\n"
           "Example:\n"
           "  apkg update\n"
           "  apkg outdated\n"
           "  apkg search soliton\n"
           "  apkg install soliton\n");
}

static void usage_testing(void)
{
    printf("Testing options (use a separate --root):\n"
           "  assert-hash <file> <sha>  Check a file's SHA-256\n"
           "  --verify-name <name>     Override the TLS certificate hostname\n"
           "  --abi <v0|v1|v11>        Override ABI selection for testing\n"
           "  --expect <status>        Require the specified exit status\n"
           "  --report <file>          Write a machine-readable test report\n"
           "  --run-id <id>            Set the test report identifier\n"
           "  --machine <name>         Record the test machine name\n"
           "  --slow <ms>              Delay download and extraction steps\n"
           "  --progress               Print progress callback events\n"
           "  --fail-at commit|doctor  Simulate a failed write\n"
           "  --break-download-at <n>  Simulate one connection reset after n bytes\n"
           "  --self-update-from <url> Release base URL for self-update (https, ends in /)\n"
           "  --self-update-sums <file> Use a local SHA256SUMS for self-update\n"
           "  --self-update-fail-at <how> Fail the self-update: write:<n> (n-th new file),\n"
           "                          rename:<n> (n-th rename), stop:<n> (stop dead after the\n"
           "                          n-th rename), managed:<n> (--interrupt-at in the package)\n"
           "  --corrupt-download-at <n> Simulate one TLS data error after n bytes\n"
           "  --cancel-at <phase>      Request cancellation in a phase\n"
           "                          download, download-mid, verify, extract, publish\n"
           "  --break-recovery <n>     Interrupt recovery after n files\n"
           "  --interrupt-at <n>       Interrupt an operation at a checkpoint:\n"
           "    1 plan; 2 directory; 3 directory marker; 4 registry;\n"
           "    5 before commit; 6 remove; 7 publish; 8 drawer icon;\n"
           "    9 old tree moved; 10 new tree published;\n"
           "    11 replacement installed, old package not yet removed\n"
           "Interrupted operations may require recovery on the next run.\n");
}

static void usage_all(void)
{
    usage();
    printf("\nAdvanced options:\n"
           "  requires <id>       Check system requirements for a package\n"
           "  unlock              Remove a stale operation lock\n"
           "  doctor --retry      Retry pending recovery\n"
           "  --keep-rejected     Keep a download that fails verification\n"
           "                      as <cache>/<id>.<ext>.rejected (not installed)\n"
           "  --index <file>      Read a local catalogue file\n"
           "  --index-url <url>   Set the catalogue download URL\n"
           "  --all-abi           Include packages for other CPUs and ABIs\n"
           "  --dry-run           Preview install/remove/upgrade/rollback\n"
           "  --fetch             Allow downloads during a dry run\n\n");
    usage_testing();
}

/* The progress contract, driven from the command line so a guest run can show
 * the callback fires and that cancelling does exactly what pkg.h promises. */
static const char *cancel_at = NULL;
static int show_progress = 0;
static int show_progress_lines = 0;  /* print the stages (rich or plain text) */
static int stop_requested_late;      /* Ctrl-C arrived when it could no longer stop anything */
static void cli_progress(void *u, const pkg_progress_ev *ev, int *cancel)
{
    (void)u;
    /* Ctrl-C asks the library to stop. It honours that only while nothing
       has been changed (the download, and right after verification); once a
       transaction exists the request is ignored and the operation finishes,
       because a half-done one is what recovery would have to clean up. The
       signal is consumed here, so it is asked once per press. */
    if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        if (ev->can_cancel) *cancel = 1;
        else stop_requested_late = 1;
    }
    if (show_progress)
        printf("progress %s %s %lu/%lu cancel=%s\n", ev->id, ev->phase, ev->done, ev->total,
               ev->can_cancel ? "yes" : "no");
    else if (show_progress_lines) pr_progress(ev->id, ev->phase, ev->done, ev->total);
    /* Set regardless of can_cancel on purpose: the test is that the LIBRARY
       ignores it where it said it would, not that this caller is polite. */
    if (cancel_at && strcmp(ev->phase, cancel_at) == 0) *cancel = 1;
    /* "download-mid": only once bytes are flowing, so the cancel lands inside
       the transport rather than at the check before it. */
    if (cancel_at && strcmp(cancel_at, "download-mid") == 0 &&
        strcmp(ev->phase, "download") == 0 && ev->done > 0) *cancel = 1;
}

/* Rows as a JSON array, one object per line. The strings are the index's and
 * the registry's, so the escapes needed are the two our own writer emits. */
static void jstr(const char *v)
{
    putchar('"');
    for (; *v; v++) {
        if (*v == '"' || *v == '\\') putchar('\\');
        putchar(*v);
    }
    putchar('"');
}

static void print_rows(const pkg_entries *es, int hidden)
{
    int i;
    printf("{ \"rows\": [");
    for (i = 0; i < es->n; i++) {
        const pkg_entry *p = &es->v[i];
        printf("%s\n  { \"id\": ", i ? "," : "");     jstr(p->id);
        printf(", \"version\": ");                    jstr(p->version);
        printf(", \"revision\": %ld, \"summary\": ", p->revision); jstr(p->summary);
        printf(", \"category\": ");                   jstr(p->category);
        printf(", \"arch\": ");                       jstr(p->arch);
        printf(", \"abi\": ");                        jstr(p->abi);
        printf(", \"kind\": ");                       jstr(p->kind);
        printf(", \"size\": %ld, \"ours\": %s, \"installed\": %s }",
               p->size, p->ours ? "true" : "false", p->installed ? "true" : "false");
    }
    printf("%s], \"hidden\": %d }\n", es->n ? "\n" : "", hidden);
}

/* One place, so every exit path records the run. */
static void write_report(const char *path, const char *run_id, const char *machine,
                         const char *cmd, const char *arg, const char *root,
                         pkg_stop stop, pkg_status st, const pkg_err *e)
{
    FILE *f;
    char rid[64];
    if (!path) return;
    f = fopen(path, "wb");
    if (!f) { printf("apkg: cannot write the report to %s\n", path); return; }
    if (run_id) snprintf(rid, sizeof rid, "%s", run_id);
    else        snprintf(rid, sizeof rid, "%lx", (unsigned long)time(NULL));
    {
        char self[65];
        pkg_self_sha256(self);      /* "unknown" if it could not be read */
        fprintf(f,
            "{ \"run_id\": \"%s\", \"machine\": \"%s\",\n"
            "  \"binary_sha256\": \"%s\",\n"
            "  \"source_id\": \"%s\", \"toolchain\": \"%s\",\n"
            "  \"sdk\": \"%s\", \"cc\": \"%s\",\n"
            "  \"command\": \"%s\", \"argument\": \"%s\", \"root\": \"%s\",\n"
            "  \"interrupt_at\": %d,\n"
            "  \"status\": %d, \"status_text\": \"%s\",\n"
            "  \"summary\": \"%s\", \"subject\": \"%s\" }\n",
            rid, machine, self, pkg_build_id(), pkg_toolchain(),
            pkg_sdk(), pkg_cc(), cmd, arg ? arg : "", root,
            (int)stop, (int)st, pkg_strstatus(st),
            st == PKG_OK ? "ok" : e->summary, st == PKG_OK ? "" : e->subject);
    }
    fclose(f);
    printf("report %s\n", path);
}

/* What the command was doing, for its error: "install soliton". Empty for
   commands whose errors read on their own. */
static char doing[96];
static const char *doing_id;       /* the package named in `doing` */
static const char *cancel_noun;    /* "Installation", for "Installation cancelled" */

/* The error as libpkg worded it -- the cause is never replaced by a
   generic one -- after what was being done, and the subject and detail on
   their own lines. A cancel is said as a cancel, not as a failure. */
static void show(const pkg_err *e)
{
    char head[400];
    pr_end_line();
    if (doing[0] && e->status == PKG_E_CANCELLED && !doing_id)
        snprintf(head, sizeof head, "%s cancelled", cancel_noun ? cancel_noun : "Operation");
    else if (doing[0] && e->status == PKG_E_CANCELLED)
        /* libpkg's summary ("download cancelled") only repeats the word;
           anything it says beyond that is in the detail line. */
        snprintf(head, sizeof head, "%s cancelled: %s%s%s", cancel_noun ? cancel_noun : "Operation",
                 doing_id ? doing_id : doing,
                 strstr(e->summary, "cancel") ? "" : " -- ", strstr(e->summary, "cancel") ? "" : e->summary);
    else if (doing[0])
        snprintf(head, sizeof head, "Could not %s: %s", doing, e->summary);
    else
        snprintf(head, sizeof head, "apkg: %s", e->summary);
    fputs(pr_color(PR_RED), stdout);
    pr_text(head, 0);
    fputs(pr_off(), stdout);
    /* The subject is often the package itself, already in the first line. */
    if (e->subject[0] && !(doing_id && !strcmp(e->subject, doing_id))) pr_text(e->subject, 2);
    if (e->detail[0])  pr_text(e->detail, 2);
}

/* The package's notes as a section. After an install, the copy just
   recorded; in show, the catalogue's current text, or the recorded copy
   when the catalogue no longer has the package. */
static void print_notes(const pkg_details *d, int after_install)
{
    const char *n = after_install || !d->in_index ? d->notes_recorded : d->notes;
    if (!after_install && d->in_index && d->notes_bad) {
        pr_section("Notes from the package");
        pr_text("Not shown: they are not plain lines this apkg can display.", 2);
        return;
    }
    if (!*n) return;
    pr_section(after_install || d->in_index ? "Notes from the package" : "Notes recorded at installation");
    pr_text(n, 2);
}

/* After an install, upgrade or rollback: what is installed now, where, and
   how to get to it, from the registry entry just written. */
static void print_installed(pkg_ctx *c, const char *index, const char *id, const char *verb)
{
    pkg_details d; pkg_err e2; char v[80], line[2 * PKG_MAXPATH + PKG_MAXID + 32];
    memset(&e2, 0, sizeof e2);
    putchar('\n');
    if (pkg_details_get(c, index, id, &d, &e2) != PKG_OK || !d.e.installed) {
        pr_word(PR_GREEN, verb); printf(" %s\n", id);
        return;
    }
    rev_label(v, sizeof v, d.e.installed_version, d.e.installed_revision);
    pr_word(PR_GREEN, verb); printf(" %s %s\n", id, v);
    pr_field("Location", d.installed_dir, -1);
    open_hint(line, sizeof line, hint_root, id);
    pr_field("Open folder", line, -1);
    print_notes(&d, 1);
}

/* The version the catalogue offers, for an operation's first line. */
static void offered(pkg_ctx *c, const char *index, const char *id, char *v, size_t n, int installed)
{
    pkg_details d; pkg_err e2;
    v[0] = 0;
    memset(&e2, 0, sizeof e2);
    if (pkg_details_get(c, index, id, &d, &e2) != PKG_OK) return;
    if (installed && d.e.installed) rev_label(v, n, d.e.installed_version, d.e.installed_revision);
    else if (!installed && d.in_index) rev_label(v, n, d.e.version, d.e.revision);
}

/* ---------------------------------------------------------- relations */

static const char *op_verb(int k)
{
    return k == PKG_OP_DEPENDENCY ? "install (needed)" : k == PKG_OP_INSTALL ? "install" :
           k == PKG_OP_REMOVE_REPLACED ? "remove (replaced)" : "keep, as installed by you";
}

/* The plan of `apkg install`, step by step, before anything happens. */
static void print_plan(const pkg_install_plan *pl)
{
    int i; char line[400], v[80];
    pr_heading("Plan");
    for (i = 0; i < pl->nops; i++) {
        const pkg_plan_op *o = &pl->ops[i];
        rev_label(v, sizeof v, o->version, o->revision);
        snprintf(line, sizeof line, "%d. %s %s %s%s%s", i + 1, op_verb(o->kind), o->id, v,
                 o->note[0] ? " -- " : "", o->note);
        pr_text(line, 2);
    }
}

/* After a run that stopped: which steps ran and which did not. */
static void print_plan_result(const pkg_install_plan *pl)
{
    int i; char line[500];
    pr_heading("What was done");
    for (i = 0; i < pl->nops; i++) {
        const pkg_plan_op *o = &pl->ops[i];
        snprintf(line, sizeof line, "%d. %s %s: %s%s%s", i + 1, op_verb(o->kind), o->id,
                 !o->done ? "not done" : o->status == PKG_OK ? "done" : "failed",
                 o->done && o->status != PKG_OK ? " -- " : "", o->done && o->status != PKG_OK ? o->error : "");
        pr_text(line, 2);
    }
}

static int assume_yes;   /* --yes */

/* A plan that changes more than the package named is confirmed first: in
   a window, by answering; in a script, by --yes. */
static int plan_confirmed(void)
{
    char ans[16];
    if (assume_yes) return 1;
    if (!IsInteractive(Input())) {
        pr_text("This changes more than the package you named. Run it again with --yes to go ahead, "
                "or with --dry-run to see it only. Nothing was changed.", 0);
        return 0;
    }
    printf("Go ahead? [y/N] ");
    fflush(stdout);
    if (!fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
        pr_text("Nothing was changed.", 0);
        return 0;
    }
    return 1;
}

/* What `show` lists under Relations. */
static void print_relations(pkg_ctx *c, const char *index, const char *id, int rich)
{
    pkg_relation *rows = (pkg_relation *)malloc(sizeof *rows * PKG_REL_ROWS);
    pkg_err e2; int n = 0, i; char line[600];
    memset(&e2, 0, sizeof e2);
    if (!rows) return;
    if (pkg_relations_get(c, index, id, rows, PKG_REL_ROWS, &n, &e2) != PKG_OK || !n) { free(rows); return; }
    if (rich) { putchar('\n'); pr_heading("Relations"); }
    for (i = 0; i < n; i++) {
        const pkg_relation *r = &rows[i];
        char what[200];
        snprintf(what, sizeof what, "%s%s%s", r->id, r->min_version[0] ? " >= " : "", r->min_version);
        if (r->kind == PKG_REL_RECOMMENDS && r->reason[0])
            snprintf(line, sizeof line, "%s -- %s. %s", what, r->reason, r->state);
        else snprintf(line, sizeof line, "%s: %s", what, r->state);
        if (r->kind == PKG_REL_NEEDED_BY)
            snprintf(line, sizeof line, "%s%s%s%s: %s", r->id, r->min_version[0] ? " (needs at least " : "",
                     r->min_version, r->min_version[0] ? ")" : "", r->state);
        if (rich) pr_field(r->kind == PKG_REL_DEPENDS ? "Needs" : r->kind == PKG_REL_RECOMMENDS ? "Recommends" :
                           r->kind == PKG_REL_CONFLICTS ? "Conflicts with" : r->kind == PKG_REL_REPLACES ? "Replaces" :
                           "Needed by", line,
                           r->kind == PKG_REL_DEPENDS ? (r->satisfied ? PR_GREEN : PR_YELLOW) : -1);
        else printf("  %-13s %s\n", r->kind == PKG_REL_DEPENDS ? "needs:" : r->kind == PKG_REL_RECOMMENDS ? "recommends:" :
                    r->kind == PKG_REL_CONFLICTS ? "conflicts:" : r->kind == PKG_REL_REPLACES ? "replaces:" : "needed by:", line);
    }
    free(rows);
}

static void heading2(const char *verb, const char *id, const char *v)
{
    char t[200];
    snprintf(t, sizeof t, "%s %s%s%s", verb, id, *v ? " " : "", v);
    pr_heading(t);
}

/* ------------------------------------------------------------- outdated */

/* "1.2-aros3", or "-" when the catalogue offers nothing to compare. */
static void ver_cell(char *out, size_t n, const char *v, long rev)
{
    if (!*v) snprintf(out, n, "-");
    else rev_label(out, n, v, rev);
}

static const char *state_text(pkg_upd_state s)
{
    switch (s) {
    case PKG_UPD_AVAILABLE:       return "update available";
    case PKG_UPD_BLOCKED:         return "update blocked";
    case PKG_UPD_CURRENT:         return "up to date";
    case PKG_UPD_NEWER_INSTALLED: return "newer than the catalogue";
    case PKG_UPD_INCOMPARABLE:    return "cannot compare versions";
    case PKG_UPD_AMBIGUOUS:       return "several catalogue entries";
    case PKG_UPD_NOT_IN_INDEX:    return "not in the catalogue";
    case PKG_UPD_OTHER_TARGET:    return "no build for this target";
    case PKG_UPD_NO_CATALOGUE:    return "no catalogue";
    }
    return "?";
}

/* The one line `apkg update` ends with, and the line under `outdated`:
   how many, and the command that shows or does more. */
static void updates_summary(const pkg_updates *u, const char *root)
{
    char line[900], cmd[PKG_MAXPATH + 40];
    if (!u->installed) { pr_text("No packages are installed.", 0); return; }
    if (!u->newer) {
        snprintf(line, sizeof line, "No updates: the catalogue offers nothing newer for the %d installed package%s.",
                 u->installed, u->installed == 1 ? "" : "s");
        pr_text(line, 0);
        return;
    }
    cmd_hint(cmd, sizeof cmd, root, "outdated");
    if (!u->ready)
        snprintf(line, sizeof line, "%d update%s for installed packages, none of which can be installed now. See why: %s",
                 u->newer, u->newer == 1 ? "" : "s", cmd);
    else if (u->blocked)
        snprintf(line, sizeof line, "%d update%s available for installed packages (%d of them blocked). See them: %s",
                 u->newer, u->newer == 1 ? "" : "s", u->blocked, cmd);
    else
        snprintf(line, sizeof line, "%d update%s available for installed packages. See them: %s",
                 u->newer, u->newer == 1 ? "" : "s", cmd);
    pr_text(line, 0);
}

/* ------------------------------------------------------- what is new */

/* "a, b, c and 4 more": at most `max` rows of one kind, by name. */
static void news_names(const pkg_news *nw, pkg_news_kind k, int max, char *out, size_t n)
{
    int i, shown = 0, all = 0;
    size_t used = 0;
    out[0] = 0;
    for (i = 0; i < nw->n; i++) {
        const pkg_news_row *r = &nw->rows[i];
        char one[300], a[80], b[80];
        if (r->kind != k) continue;
        if (all++ >= max) continue;
        if (k == PKG_NEWS_UPDATED) {
            rev_label(a, sizeof a, r->old_version, r->old_revision);
            rev_label(b, sizeof b, r->version, r->revision);
            snprintf(one, sizeof one, "%s %s -> %s", r->id, a, b);
        } else if (k == PKG_NEWS_RECENT)
            snprintf(one, sizeof one, "%s (%s)", r->id, r->app_first_added);
        else snprintf(one, sizeof one, "%s%s", r->id, r->returned ? " (back)" : "");
        if (used + strlen(one) + 3 < n) used += (size_t)snprintf(out + used, n - used, "%s%s", shown++ ? ", " : "", one);
    }
    if (all > shown && used + 20 < n) snprintf(out + used, n - used, " and %d more", all - shown);
}

/* After `apkg update`: the short account; `apkg recent --since-last-update`
   has the whole of it. */
static void news_summary(pkg_ctx *c, const char *index, const char *root)
{
    pkg_news nw; pkg_err e; char line[1200], names[900], cmd[PKG_MAXPATH + 60];
    memset(&e, 0, sizeof e);
    if (pkg_news_last(c, index, &nw, &e) != PKG_OK || !nw.recorded || !nw.current) {
        pr_text("What is new in it could not be worked out on this machine.", 0);
        pkg_news_free(&nw);
        return;
    }
    if (nw.first) {
        pr_text(nw.first_why, 0);
        if (nw.recent) {
            news_names(&nw, PKG_NEWS_RECENT, NEWS_SUMMARY_NAMES, names, sizeof names);
            snprintf(line, sizeof line, "Recently added to the catalogue: %s", names);
            pr_text(line, 0);
            cmd_hint(cmd, sizeof cmd, root, "recent");
            snprintf(line, sizeof line, "More: %s", cmd);
            pr_text(line, 0);
        } else pr_text("The catalogue does not date its applications, so recent additions cannot be listed.", 0);
        pkg_news_free(&nw);
        return;
    }
    if (!nw.n) {
        snprintf(line, sizeof line, "Nothing new for this system since the catalogue fetched %s.",
                 nw.previous[0] ? nw.previous : "the previous update");
        pr_text(line, 0);
        pkg_news_free(&nw);
        return;
    }
    if (nw.apps) {
        news_names(&nw, PKG_NEWS_APP, NEWS_SUMMARY_NAMES, names, sizeof names);
        snprintf(line, sizeof line, "New application%s (%d): %s", nw.apps == 1 ? "" : "s", nw.apps, names);
        pr_text(line, 0);
    }
    if (nw.builds) {
        news_names(&nw, PKG_NEWS_BUILD, NEWS_SUMMARY_NAMES, names, sizeof names);
        snprintf(line, sizeof line, "New build%s for this system (%d): %s", nw.builds == 1 ? "" : "s", nw.builds, names);
        pr_text(line, 0);
    }
    if (nw.updated) {
        news_names(&nw, PKG_NEWS_UPDATED, NEWS_SUMMARY_NAMES, names, sizeof names);
        snprintf(line, sizeof line, "Updated in the catalogue (%d): %s", nw.updated, names);
        pr_text(line, 0);
    }
    cmd_hint(cmd, sizeof cmd, root, "recent --since-last-update");
    snprintf(line, sizeof line, "Full list: %s", cmd);
    pr_text(line, 0);
    pkg_news_free(&nw);
}

/* recent: rows of `apkg recent` are additions by date, not news since an update. */
static const char *news_label(const pkg_news_row *r, int recent)
{
    switch (r->kind) {
    case PKG_NEWS_APP:     return recent ? "added" : r->returned ? "back in the catalogue" : "new application";
    case PKG_NEWS_BUILD:   return "new build for this system";
    case PKG_NEWS_UPDATED: return "updated";
    case PKG_NEWS_RECENT:  return "recently added";
    }
    return "";
}

static void news_rows(const pkg_news *nw, int limit, int recent)
{
    int i, w = 4, shown[4] = { 0, 0, 0, 0 };
    char line[600], v[200], o[80];
    for (i = 0; i < nw->n; i++)
        if ((int)strlen(nw->rows[i].id) > w) w = (int)strlen(nw->rows[i].id);
    if (w > 24) w = 24;
    for (i = 0; i < nw->n; i++) {
        const pkg_news_row *r = &nw->rows[i];
        const char *date = r->kind == PKG_NEWS_BUILD ? r->first_added :
                           r->kind == PKG_NEWS_UPDATED ? "" : r->app_first_added;
        if (limit > 0 && shown[r->kind]++ >= limit) continue;
        rev_label(v, sizeof v, r->version, r->revision);
        if (r->kind == PKG_NEWS_UPDATED) {
            rev_label(o, sizeof o, r->old_version, r->old_revision);
            snprintf(v + strlen(v), sizeof v - strlen(v), " (was %s)", o);
        }
        printf("  %-10s  %-*s  %s\n", date[0] ? date : r->kind == PKG_NEWS_UPDATED ? "" : "no date", w, r->id, v);
        snprintf(line, sizeof line, "%s%s%s%s", news_label(r, recent), r->installed ? ", installed" : "",
                 r->summary[0] ? " -- " : "", r->summary);
        pr_text(line, 14);
    }
}

/* The ABI the news model judges by: --abi when testing, else this build's. */
static const char *my_abi_word(void)
{
    const char *o = pkg_abi_override();
    return o && *o ? o : pkg_abi();
}

/* apkg recent. Exit status: 0 answered (also with nothing to list); 10 no
   catalogue, or for --since-last-update no record of an update that matches
   the catalogue in use; 5 usage. Documented in docs/guide/using.md. */
static int recent(pkg_ctx *c, const char *index, const char *root, const pkg_recent_opts *o,
                  int last, int limit, int json)
{
    pkg_news nw; pkg_err e; char line[800], cmd[PKG_MAXPATH + 60];
    pkg_status st;
    memset(&e, 0, sizeof e);
    st = last ? pkg_news_last(c, index, &nw, &e) : pkg_recent(c, index, o, &nw, &e);
    if (st != PKG_OK) {
        if (json) {
            char buf[400]; js_out o;
            js_out_init(&o, buf, sizeof buf);
            js_raw(&o, "{ \"schema\": 1, \"error\": "); js_vstr(&o, e.summary); js_raw(&o, " }\n");
            fputs(o.fail ? "{ \"schema\": 1, \"error\": \"\" }\n" : buf, stdout);
        }
        else show(&e);
        return 10;
    }
    if (json) {
        char *js = pkg_news_json(&nw, last ? "last-update" : "recent", last ? limit : 0);
        int rc = (last && (!nw.recorded || !nw.current)) ? 10 : 0;
        if (js) fputs(js, stdout); else { printf("{ \"schema\": 1, \"error\": \"out of memory\" }\n"); rc = 10; }
        free(js); pkg_news_free(&nw);
        return rc;
    }
    if (last) {
        if (!nw.recorded || !nw.current) {
            cmd_hint(cmd, sizeof cmd, root, "update");
            snprintf(line, sizeof line, nw.recorded
                     ? "The catalogue in use is not the one the last recorded update fetched; it was replaced "
                       "since by other means. Run %s to compare again."
                     : "No catalogue update has been recorded on this machine yet. Run %s.", cmd);
            pr_text(line, 0);
            pkg_news_free(&nw);
            return 10;
        }
        snprintf(line, sizeof line, "What the update of %s brought", nw.when);
        pr_heading(line);
        if (nw.first) pr_text(nw.first_why, 0);
        else if (nw.previous[0]) {
            snprintf(line, sizeof line, "Compared with the catalogue fetched %s, for %s/%s.", nw.previous, pkg_arch(), my_abi_word());
            pr_text(line, 0);
        }
        if (!nw.n) pr_text(nw.first ? "The catalogue does not date its applications." : "Nothing new for this system.", 0);
        else news_rows(&nw, limit, 0);
    } else {
        if (o->since[0]) snprintf(line, sizeof line, "Added since %s, for %s/%s", o->since, pkg_arch(), my_abi_word());
        else snprintf(line, sizeof line, "Recently added, for %s/%s", pkg_arch(), my_abi_word());
        pr_heading(line);
        if (nw.when[0]) {
            snprintf(line, sizeof line, "Catalogue fetched %s; dates are when the catalogue first published them.", nw.when);
            pr_text(line, 0);
        }
        if (!nw.n) pr_text(nw.unknown_dates ? "Nothing dated in that period."
                                            : "No applications for this system were added in that period.", 0);
        else news_rows(&nw, 0, 1);
        if (nw.unknown_dates) {
            snprintf(line, sizeof line, "%d application%s for this system %s no date in the catalogue and %s not listed.",
                     nw.unknown_dates, nw.unknown_dates == 1 ? "" : "s", nw.unknown_dates == 1 ? "has" : "have",
                     nw.unknown_dates == 1 ? "is" : "are");
            pr_text(line, 0);
        }
        if (!o->builds) {
            cmd_hint(cmd, sizeof cmd, root, "recent --builds");
            snprintf(line, sizeof line, "New builds of older applications too: %s", cmd);
            pr_text(line, 0);
        }
    }
    pr_text("Details: apkg show <id>", 0);
    pkg_news_free(&nw);
    return 0;
}

/* Exit status: 0 the check ran (whether or not anything is newer); 10 no
   catalogue, one that cannot be read, or the root cannot be read; 5 usage.
   Documented in docs/guide/using.md. */
static int outdated(pkg_ctx *c, const char *index, const char *root, int all, int json)
{
    pkg_updates u; pkg_err e; int i, w = 2, shown = 0, rc;
    char line[900], a[80], b[80], cmd[PKG_MAXPATH + 40];

    memset(&e, 0, sizeof e);
    if (pkg_updates_get(c, index, 1, &u, &e) != PKG_OK) { show(&e); return 10; }
    rc = u.catalogue == PKG_CAT_OK ? 0 : 10;
    if (json) {
        char *js = pkg_updates_json(&u);
        if (js) fputs(js, stdout); else { printf("{ \"error\": \"out of memory\" }\n"); rc = 10; }
        free(js);
        pkg_updates_free(&u);
        return rc;
    }
    if (u.catalogue != PKG_CAT_OK) {
        cmd_hint(cmd, sizeof cmd, root, "update");
        fputs(pr_color(PR_RED), stdout);
        snprintf(line, sizeof line, u.catalogue == PKG_CAT_MISSING
                 ? "No catalogue on this machine, so updates cannot be checked. Fetch it: %s"
                 : "The catalogue cannot be read, so updates cannot be checked. Fetch it again: %s", cmd);
        pr_text(line, 0);
        fputs(pr_off(), stdout);
        if (u.catalogue_error[0]) pr_text(u.catalogue_error, 2);
        pkg_updates_free(&u);
        return rc;
    }
    for (i = 0; i < u.n; i++)
        if ((all || u.rows[i].newer) && (int)strlen(u.rows[i].id) > w) w = (int)strlen(u.rows[i].id);
    if (w > 24) w = 24;
    snprintf(line, sizeof line, "%s for installed packages", all ? "Status" : "Updates");
    pr_heading(line);
    if (u.catalogue_when[0]) {
        snprintf(line, sizeof line, "Catalogue updated %s%s", u.catalogue_when,
                 u.catalogue_stale ? " -- more than two weeks ago; apkg update refreshes it" : "");
        if (u.catalogue_stale) { fputs(pr_color(PR_YELLOW), stdout); pr_text(line, 0); fputs(pr_off(), stdout); }
        else pr_text(line, 0);
    }
    for (i = 0; i < u.n; i++) {
        const pkg_update_row *r = &u.rows[i];
        int col = r->state == PKG_UPD_AVAILABLE ? PR_GREEN : r->state == PKG_UPD_BLOCKED ? PR_YELLOW :
                  r->state == PKG_UPD_CURRENT ? -1 : PR_YELLOW;
        if (!all && !r->newer) continue;
        if (!shown++)
            printf("%s  %-*s  %-14s %-14s %s%s\n", pr_bold(), w, "Package", "Installed", "Available", "State", pr_off());
        ver_cell(a, sizeof a, r->installed_version, r->installed_revision);
        ver_cell(b, sizeof b, r->available_version, r->available_revision);
        printf("  %-*s  %-14s %-14s ", w, r->id, a, b);
        if (col < 0) fputs(state_text(r->state), stdout); else pr_word(col, state_text(r->state));
        putchar('\n');
        /* The reason under the row: a refusal the user can act on, never a
           bare word. An available update needs none. */
        if (r->state != PKG_UPD_AVAILABLE && r->state != PKG_UPD_CURRENT) pr_text(r->why, 4);
        else if (r->state == PKG_UPD_AVAILABLE && !strcmp(r->requirements, "undetermined"))
            pr_text("some of its system requirements could not be checked", 4);
    }
    if (shown) putchar('\n');
    updates_summary(&u, root);
    if (u.ready) {
        cmd_hint(cmd, sizeof cmd, root, "upgrade <id>");
        snprintf(line, sizeof line, "Upgrade one: %s", cmd);
        pr_text(line, 0);
        cmd_hint(cmd, sizeof cmd, root, "upgrade --all");
        snprintf(line, sizeof line, "Upgrade all that can be: %s", cmd);
        pr_text(line, 0);
    }
    if (!all) {
        int other = 0;
        for (i = 0; i < u.n; i++)
            if (u.rows[i].state != PKG_UPD_CURRENT && !u.rows[i].newer) other++;
        if (other) {
            cmd_hint(cmd, sizeof cmd, root, "outdated --all");
            snprintf(line, sizeof line, "%d more installed package%s neither current nor upgradable (newer than "
                     "the catalogue, not in it, or not comparable): %s", other, other == 1 ? " is" : "s are", cmd);
            pr_text(line, 0);
        }
    }
    if (u.pending) pr_text("Uncommitted transactions exist; the next apkg that changes something recovers them first.", 0);
    pkg_updates_free(&u);
    return rc;
}

/* `upgrade --all`: every package that can be upgraded now, one after the
 * other. Each is its own transaction with its own preview, protection of
 * changed files, rollback record and recovery, exactly as `apkg upgrade <id>`;
 * the group is not atomic, and the report says what happened to each. A
 * Ctrl-C stops before the next package, never inside a transaction. */
static volatile int stop_group;

static int upgrade_all(pkg_ctx *c, const char *index, const char *root, pkg_stop stop, int *failed,
                       int *stopped, pkg_status *last)
{
    pkg_updates u; pkg_err e; int i, done = 0, n = 0;
    char a[80], b[80], line[900];
    memset(&e, 0, sizeof e);
    *failed = 0; *stopped = 0; *last = PKG_OK;
    if ((*last = pkg_updates_get(c, index, 1, &u, &e)) != PKG_OK) { show(&e); return -1; }
    if (u.catalogue != PKG_CAT_OK) {
        pkg_updates_free(&u);
        pr_text("No readable catalogue, so there is nothing to upgrade to. Run apkg update first.", 0);
        return -1;
    }
    pr_heading("Upgrade plan");
    for (i = 0; i < u.n; i++) {
        const pkg_update_row *r = &u.rows[i];
        if (!r->newer) continue;
        ver_cell(a, sizeof a, r->installed_version, r->installed_revision);
        ver_cell(b, sizeof b, r->available_version, r->available_revision);
        if (r->state == PKG_UPD_AVAILABLE) { n++; snprintf(line, sizeof line, "%s %s -> %s", r->id, a, b); }
        else snprintf(line, sizeof line, "%s %s -> %s: skipped, %s", r->id, a, b, r->why);
        pr_text(line, 2);
    }
    if (!n) {
        pr_text(u.newer ? "Nothing can be upgraded now." : "No updates: everything installed is current.", 0);
        pkg_updates_free(&u);
        return 0;
    }
    snprintf(line, sizeof line, "%d package%s, one at a time. Each one is checked against the files you "
             "changed before it is touched; one that fails leaves the others as they are.", n, n == 1 ? "" : "s");
    pr_text(line, 0);
    for (i = 0; i < u.n; i++) {
        const pkg_update_row *r = &u.rows[i];
        pkg_status st;
        if (r->state != PKG_UPD_AVAILABLE) continue;
        /* A Ctrl-C that came while the previous package was changing files
           was consumed there and could not stop it; it still ends the group. */
        if (stop_group || stop_requested_late || (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)) {
            stop_group = 1; *stopped = 1;
            snprintf(line, sizeof line, "Stopped before %s (Ctrl-C); %d of %d done.", r->id, done, n);
            pr_text(line, 0);
            break;
        }
        putchar('\n');
        snprintf(doing, sizeof doing, "upgrade %s", r->id);
        doing_id = r->id; cancel_noun = "Upgrade";
        memset(&e, 0, sizeof e);
        st = pkg_upgrade(c, index, r->id, stop, &e);
        pr_end_line();
        if (st == PKG_OK) { done++; print_installed(c, index, r->id, "Upgraded"); }
        else { (*failed)++; *last = st; show(&e); if (st == PKG_E_CANCELLED) stop_group = 1; }
    }
    putchar('\n');
    snprintf(line, sizeof line, "Upgraded %d of %d%s%d failed or stopped.", done, n,
             *failed ? "; " : "", *failed);
    if (!*failed && *stopped) snprintf(line, sizeof line, "Upgraded %d of %d; stopped.", done, n);
    else if (!*failed) snprintf(line, sizeof line, "Upgraded %d of %d.", done, n);
    pr_text(line, 0);
    {
        char cmd[PKG_MAXPATH + 40];
        cmd_hint(cmd, sizeof cmd, root, "outdated");
        snprintf(line, sizeof line, "What is left: %s", cmd);
        if (done < n) pr_text(line, 0);
    }
    pkg_updates_free(&u);
    return done;
}

static const char *launch_mode_text(pkg_launch_mode m)
{
    return m == PKG_LAUNCH_WORKBENCH ? "Workbench, from its icon"
         : m == PKG_LAUNCH_SHELL ? "Shell command" : "not described";
}

/* apkg run: start it, or with --check (or --json) say whether and how it
   would start. The same pkg_app_info the window's Run button reads, so the
   command and the button cannot disagree. A Shell program run here uses
   this console and its return code becomes apkg's (*rc). */
static pkg_status run_app(pkg_ctx *c, const char *what, const char *const *args, int nargs,
                          int check, int json, pkg_err *e, int *rc)
{
    pkg_app_info a;
    pkg_run_result r;
    pkg_status st;
    char line[400];

    *rc = 0;
    if (check || json) {
        st = pkg_app_info_get(c, what, &a, e);
        if (st != PKG_OK) return st;
        if (json) {
            printf("{\"id\": "); jstr(a.id);
            printf(", \"state\": "); jstr(pkg_run_word(a.state));
            printf(", \"mode\": ");
            jstr(a.launch.mode == PKG_LAUNCH_WORKBENCH ? "workbench" : a.launch.mode == PKG_LAUNCH_SHELL ? "shell" : "");
            printf(", \"drawer\": "); jstr(a.drawer);
            printf(", \"program\": "); jstr(a.program);
            printf(", \"stack\": %ld, \"requirements\": ", a.stack); jstr(a.requirements);
            printf(", \"why\": "); jstr(a.why);
            printf("}\n");
        } else {
            pr_heading(a.name);
            pr_field("State", a.state == PKG_RUN_READY ? "can be started" : pkg_run_word(a.state),
                     a.state == PKG_RUN_READY ? PR_GREEN : PR_YELLOW);
            pr_field("Start", launch_mode_text(a.launch.mode), -1);
            if (a.program[0]) pr_field("Program", a.program, -1);
            if (a.launch.mode == PKG_LAUNCH_SHELL) pr_field("Directory", a.workdir, -1);
            if (a.stack > 0) {
                snprintf(line, sizeof line, "%ld bytes, from the program's icon", a.stack);
                pr_field("Stack", line, -1);
            }
            if (strcmp(a.requirements, "none")) pr_field("Requirements", a.requirements, -1);
            pr_text(a.why, 2);
        }
        /* The answer is the output; "cannot be started" is not an error of
           the question. A script reads the return code: 0 ready, 5 not. */
        if (a.state != PKG_RUN_READY) *rc = 5;
        return PKG_OK;
    }
    st = pkg_run(c, what, args, nargs, PKG_RUN_WAIT, &r, e);
    if (st != PKG_OK) return st;
    pr_text(r.how, 0);
    if (r.waited && r.rc > 0) *rc = (int)r.rc;
    return PKG_OK;
}

static int real_main(int argc, char **argv)
{
    const char *root = "SYS:Packages";
    const char *index = NULL;
    char indexbuf[512];
    pkg_stop stop = PKG_STOP_NONE;
    int i;
    const char *cmd = NULL, *arg = NULL;
    const char *report = NULL, *run_id = NULL, *machine = "unspecified";
    const char *index_url = PKG_INDEX_URL;
    const char *arg2 = NULL;
    const char *rest[16];            /* every word after the command: run passes them on */
    int nrest = 0;
    int group_rc = -1, shown = 0;    /* upgrade --all: its own exit status; errors already shown */
    int expect = -1, retry = 0, json = 0, dry = 0, fetch = 0, plain = 0, color = PR_COLOR_AUTO, check = 0, all = 0;
    su_opts suo = { 0, 0, 0, NULL, NULL };
    int limit = -1, days = -1, builds = 0, last_update = 0, rc_dry = 0;
    const char *since = NULL;
    pkg_ctx *c = NULL;
    pkg_err e;
    pkg_status st;

    memset(&e, 0, sizeof e);

    for (i = 1; i < argc; i++) {
        /* After `run <id>` every word is the program's, options included:
           `apkg run folio --help` must not print apkg's help. */
        if (cmd && arg && !strcmp(cmd, "run")) {
            if (nrest >= (int)(sizeof rest / sizeof rest[0])) {
                printf("apkg run: at most %d arguments can be passed on\n", (int)(sizeof rest / sizeof rest[0]) - 1);
                return 5;
            }
            rest[nrest++] = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--root") && i + 1 < argc)        hint_root = root = argv[++i];
        else if (!strcmp(argv[i], "--index") && i + 1 < argc)  index = argv[++i];
        else if (!strcmp(argv[i], "--index-url") && i + 1 < argc) index_url = argv[++i];
        else if (!strcmp(argv[i], "--interrupt-at") && i + 1 < argc)
            stop = (pkg_stop)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fail-at") && i + 1 < argc) {
            const char *w = argv[++i];
            pkg_test_fault(!strcmp(w, "commit") ? PKG_FAIL_COMMIT :
                           !strcmp(w, "doctor") ? PKG_FAIL_DOCTOR : PKG_FAIL_NONE);
        }
        else if (!strcmp(argv[i], "--break-recovery") && i + 1 < argc)
            pkg_test_break_recovery(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--verify-name") && i + 1 < argc)
            pkg_set_verify_name(argv[++i]);
        else if (!strcmp(argv[i], "--all-abi")) pkg_set_abi_show_all(1);
        else if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--days") && i + 1 < argc) days = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--since") && i + 1 < argc) since = argv[++i];
        else if (!strcmp(argv[i], "--builds")) builds = 1;
        else if (!strcmp(argv[i], "--yes")) assume_yes = 1;
        else if (!strcmp(argv[i], "--since-last-update")) last_update = 1;
        else if (!strcmp(argv[i], "--verbose")) pkg_set_verbose(1);
        else if (!strcmp(argv[i], "--keep-rejected")) pkg_set_keep_rejected(1);
        else if (!strcmp(argv[i], "--plain")) plain = 1;
        else if (!strcmp(argv[i], "--color=auto")) color = PR_COLOR_AUTO;
        else if (!strcmp(argv[i], "--color=always")) color = PR_COLOR_ALWAYS;
        else if (!strcmp(argv[i], "--color=never")) color = PR_COLOR_NEVER;
        else if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--fetch")) fetch = 1;
        else if (!strcmp(argv[i], "--progress")) show_progress = 1;
        else if (!strcmp(argv[i], "--slow") && i + 1 < argc) pkg_test_slow(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--self-update-from") && i + 1 < argc) su_test_from(argv[++i]);
        else if (!strcmp(argv[i], "--self-update-sums") && i + 1 < argc) su_test_sums(argv[++i]);
        else if (!strcmp(argv[i], "--self-update-fail-at") && i + 1 < argc) su_test_fail(argv[++i]);
        else if (!strcmp(argv[i], "--drawer") && i + 1 < argc) suo.drawer = argv[++i];
        else if (!strcmp(argv[i], "--apkg-only")) suo.drawer = "";
        else if (!strcmp(argv[i], "--recover")) suo.recover = 1;
        else if (!strcmp(argv[i], "--apply") && i + 1 < argc) suo.apply = argv[++i];
        else if (!strcmp(argv[i], "--protocol")) suo.protocol = 1;
        else if (!strcmp(argv[i], "--break-download-at") && i + 1 < argc)
            pkg_test_break_download(atol(argv[++i]), 1);
        else if (!strcmp(argv[i], "--corrupt-download-at") && i + 1 < argc)
            pkg_test_break_download(atol(argv[++i]), 2);
        else if (!strcmp(argv[i], "--cancel-at") && i + 1 < argc) cancel_at = argv[++i];
        else if (!strcmp(argv[i], "--abi") && i + 1 < argc)
            pkg_set_abi_override(argv[++i]);
        else if (!strcmp(argv[i], "--retry")) retry = 1;
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc)   expect = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc)  report = argv[++i];
        else if (!strcmp(argv[i], "--run-id") && i + 1 < argc)   run_id = argv[++i];
        else if (!strcmp(argv[i], "--machine") && i + 1 < argc)  machine = argv[++i];
        else if (!strcmp(argv[i], "--version")) {
            {
                char self[65];
                pkg_self_sha256(self);
                /* The last line is how PkgManager finds an apkg that can
                   finish its update: --version is the one request every
                   apkg since 0.3 answers without touching anything. */
                printf("%s\n  abi     %s\n  binary  %s\n  source  %s\n"
                       "  cc      %s\n  tc      %s\n  sdk     %s\n  tls     %s\n"
                       "  self-update protocol %d\n",
                       VERSION, pkg_abi(), self, pkg_build_id(), pkg_cc(),
                       pkg_toolchain(), pkg_sdk(), tls_name(), SU_PROTOCOL);
            }
            return 0;
        }
        else if (!strcmp(argv[i], "--about")) { printf("%s\n\n%s\n", VERSION, AROSPKG_ABOUT); return 0; }
        else if (!strcmp(argv[i], "--help-all")) { usage_all(); return 0; }
        else if (!strcmp(argv[i], "--help-testing")) { usage_testing(); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!cmd) cmd = argv[i];
        else {
            if (!arg) arg = argv[i];
            else if (!arg2) arg2 = argv[i];
            if (nrest < (int)(sizeof rest / sizeof rest[0])) rest[nrest++] = argv[i];
            else { printf("apkg: too many words on the command line\n"); return 5; }
        }
    }
    if (!cmd) { usage(); return 5; }
    /* Once, before the first line this command prints. */
    pr_init(json, plain, color);

    if (!index) {
        snprintf(indexbuf, sizeof indexbuf, "%s/db/index.json", root);
        index = indexbuf;
    }

    if (!strcmp(cmd, "assert-hash")) {
        /* A test passes when the end state is what was expected, not when it
           produced a report. This is how the harness checks that a file it
           had no right to touch is still byte-for-byte what it was. */
        char got[65];
        if (!arg || !arg2) { printf("FAIL assert-hash: need <file> <sha256>\n"); return 5; }
        if (sha256_file(arg, got) != 0) {
            printf("FAIL assert-hash %s: cannot read it\n", arg);
            return 10;
        }
        if (strcmp(got, arg2) != 0) {
            printf("FAIL assert-hash %s\n  want %s\n  got  %s\n", arg, arg2, got);
            return 10;
        }
        printf("PASS assert-hash %s\n", arg);
        return 0;
    }

    /* It replaces this program's file and nothing in the package root, so
       it never opens the root: no recovery, no lock of its own. */
    if (!strcmp(cmd, "self-update")) {
        suo.check = check;
        return apkg_self_update(VERSION + 5, root, &suo);
    }

    /* A drawer given by path needs no package root: the description travels
       with the drawer (RFC 0002), so an application unpacked by hand starts
       the same way. */
    if (!strcmp(cmd, "run") && arg && strpbrk(arg, ":/")) {
        int rc = 0;
        snprintf(doing, sizeof doing, "start %s", arg);
        doing_id = arg;
        st = run_app(NULL, arg, rest + 1, nrest - 1, check, json, &e, &rc);
        if (st != PKG_OK) { show(&e); return 10; }
        return rc;
    }

    if (!strcmp(cmd, "unlock")) {
        st = pkg_unlock(root, &e);
        if (st != PKG_OK) { show(&e); return 10; }
        printf("lock cleared. Make sure no other apkg or PkgManager is running.\n");
        return 0;
    }

    {
        pkg_mode mode = PKG_OPEN_WRITE;
        if (!strcmp(cmd, "list") || !strcmp(cmd, "info") || !strcmp(cmd, "show") ||
            !strcmp(cmd, "search") || !strcmp(cmd, "requires") ||
            !strcmp(cmd, "verify") || !strcmp(cmd, "open") || !strcmp(cmd, "outdated") ||
            !strcmp(cmd, "recent") ||
            !strcmp(cmd, "run") ||
            dry) mode = PKG_OPEN_READ;   /* a dry run never recovers */
        else if (!strcmp(cmd, "doctor")) mode = retry ? PKG_OPEN_DOCTOR : PKG_OPEN_READ;
        st = pkg_open(root, mode, &c, &e);
    }
    if (st != PKG_OK) {
        show(&e);
        /* Report before leaving. A run that could not even open the root --
           a held lock, a stale lock, recovery that stopped short -- is
           precisely the one a test needs recorded; the first stabilisation
           run lost b1.json this way and the gap was only visible because a
           report was missing from a directory listing. */
        write_report(report, run_id, machine, cmd, arg, root, stop, st, &e);
        return 10;
    }

    /* Always: the callback is also where Ctrl-C is seen. Whether it prints
       is decided as before. */
    show_progress_lines = !json && (show_progress || cancel_at || pr_wants_progress());
    pkg_set_progress(c, cli_progress, NULL);

    {
        int r = 0, u = 0, pending;
        pkg_recovery_counts(c, &r, &u);
        /* Never mixed into JSON: outdated --json carries the pending count. */
        if (json) r = 0;
        if (r) printf("recovered %d interrupted transaction(s)\n", r);
        if (!json && *pkg_replacements_finished(c))
            printf("finished an interrupted replacement: %s\n", pkg_replacements_finished(c));
        pending = json ? 0 : pkg_pending_txns(c);
        if (pending)
            printf("WARNING: %d uncommitted transaction(s) exist. What follows is\n"
                   "         what is on disk, not a settled state. A mutating\n"
                   "         command will recover them.\n", pending);
    }

    if (!strcmp(cmd, "list") && json) {
        pkg_entries es;
        st = pkg_installed(c, index, &es, &e);
        if (st == PKG_OK) print_rows(&es, 0);
        pkg_entries_free(&es);
    } else if (!strcmp(cmd, "list")) {
        char *out = NULL;
        st = pkg_list(c, &out, &e);
        if (st == PKG_OK) { if (out && *out) fputs(out, stdout); else printf("nothing installed\n"); }
        free(out);
    } else if (!strcmp(cmd, "show")) {
        if (!arg) { printf("apkg show: which package?\n"); pkg_close(c); return 5; }
        st = show_package(c, index, arg, &e);
    } else if (!strcmp(cmd, "info")) {
        char *out = NULL;
        if (!arg) { printf("apkg info: which package?\n"); pkg_close(c); return 5; }
        st = pkg_info(c, arg, &out, &e);
        if (st == PKG_OK) fputs(out, stdout);
        free(out);
    } else if (!strcmp(cmd, "update")) {
        snprintf(doing, sizeof doing, "update the package catalogue");
        cancel_noun = "Catalogue update";
        if (!json) pr_heading("Updating the package catalogue");
        st = pkg_update(c, index_url, &e);
        if (st == PKG_OK && !json) {
            pkg_entries es; int hidden = 0; pkg_err e2; char line[120];
            memset(&e2, 0, sizeof e2);
            putchar('\n');
            pr_word(PR_GREEN, "Updated");
            if (pkg_query(c, index, NULL, &es, &hidden, &e2) == PKG_OK) {
                printf(" the package catalogue: %d packages for this system\n", es.n);
                if (hidden) {
                    snprintf(line, sizeof line, "%d more are for other systems (apkg --all-abi search)", hidden);
                    pr_text(line, 2);
                }
                pkg_entries_free(&es);
            } else printf(" the package catalogue\n");
            {
                /* What the new catalogue means for what is installed: the
                   same count `apkg outdated` lists. */
                pkg_updates u; pkg_err e3; memset(&e3, 0, sizeof e3);
                if (pkg_updates_get(c, index, 1, &u, &e3) == PKG_OK) { updates_summary(&u, root); pkg_updates_free(&u); }
            }
            news_summary(c, index, root);
        }
    } else if (!strcmp(cmd, "recent")) {
        pkg_recent_opts o; int rc;
        memset(&o, 0, sizeof o);
        if (arg || limit < -1 || (days != -1 && days < 1) || (since && !news_date_ok(since))
            || (since && days != -1) || (last_update && (since || days != -1 || builds))) {
            printf("apkg recent: [--limit N] [--days N | --since YYYY-MM-DD] [--builds] [--json],\n"
                   "             or --since-last-update [--limit N] [--json]\n");
            pkg_close(c);
            return 5;
        }
        o.limit = limit >= 0 ? limit : 10;
        o.builds = builds;
        if (since) snprintf(o.since, sizeof o.since, "%s", since);
        else if (days > 0) pkg_days_ago(days - 1, o.since);
        rc = recent(c, index, root, &o, last_update, limit > 0 ? limit : 0, json);
        pkg_close(c);
        return rc;
    } else if (!strcmp(cmd, "outdated")) {
        int rc;
        if (arg) { printf("apkg outdated: it takes no package; it lists all of them\n"); pkg_close(c); return 5; }
        rc = outdated(c, index, root, all, json);
        pkg_close(c);
        return rc;
    } else if (!strcmp(cmd, "upgrade") && all && !arg) {
        int failed = 0, stopped = 0, done;
        shown = 1;      /* each package's error was shown as it happened */
        if (dry) {
            /* The plan of each, from the same dry run `apkg --dry-run upgrade <id>` prints. */
            pkg_updates u; int k, any = 0;
            st = pkg_updates_get(c, index, 1, &u, &e);
            if (st == PKG_OK) {
                for (k = 0; k < u.n; k++) {
                    char *out = NULL; pkg_err e4; memset(&e4, 0, sizeof e4);
                    if (u.rows[k].state != PKG_UPD_AVAILABLE) continue;
                    any = 1;
                    if (pkg_dry_run(c, index, "upgrade", u.rows[k].id, fetch, &out, &e4) == PKG_OK) fputs(out, stdout);
                    else { show(&e4); failed++; st = e4.status; e = e4; }
                    free(out);
                }
                if (!any) printf("nothing can be upgraded now\n");
                pkg_updates_free(&u);
            } else show(&e);
            group_rc = st != PKG_OK || failed ? 10 : 0;
        } else {
            /* Falls through to the common end, for the late Ctrl-C notice
               and --report; the status is the last failure, if any. */
            done = upgrade_all(c, index, root, stop, &failed, &stopped, &st);
            if (st == PKG_OK && stopped) st = PKG_E_CANCELLED;
            group_rc = done < 0 || failed || stopped ? 10 : 0;
        }
    } else if (!strcmp(cmd, "search") && json) {
        pkg_entries es; int hidden = 0;
        st = pkg_query(c, index, arg, &es, &hidden, &e);
        if (st == PKG_OK) print_rows(&es, hidden);
        pkg_entries_free(&es);
    } else if (!strcmp(cmd, "search") && pr_rich()) {
        pkg_entries es; int hidden = 0;
        st = pkg_query(c, index, arg, &es, &hidden, &e);
        if (st == PKG_OK) search_table(&es, hidden);
        pkg_entries_free(&es);
    } else if (!strcmp(cmd, "search")) {
        char *out = NULL;
        st = pkg_search(c, index, arg, &out, &e);
        if (st == PKG_OK) fputs(out, stdout);
        free(out);
    } else if (dry && (!strcmp(cmd, "install") || !strcmp(cmd, "remove") ||
                       !strcmp(cmd, "upgrade") || !strcmp(cmd, "rollback"))) {
        char *out = NULL;
        if (!arg) { printf("apkg %s: which package?\n", cmd); pkg_close(c); return 5; }
        if (!strcmp(cmd, "install")) {
            /* The relations plan, as install would make it; then the files
               of the package itself. */
            pkg_install_plan *pl = (pkg_install_plan *)calloc(1, sizeof *pl);
            if (pl && pkg_plan_install(c, index, arg, pl, &e) == PKG_OK) {
                if (json) {
                    char *js = pkg_install_plan_json(pl);
                    if (js) fputs(js, stdout);
                    free(js);
                    rc_dry = pl->ok ? 0 : 10;
                    free(pl); pkg_close(c); return rc_dry;
                }
                if (!pl->ok) { pr_text(pl->why, 0); free(pl); pkg_close(c); return 10; }
                if (pl->extra) print_plan(pl);
            }
            free(pl);
        }
        st = pkg_dry_run(c, index, cmd, arg, fetch, &out, &e);
        if (st == PKG_OK) fputs(out, stdout);
        free(out);
    } else if (!strcmp(cmd, "verify")) {
        pkg_verify_result v; char *listing = NULL;
        if (!arg) { printf("apkg verify: which package?\n"); pkg_close(c); return 5; }
        st = pkg_verify(c, arg, &v, &listing, &e);
        if (st == PKG_OK) {
            if (json)
                printf("{ \"id\": \"%s\", \"ok\": %d, \"changed\": %d, \"missing\": %d, \"unknown\": %d, \"icon\": \"%s\" }\n",
                       arg, v.ok, v.changed, v.missing, v.unknown, v.icon);
            else {
                printf("%s: %d as installed, %d changed locally, %d missing, %d not the package's; drawer icon %s\n",
                       arg, v.ok, v.changed, v.missing, v.unknown, v.icon);
                if (listing && *listing) fputs(listing, stdout);
            }
        }
        free(listing);
    } else if (!strcmp(cmd, "requires")) {
        char out[2048];
        if (!arg) { printf("apkg requires: which package?\n"); pkg_close(c); return 5; }
        st = pkg_requirements(c, index, arg, out, sizeof out, &e);
        if (st == PKG_OK) printf("%s\n", out[0] ? out : "no system requirements");
    } else if (!strcmp(cmd, "doctor")) {
        char *out = NULL;
        st = pkg_doctor(c, &out, &e);
        if (st == PKG_OK) fputs(out, stdout);
        free(out);
    } else if (!strcmp(cmd, "install")) {
        char v[80];
        if (!arg) { printf("apkg install: which package?\n"); pkg_close(c); return 5; }
        snprintf(doing, sizeof doing, "install %s", arg);
        doing_id = arg;
        cancel_noun = "Installation";
        {
            /* The plan first: what it needs, what it replaces, what it
               refuses. Only a plan beyond the package itself is asked about. */
            pkg_install_plan *pl = (pkg_install_plan *)calloc(1, sizeof *pl);
            int i;
            if (!pl) { printf("out of memory\n"); pkg_close(c); return 10; }
            st = pkg_plan_install(c, index, arg, pl, &e);
            if (st == PKG_OK && !pl->ok) {
                e.status = st = pl->status ? pl->status : PKG_E_REQUIRES;
                snprintf(e.summary, sizeof e.summary, "cannot install it");
                snprintf(e.detail, sizeof e.detail, "%s", pl->why);
                snprintf(e.subject, sizeof e.subject, "%s", arg);
            }
            if (st == PKG_OK && json) {
                char *js;
                if (pl->extra && !assume_yes) {
                    js = pkg_install_plan_json(pl); if (js) fputs(js, stdout); free(js);
                    free(pl); pkg_close(c); return 5;
                }
                st = pkg_install_plan_run(c, index, pl, stop, &e);
                js = pkg_install_plan_json(pl); if (js) fputs(js, stdout); free(js);
                free(pl); pkg_close(c);
                return st == PKG_OK ? 0 : 10;
            }
            if (st == PKG_OK && (pl->extra || pl->ops[0].kind == PKG_OP_MARK_EXPLICIT)) print_plan(pl);
            if (st == PKG_OK && pl->extra && !plan_confirmed()) { free(pl); pkg_close(c); return 5; }
            if (st == PKG_OK) {
                offered(c, index, arg, v, sizeof v, 0);
                /* With a plan shown, its steps say what is installed; the
                   progress lines name each package. */
                if (pl->ops[0].kind != PKG_OP_MARK_EXPLICIT && !pl->extra) heading2("Installing", arg, v);
                st = pkg_install_plan_run(c, index, pl, stop, &e);
                pr_end_line();
                if (st == PKG_OK) {
                    for (i = 0; i < pl->nops; i++) {
                        const pkg_plan_op *o = &pl->ops[i];
                        if (o->kind == PKG_OP_DEPENDENCY) print_installed(c, index, o->id, "Installed (needed)");
                        else if (o->kind == PKG_OP_MARK_EXPLICIT)
                            { char l[200]; snprintf(l, sizeof l, "%s was installed as a dependency; it is now recorded as installed by you.", o->id); pr_text(l, 0); }
                    }
                    if (pl->ops[pl->nops - 1].kind != PKG_OP_MARK_EXPLICIT || pl->nops > 1)
                        for (i = 0; i < pl->nops; i++) if (pl->ops[i].kind == PKG_OP_INSTALL) print_installed(c, index, arg, "Installed");
                    for (i = 0; i < pl->nops; i++)
                        if (pl->ops[i].kind == PKG_OP_REMOVE_REPLACED) {
                            char l[400], d[PKG_MAXPATH]; pkg_removal rm;
                            int last = 1, k2;
                            for (k2 = i + 1; k2 < pl->nops; k2++) if (pl->ops[k2].kind == PKG_OP_REMOVE_REPLACED) last = 0;
                            /* Only the last removal's record is kept; for the
                               others, where their files would be. */
                            pkg_last_removal(c, &rm);
                            snprintf(d, sizeof d, "%s", last ? rm.dir : "");
                            snprintf(l, sizeof l, "Removed %s, which %s replaces. Files you changed or added in it "
                                     "stay in its drawer%s%s.", pl->ops[i].id, arg, d[0] ? ": " : "", d);
                            pr_text(l, 0);
                        }
                } else if (pl->extra) print_plan_result(pl);
            }
            free(pl);
        }
    } else if (!strcmp(cmd, "remove")) {
        char v[80];
        pkg_removal rm;
        if (!arg) { printf("apkg remove: which package?\n"); pkg_close(c); return 5; }
        snprintf(doing, sizeof doing, "remove %s", arg);
        doing_id = arg;
        cancel_noun = "Removal";
        offered(c, index, arg, v, sizeof v, 1);
        heading2("Removing", arg, v);
        st = pkg_remove(c, arg, stop, &e);
        pr_end_line();
        if (st == PKG_OK) {
            int i;
            char line[PKG_MAXPATH + 120];
            pkg_last_removal(c, &rm);
            putchar('\n');
            pr_word(PR_GREEN, "Removed"); printf(" %s%s%s\n", arg, *v ? " " : "", v);
            /* What is still there is said as such: a user who reads only
               "Removed" would think it gone. */
            /* What is still there, file by file with the reason: what the
               user changed, and what apkg never installed. Only names apkg
               knows are listed; a longer list says how many more. */
            if (rm.kept || rm.others) {
                snprintf(line, sizeof line, "Kept files in %s:", rm.dir[0] ? rm.dir : "the drawer");
                pr_text(line, 2);
                for (i = 0; i < rm.named; i++) {
                    snprintf(line, sizeof line, "%s (you changed it)", rm.names[i]);
                    pr_text(line, 4);
                }
                if (rm.kept > rm.named) {
                    snprintf(line, sizeof line, "and %d more you changed, listed in %s", rm.kept - rm.named, rm.list_file);
                    pr_text(line, 4);
                }
                for (i = 0; i < rm.others_named; i++) {
                    snprintf(line, sizeof line, "%s (not installed by apkg)", rm.other_names[i]);
                    pr_text(line, 4);
                }
                if (rm.others > rm.others_named) {
                    snprintf(line, sizeof line, "and %d more not installed by apkg", rm.others - rm.others_named);
                    pr_text(line, 4);
                }
            } else if (rm.dir[0]) {
                snprintf(line, sizeof line, "The drawer %s was kept; it is not empty.", rm.dir);
                pr_text(line, 2);
            }
            if (rm.icon[0]) {
                snprintf(line, sizeof line, "Kept the drawer icon %s: it was changed after installation", rm.icon);
                pr_text(line, 2);
            }
        }
    } else if (!strcmp(cmd, "upgrade")) {
        if (!arg) { printf("apkg upgrade: which package?\n"); pkg_close(c); return 5; }
        snprintf(doing, sizeof doing, "upgrade %s", arg);
        doing_id = arg;
        cancel_noun = "Upgrade";
        st = pkg_upgrade(c, index, arg, stop, &e);
        pr_end_line();
        if (st == PKG_OK) print_installed(c, index, arg, "Upgraded");
    } else if (!strcmp(cmd, "rollback")) {
        if (!arg) { printf("apkg rollback: which package?\n"); pkg_close(c); return 5; }
        snprintf(doing, sizeof doing, "roll back %s", arg);
        doing_id = arg;
        cancel_noun = "Rollback";
        st = pkg_rollback(c, arg, stop, &e);
        pr_end_line();
        if (st == PKG_OK) print_installed(c, index, arg, "Rolled back");
    } else if (!strcmp(cmd, "open")) {
        char where[PKG_MAXPATH];
        if (!arg) { printf("apkg open: which package?\n"); pkg_close(c); return 5; }
        snprintf(doing, sizeof doing, "open %s", arg);
        doing_id = arg;
        cancel_noun = "Opening";
        st = pkg_open_folder(c, arg, where, sizeof where, &e);
        if (st == PKG_OK) printf("Opened %s\n", where);
    } else if (!strcmp(cmd, "run")) {
        if (!arg) { printf("apkg run: which application?\n"); pkg_close(c); return 5; }
        snprintf(doing, sizeof doing, "start %s", arg);
        doing_id = arg;
        st = run_app(c, arg, rest + 1, nrest - 1, check, json, &e, &group_rc);
        if (st != PKG_OK) group_rc = -1;
    } else {
        printf("apkg: unknown command '%s'\n", cmd);
        pkg_close(c);
        return 5;
    }

    if (st != PKG_OK && !shown) show(&e);
    if (stop_requested_late)
        pr_text("Ctrl-C came after files were already being changed; the operation was finished "
                "rather than left half done.", 0);

    write_report(report, run_id, machine, cmd, arg, root, stop, st, &e);

    if (expect >= 0) {
        if ((int)st == expect) {
            printf("PASS %s: %s as expected\n", run_id ? run_id : cmd, pkg_strstatus(st));
        } else {
            printf("FAIL %s: expected %s, got %s\n", run_id ? run_id : cmd,
                   pkg_strstatus((pkg_status)expect), pkg_strstatus(st));
            pkg_close(c);
            return 20;
        }
    }

    pkg_close(c);
    if (group_rc >= 0) return group_rc;
    return st == PKG_OK ? 0 : 10;
}

int main(int argc, char **argv)
{
    int rc = pkg_run_with_stack(real_main, argc, argv);
    /* Not optional. On mainline before da9bbd01b5 (2026-09-24, so the pool's
       v1 base and the Raspberry Pi image of 2026-08-22), output still in the
       stdio buffer at exit is discarded when stdout is redirected: measured
       with tests/stdout-probe on pool v1-1, fputs/fwrite/putchar lost, printf
       and puts kept, fputs followed by fflush kept. `apkg search >file` and
       `apkg outdated --json >file` came out empty that way, because each
       prints its answer with one fputs. */
    fflush(stdout);
    return rc;
}
