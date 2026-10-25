/* The application description's rules (src/libpkg/app.c), on the host.
 * Starting a program is AROS-only and is tested on a guest; what is decided
 * before anything is started -- which paths are acceptable, what a launch
 * table means, what a program file is built for -- is decided here. */
#include "../src/libpkg/app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static int parse_launch(const char *js, pkg_launch *l, char *why, size_t n)
{
    js_tok *t = NULL;
    int ntok = js_parse_alloc(js, strlen(js), &t, 256), r;
    if (ntok <= 0) { free(t); return -2; }
    r = launch_from_object(js, t, ntok, js_member(js, t, ntok, 0, "launch"), l, why, n);
    free(t);
    return r;
}

static void paths(void)
{
    char why[200];
    CHECK(app_path_ok("Folio", why, sizeof why) == 0, "plain name");
    CHECK(app_path_ok("bin/Folio", why, sizeof why) == 0, "subdirectory");
    CHECK(app_path_ok("My Program", why, sizeof why) == 0, "a space is allowed");
    CHECK(app_path_ok(".", why, sizeof why) == 0, "the drawer itself");
    CHECK(app_path_ok("", why, sizeof why) != 0, "empty");
    CHECK(app_path_ok("/Folio", why, sizeof why) != 0, "absolute");
    CHECK(app_path_ok("SYS:C/Dir", why, sizeof why) != 0, "a volume");
    CHECK(app_path_ok("../Other/Prog", why, sizeof why) != 0, "leaves the drawer");
    CHECK(app_path_ok("bin/../../x", why, sizeof why) != 0, "leaves through a subdirectory");
    CHECK(app_path_ok("bin//x", why, sizeof why) != 0, "empty component");
    CHECK(app_path_ok("bin/", why, sizeof why) != 0, "trailing slash");
    CHECK(app_path_ok("./Folio", why, sizeof why) != 0, "a '.' component");
    CHECK(app_path_ok("bin\\x", why, sizeof why) != 0, "backslash");
    CHECK(app_path_ok("a\nb", why, sizeof why) != 0, "control character");
    CHECK(app_path_ok("a\"b", why, sizeof why) != 0, "quote");
    CHECK(app_path_ok("caf\xc3\xa9", why, sizeof why) != 0, "non-ASCII");
}

static void launch_tables(void)
{
    pkg_launch l;
    char why[200];

    CHECK(parse_launch("{\"launch\":{\"executable\":\"Folio\",\"mode\":\"workbench\"}}", &l, why, sizeof why) == 1,
          "minimal workbench table");
    CHECK(l.mode == PKG_LAUNCH_WORKBENCH && !strcmp(l.workdir, "."), "workbench default directory is the drawer");

    CHECK(parse_launch("{\"launch\":{\"executable\":\"bin/Tool\",\"mode\":\"shell\"}}", &l, why, sizeof why) == 1,
          "shell table");
    CHECK(!strcmp(l.workdir, "bin"), "the default directory is the program's drawer");

    CHECK(parse_launch("{\"launch\":{\"executable\":\"My Tool\",\"mode\":\"shell\",\"working_directory\":\"Data Files\"}}",
                       &l, why, sizeof why) == 1, "spaces in both paths");
    CHECK(!strcmp(l.executable, "My Tool") && !strcmp(l.workdir, "Data Files"), "spaces kept as written");

    CHECK(parse_launch("{\"x\":1}", &l, why, sizeof why) == 0 && l.mode == PKG_LAUNCH_NONE, "no table is none, not an error");

    CHECK(parse_launch("{\"launch\":{\"mode\":\"workbench\"}}", &l, why, sizeof why) == -1, "missing program");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"F\"}}", &l, why, sizeof why) == -1, "missing mode");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"F\",\"mode\":\"cli\"}}", &l, why, sizeof why) == -1, "unknown mode");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"F\",\"mode\":\"shell\",\"args\":\"-x\"}}", &l, why, sizeof why) == -1,
          "unknown key refused, not ignored");
    CHECK(strstr(why, "args") != NULL, "the unknown key is named");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"F\",\"mode\":\"shell\",\"mode\":\"workbench\"}}", &l, why, sizeof why) == -1,
          "duplicate key");
    CHECK(parse_launch("{\"launch\":{\"executable\":7,\"mode\":\"shell\"}}", &l, why, sizeof why) == -1, "wrong type");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"SYS:C/List\",\"mode\":\"shell\"}}", &l, why, sizeof why) == -1,
          "a program outside the drawer");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"../x\",\"mode\":\"shell\"}}", &l, why, sizeof why) == -1, "'..'");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"Folio.info\",\"mode\":\"workbench\"}}", &l, why, sizeof why) == -1,
          "the icon named instead of the program");
    CHECK(parse_launch("{\"launch\":{\"executable\":\".\",\"mode\":\"shell\"}}", &l, why, sizeof why) == -1, "the drawer as program");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"F\",\"mode\":\"workbench\",\"working_directory\":\"Data\"}}",
                       &l, why, sizeof why) == -1, "workbench cannot honour another directory");
    CHECK(parse_launch("{\"launch\":{\"executable\":\"bin/F\",\"mode\":\"workbench\",\"working_directory\":\"bin\"}}",
                       &l, why, sizeof why) == 1, "workbench with its own drawer stated");
    CHECK(parse_launch("{\"launch\":[1]}", &l, why, sizeof why) == -1, "not a table");
    CHECK(parse_launch("{\"launch\":{\"working_directory\":\"executable\",\"executable\":\"bin/Prog\",\"mode\":\"shell\"}}",
                       &l, why, sizeof why) == -1 || !strcmp(l.executable, "bin/Prog"),
          "a value equal to a key name is not taken for that key");
}

static void descriptions(void)
{
    pkg_launch l = { PKG_LAUNCH_WORKBENCH, "Folio", "." };
    req_ent reqs[2], back[MAXREQ];
    app_desc d;
    char why[200], *txt;
    int nreq = 0;

    memset(reqs, 0, sizeof reqs);
    snprintf(reqs[0].type, sizeof reqs[0].type, "library");
    snprintf(reqs[0].id, sizeof reqs[0].id, "muimaster.library");
    snprintf(reqs[1].type, sizeof reqs[1].type, "library");
    snprintf(reqs[1].id, sizeof reqs[1].id, "crt.library");
    reqs[1].min_version = 3;

    txt = app_text("folio", "0.4.1", 2, "x86_64", "v11", reqs, 2, &l);
    CHECK(txt != NULL, "written");
    CHECK(app_parse(txt, strlen(txt), &d, back, MAXREQ, &nreq, why, sizeof why) == 0, "what is written reads back");
    CHECK(!strcmp(d.id, "folio") && !strcmp(d.version, "0.4.1") && d.revision == 2, "identity");
    CHECK(!strcmp(d.arch, "x86_64") && !strcmp(d.abi, "v11"), "target");
    CHECK(!strcmp(d.name, "folio"), "the name falls back to the id");
    CHECK(nreq == 2 && !strcmp(back[1].id, "crt.library") && back[1].min_version == 3, "requirements");
    CHECK(d.launch.mode == PKG_LAUNCH_WORKBENCH && !strcmp(d.launch.executable, "Folio"), "launch");
    free(txt);

    {
        const char *bad[] = {
            "{\"schema\":2,\"id\":\"a\",\"version\":\"1\",\"targets\":[{\"arch\":\"x86_64\",\"abi\":\"v11\"}],"
            "\"launch\":{\"executable\":\"a\",\"mode\":\"shell\"}}",
            "{\"schema\":1,\"version\":\"1\",\"targets\":[{\"arch\":\"x86_64\",\"abi\":\"v11\"}],"
            "\"launch\":{\"executable\":\"a\",\"mode\":\"shell\"}}",
            "{\"schema\":1,\"id\":\"a\",\"version\":\"1\",\"targets\":[],"
            "\"launch\":{\"executable\":\"a\",\"mode\":\"shell\"}}",
            "{\"schema\":1,\"id\":\"a\",\"version\":\"1\",\"targets\":[{\"arch\":\"x86_64\",\"abi\":\"v11\"},"
            "{\"arch\":\"i386\",\"abi\":\"v0\"}],\"launch\":{\"executable\":\"a\",\"mode\":\"shell\"}}",
            "{\"schema\":1,\"id\":\"a\",\"version\":\"1\",\"targets\":[{\"arch\":\"x86_64\",\"abi\":\"v11\"}]}",
            "not json",
        };
        const char *what[] = { "unsupported schema", "no id", "no target", "several targets",
                               "no launch table", "not JSON" };
        size_t i;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
            CHECK(app_parse(bad[i], strlen(bad[i]), &d, back, MAXREQ, &nreq, why, sizeof why) != 0, what[i]);
    }
}

static void binaries(void)
{
    unsigned char h[64];
    memset(h, 0, sizeof h);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = 2; h[5] = 1;
    h[18] = 62;   CHECK(!strcmp(app_binary_arch(h, sizeof h), "x86_64"), "x86_64 ELF");
    h[4] = 1; h[18] = 3; CHECK(!strcmp(app_binary_arch(h, sizeof h), "i386"), "i386 ELF");
    h[4] = 2; h[18] = 183; CHECK(!strcmp(app_binary_arch(h, sizeof h), "aarch64"), "aarch64 ELF");
    h[5] = 2; h[18] = 0; h[19] = 183; CHECK(!strcmp(app_binary_arch(h, sizeof h), "aarch64"), "big-endian aarch64 ELF");
    memset(h, 0, sizeof h); h[2] = 0x03; h[3] = 0xf3;
    CHECK(!strcmp(app_binary_arch(h, sizeof h), "m68k"), "Amiga hunk file");
    memcpy(h, "echo hi\n", 8);
    CHECK(!strcmp(app_binary_arch(h, sizeof h), ""), "a script is not a binary");
    CHECK(!strcmp(app_binary_arch(h, 3), ""), "too short to tell");
}

int main(void)
{
    paths();
    launch_tables();
    descriptions();
    binaries();
    if (fails) { printf("test_app: %d failure(s)\n", fails); return 1; }
    printf("APP: paths, launch tables, descriptions and program files decided as documented\n");
    return 0;
}
