/* PkgManager's ARexx parser and replies, on the host. What the port does on
 * AROS is checked there; this covers the part that is plain C. */
#include "../src/pkgmanager/rexxcmd.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int parse(const char *line, rx_req *r, char *err)
{
    return rx_parse(line, r, err, 200);
}

static void ok(const char *line, rx_cmd cmd, const char *pkg)
{
    rx_req r; char err[200];
    int rc = parse(line, &r, err);
    if (rc != 0 || r.cmd != cmd || strcmp(r.package, pkg)) {
        printf("FAIL parse [%s]: rc=%d cmd=%d pkg=[%s] err=[%s]\n", line, rc, r.cmd, r.package, err);
        fails++;
    }
}

static void bad(const char *line, const char *prefix)
{
    rx_req r; char err[200];
    int rc = parse(line, &r, err);
    if (rc != RX_RC_FAIL || strncmp(err, prefix, strlen(prefix))) {
        printf("FAIL refuse [%s]: rc=%d err=[%s], wanted %s\n", line, rc, err, prefix);
        fails++;
    }
}

int main(void)
{
    rx_req r; char err[200], out[4096];

    /* commands, any case, whole names only */
    ok("VERSION", RXC_VERSION, "");
    ok("version", RXC_VERSION, "");
    ok("  Install soliton", RXC_INSTALL, "soliton");
    ok("INSTALL\tsoliton  ", RXC_INSTALL, "soliton");
    ok("REMOVE soliton", RXC_REMOVE, "soliton");
    ok("UPGRADE soliton", RXC_UPGRADE, "soliton");
    ok("ROLLBACK soliton", RXC_ROLLBACK, "soliton");
    ok("SHOW soliton", RXC_SHOW, "soliton");
    ok("REQUIREMENTS soliton", RXC_REQUIREMENTS, "soliton");
    ok("INSTALL PACKAGE=soliton", RXC_INSTALL, "soliton");
    ok("INSTALL PACKAGE soliton", RXC_INSTALL, "soliton");
    ok("UPDATE", RXC_UPDATE, "");
    ok("QUIT", RXC_QUIT, "");
    ok("LastError", RXC_LASTERROR, "");
    bad("LASTERROR now", "BADARGS LASTERROR: unexpected argument");
    bad("IN soliton", "BADCOMMAND");
    bad("INSTALLX soliton", "BADCOMMAND");
    bad("", "BADCOMMAND");
    bad("   ", "BADCOMMAND");
    bad("FROB", "BADCOMMAND unknown command \"FROB\"");

    /* quoting: spaces, quotes and AmigaDOS escapes inside */
    ok("INSTALL \"sol iton\"", RXC_INSTALL, "sol iton");
    ok("INSTALL \"a*\"b\"", RXC_INSTALL, "a\"b");
    ok("INSTALL \"a**b\"", RXC_INSTALL, "a*b");
    ok("INSTALL PACKAGE=\"x y\"", RXC_INSTALL, "x y");
    ok("INSTALL 'soliton'", RXC_INSTALL, "'soliton'");   /* ARexx quotes are the script's, not ours */
    ok("INSTALL so;li$ton", RXC_INSTALL, "so;li$ton");
    ok("INSTALL \"FIELD\"", RXC_INSTALL, "FIELD");         /* quoted: never a keyword */
    bad("INSTALL \"soliton", "BADARGS unterminated quote");
    bad("INSTALL \"sol\"iton", "BADARGS text after a closing quote");
    bad("INSTALL \"\"", "BADARGS PACKAGE is empty");
    bad("INSTALL \"a*nb\"", "BADARGS PACKAGE contains a control character");
    bad("INSTALL a\x01" "b", "BADARGS PACKAGE contains a control character");

    /* arguments */
    bad("INSTALL", "BADARGS INSTALL needs PACKAGE");
    bad("INSTALL soliton extra", "BADARGS INSTALL: unexpected argument \"extra\"");
    bad("INSTALL soliton FIELD version", "BADARGS INSTALL: unexpected argument \"FIELD\"");
    bad("INSTALL FOO=bar", "BADARGS INSTALL does not take FOO");
    bad("INSTALL PACKAGE=a PACKAGE=b", "BADARGS PACKAGE given twice");
    bad("INSTALL 0123456789012345678901234567890123456789012345678901234567890123", "BADARGS PACKAGE is longer than 63");
    ok("INSTALL 012345678901234567890123456789012345678901234567890123456789012", RXC_INSTALL,
       "012345678901234567890123456789012345678901234567890123456789012");
    bad("VERSION now", "BADARGS VERSION: unexpected argument");

    CHECK(parse("INFO soliton FIELD version", &r, err) == 0 && !strcmp(r.field, "version"));
    CHECK(parse("INFO FIELD=state soliton", &r, err) == 0 && !strcmp(r.field, "state") && !strcmp(r.package, "soliton"));
    CHECK(parse("info soliton field STATE", &r, err) == 0 && !strcmp(r.field, "STATE"));
    bad("INFO soliton FIELD", "BADARGS FIELD needs a value");
    bad("INFO FIELD state", "BADARGS INFO needs PACKAGE");

    CHECK(parse("LIST", &r, err) == 0 && r.cmd == RXC_LIST && !r.installed && !r.match[0]);
    CHECK(parse("LIST INSTALLED", &r, err) == 0 && r.installed);
    CHECK(parse("LIST MATCH \"card game\"", &r, err) == 0 && !strcmp(r.match, "card game"));
    CHECK(parse("LIST installed MATCH=sol", &r, err) == 0 && r.installed && !strcmp(r.match, "sol"));
    bad("LIST INSTALLED=1", "BADARGS INSTALLED takes no value");
    bad("LIST soliton", "BADARGS LIST: unexpected argument");

    CHECK(parse("JOB", &r, err) == 0 && r.cmd == RXC_JOB && r.job == 0);
    CHECK(parse("JOB 12", &r, err) == 0 && r.job == 12);
    CHECK(parse("JOB ID=3 FIELD state", &r, err) == 0 && r.job == 3 && !strcmp(r.field, "state"));
    CHECK(parse("WAIT 4", &r, err) == 0 && r.cmd == RXC_WAIT && r.job == 4);
    CHECK(parse("CANCEL 7", &r, err) == 0 && r.cmd == RXC_CANCEL && r.job == 7);
    bad("JOB x", "BADARGS ID must be a job number");
    bad("JOB 0", "BADARGS ID must be a job number");
    bad("JOB -1", "BADARGS ID must be a job number");
    bad("JOB 3x", "BADARGS ID must be a job number");
    bad("CANCEL 1 2", "BADARGS CANCEL: unexpected argument");

    CHECK(!strcmp(rx_help(), "VERSION HELP LIST INFO REQUIREMENTS SHOW UPDATE INSTALL REMOVE UPGRADE ROLLBACK JOB WAIT CANCEL QUIT LASTERROR OUTDATED RUN VERIFY"));

    /* replies: every field as "name value" lines, or one bare value */
    {
        pkg_entry e;
        memset(&e, 0, sizeof e);
        strcpy(e.id, "soliton"); strcpy(e.version, "2.2"); e.revision = 2;
        strcpy(e.summary, "Klondike\nand Freecell"); strcpy(e.category, "game/card");
        strcpy(e.arch, "x86_64"); strcpy(e.abi, "v11"); strcpy(e.kind, "app");
        e.size = 223945; strcpy(e.requires, "crt.library,m.library"); e.ours = 1;
        CHECK(rx_format_entry(&e, 0, 0, "native", "satisfied", NULL, out, sizeof out) == 0);
        CHECK(!strcmp(out,
            "id soliton\nversion 2.2\nrevision 2\nstate available\ninstalledversion \n"
            "installedrevision \nsummary Klondike and Freecell\ncategory game/card\n"
            "arch x86_64\nabi v11\nkind app\nsize 223945\nrequires crt.library,m.library\n"
            "canupgrade 0\ncanrollback 0\ncompatibility native\nrequirements satisfied"));
        e.installed = 1; strcpy(e.installed_version, "2.2"); e.installed_revision = 1;
        CHECK(rx_format_entry(&e, 1, 0, "native", "satisfied", "STATE", out, sizeof out) == 0 && !strcmp(out, "installed"));
        CHECK(rx_format_entry(&e, 1, 0, "native", "satisfied", "installedrevision", out, sizeof out) == 0 && !strcmp(out, "1"));
        CHECK(rx_format_entry(&e, 1, 0, "native", "satisfied", "canupgrade", out, sizeof out) == 0 && !strcmp(out, "1"));
        CHECK(rx_format_entry(&e, 1, 0, "native", "satisfied", "summary", out, sizeof out) == 0 && !strcmp(out, "Klondike and Freecell"));
        CHECK(rx_format_entry(&e, 1, 0, "native", "satisfied", "colour", out, sizeof out) == -1);
        CHECK(rx_format_entry(&e, 1, 0, "native", "satisfied", "compatibility", out, sizeof out) == 0 && !strcmp(out, "native"));
        CHECK(rx_format_entry(&e, 1, 0, "native", "missing", "requirements", out, sizeof out) == 0 && !strcmp(out, "missing"));
        e.installed = 0; e.ours = 0;
        CHECK(rx_format_entry(&e, 0, 0, "incompatible", "satisfied", "state", out, sizeof out) == 0 && !strcmp(out, "otherabi"));
        CHECK(rx_format_entry(&e, 0, 0, "native", "satisfied", NULL, out, 40) == -2);      /* does not fit */
    }
    {
        rx_job j;
        memset(&j, 0, sizeof j);
        j.id = 3; strcpy(j.op, "install"); strcpy(j.package, "soliton"); strcpy(j.origin, "arexx");
        j.state = RXJ_RUNNING; strcpy(j.phase, "download"); j.done = 16384; j.total = 223945; j.can_cancel = 1;
        CHECK(rx_format_job(&j, NULL, out, sizeof out) == 0);
        CHECK(!strcmp(out, "id 3\nop install\npackage soliton\norigin arexx\nstate running\nphase download\n"
                           "done 16384\ntotal 223945\ncancancel 1\nerror \nmessage "));
        CHECK(rx_format_job(&j, "state", out, sizeof out) == 0 && !strcmp(out, "running"));
        j.state = RXJ_FAILED; strcpy(j.error, "LOCKED"); strcpy(j.message, "the package root is in use");
        CHECK(rx_format_job(&j, "cancancel", out, sizeof out) == 0 && !strcmp(out, "0"));  /* never on a final job */
        CHECK(rx_format_job(&j, "error", out, sizeof out) == 0 && !strcmp(out, "LOCKED"));
        CHECK(rx_format_job(&j, "nope", out, sizeof out) == -1);
        CHECK(!strcmp(rx_statename(RXJ_CONFIRM), "confirm") && !strcmp(rx_statename(RXJ_DECLINED), "declined"));
        CHECK(rx_final(RXJ_DONE) && rx_final(RXJ_DECLINED) && !rx_final(RXJ_CONFIRM) && !rx_final(RXJ_PREVIEW));
    }
    CHECK(!strcmp(rx_statusword(PKG_E_LOCKED), "LOCKED"));
    CHECK(!strcmp(rx_statusword(PKG_E_CANCELLED), "CANCELLED"));
    CHECK(!strcmp(rx_statusword(PKG_E_REQUIRES), "REQUIRES"));
    CHECK(!strcmp(rx_statusword(PKG_OK), "OK"));

    {
        rx_req r; char err[200], out[512];
        pkg_update_row rows[2]; pkg_updates up;
        CHECK(rx_parse("OUTDATED", &r, err, sizeof err) == 0 && r.cmd == RXC_OUTDATED && !r.show);
        CHECK(rx_parse("outdated SHOW", &r, err, sizeof err) == 0 && r.show);
        CHECK(rx_parse("OUTDATED SHOW=1", &r, err, sizeof err) == RX_RC_FAIL);
        CHECK(rx_parse("OUTDATED folio", &r, err, sizeof err) == RX_RC_FAIL);
        memset(&up, 0, sizeof up); memset(rows, 0, sizeof rows);
        snprintf(rows[0].id, sizeof rows[0].id, "folio"); rows[0].newer = 1;
        snprintf(rows[1].id, sizeof rows[1].id, "soliton");
        up.rows = rows; up.n = 2; up.installed = 2; up.newer = 1; up.ready = 1;
        snprintf(up.catalogue_when, sizeof up.catalogue_when, "2026-10-11 10:25");
        CHECK(rx_format_updates(&up, out, sizeof out) == 0);
        CHECK(!strcmp(out, "catalogue ok\nupdated 2026-10-11 10:25\nstale 0\ninstalled 2\nnewer 1\nready 1\nblocked 0\npackages folio"));
        up.newer = 0; rows[0].newer = 0; up.catalogue = PKG_CAT_MISSING;
        CHECK(rx_format_updates(&up, out, sizeof out) == 0 && strstr(out, "catalogue missing") && strstr(out, "\npackages") && out[strlen(out) - 1] == 's');
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("rexxcmd: all checks pass\n");
    return 0;
}
