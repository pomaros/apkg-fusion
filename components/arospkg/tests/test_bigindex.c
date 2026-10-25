/* The JSON reader on generated catalogues larger than the old fixed token
 * array (8192), and its failures: malformed text, too many values, no memory.
 *
 *   test_bigindex INDEX.json [EXPECTED_ROWS]   time and check a generated index
 *   test_bigindex INDEX.json -                 time any index
 *   test_bigindex --errors                     the failure cases
 *
 * json.c and entries.c are built with malloc renamed to t_malloc, so a test
 * can make the Nth allocation fail.
 */
#include "../src/libpkg/entries.h"
#include "../src/libpkg/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

static long fail_at = -1, calls;
void *t_malloc(size_t n)
{
    if (fail_at >= 0 && calls++ == fail_at) return NULL;
    return malloc(n);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static char *slurp(const char *p, size_t *len)
{
    FILE *f = fopen(p, "rb"); char *b; long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); b[n] = 0; *len = (size_t)n;
    return b;
}

static double now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int errors(void)
{
    static const char *bad[] = { "{\"packages\": [", "{\"packages\": [}", "]", "{\"a\": \"x}",
                                 "{\"packages\": [ {\"id\": \"a\"} ]]" };
    const char *ok = "{\"packages\": [ {\"id\": \"a\", \"arch\": \"x86_64\", \"abi\": \"v11\"} ]}";
    js_tok *t; pkg_entries out; pkg_details d; int i, n;
    char *many; size_t k, len;

    for (i = 0; i < (int)(sizeof bad / sizeof *bad); i++) {
        n = js_parse_alloc(bad[i], strlen(bad[i]), &t, PKG_JSON_MAXTOK);
        CHECK(n == JS_EINVAL && !t, "malformed %d: got %d", i, n);
    }
    n = js_parse_alloc(ok, strlen(ok), &t, 5);
    CHECK(n == JS_ELIMIT && !t, "limit: got %d", n);
    n = js_parse_alloc(ok, strlen(ok), &t, PKG_JSON_MAXTOK);
    CHECK(n == 10, "count: got %d", n); free(t);

    /* More than the old 8192 values in one array still reads. */
    len = 2 + 3 * 20000; many = malloc(len + 1);
    many[0] = '['; for (k = 0; k < 20000; k++) memcpy(many + 1 + 3 * k, "1, ", 3);
    many[len - 2] = '1'; many[len - 1] = ']'; many[len] = 0;
    n = js_parse_alloc(many, len, &t, PKG_JSON_MAXTOK);
    CHECK(n == 20002, "20001-element array: got %d", n); free(t); free(many);

    /* Every allocation failing in turn: a clean JS_ENOMEM, never a crash. */
    for (fail_at = 0; fail_at < 4; fail_at++) {
        calls = 0; memset(&out, 0, sizeof out);
        n = entries_from_index(ok, strlen(ok), NULL, "v11", 1, &out, NULL);
        CHECK(n == 0 || n == JS_ENOMEM, "nomem at %ld: got %d", fail_at, n);
        pkg_entries_free(&out);
        calls = 0;
        n = details_from_index(ok, strlen(ok), "a", "x86_64", "v11", &d);
        CHECK(n == 0 || n == JS_ENOMEM, "details nomem at %ld: got %d", fail_at, n);
    }
    fail_at = 0; calls = 0;
    n = js_parse_alloc(ok, strlen(ok), &t, PKG_JSON_MAXTOK);
    CHECK(n == JS_ENOMEM && !t, "first malloc fails: got %d", n);
    fail_at = -1;
    printf(fails ? "errors: %d failed\n" : "errors: ok\n", fails);
    return fails != 0;
}

int main(int argc, char **argv)
{
    char *json; size_t len; double t0, t1, t2, t3; js_tok *t; int ntok, hidden = 0, rc;
    pkg_entries all, hit; pkg_details d; struct rusage ru;

    if (argc > 1 && !strcmp(argv[1], "--errors")) return errors();
    if (argc < 2 || !(json = slurp(argv[1], &len))) { printf("usage\n"); return 2; }

    t0 = now();
    ntok = js_parse_alloc(json, len, &t, PKG_JSON_MAXTOK);
    t1 = now(); free(t);
    memset(&all, 0, sizeof all); memset(&hit, 0, sizeof hit);
    rc = entries_from_index_on(json, len, NULL, "x86_64", "v11", 0, &all, &hidden);
    t2 = now();
    CHECK(rc == 0, "list: rc %d", rc);
    CHECK(entries_from_index_on(json, len, "calculator", "x86_64", "v11", 0, &hit, NULL) == 0, "search");
    rc = details_from_index(json, len, "zz-target", "x86_64", "v11", &d);
    t3 = now();
    if (argc > 2 && !strcmp(argv[2], "-")) goto report;   /* not a generated index */
    CHECK(rc == 0 && d.in_index, "zz-target at the end: rc %d", rc);
    CHECK(!strcmp(d.sha256, "65ecebd838705d29ffdb05dbd4727aa8e9c8f49342923a73e30d60acb6427131"), "zz-target sha");
    CHECK(strstr(d.notes, "end of a generated index") != NULL, "zz-target notes");
    CHECK(!strcmp(d.e.requires, "crt.library"), "zz-target requires: %s", d.e.requires);
    /* pkg00000 is offered for four targets in shuffled order: each machine
       must get its own, whatever the order. */
    rc = details_from_index(json, len, "pkg00000", "i386", "v0", &d);
    CHECK(rc == 0 && !strcmp(d.e.arch, "i386") && !strcmp(d.e.abi, "v0"), "variant i386/v0: %s/%s", d.e.arch, d.e.abi);
    rc = details_from_index(json, len, "pkg00000", "x86_64", "v1", &d);
    CHECK(rc == 0 && !strcmp(d.e.arch, "x86_64") && !strcmp(d.e.abi, "v1"), "variant x86_64/v1: %s/%s", d.e.arch, d.e.abi);
    if (argc > 2) CHECK(all.n == atoi(argv[2]), "rows %d, expected %s", all.n, argv[2]);
report:
    getrusage(RUSAGE_SELF, &ru);
    printf("%s: %zu bytes, %d values, %d rows for x86_64/v11 (%d hidden), %d matching 'calculator'; "
           "parse %.1f ms, list %.1f ms, show last %.1f ms; peak RSS %ld KiB\n",
           argv[1], len, ntok, all.n, hidden, hit.n,
           (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3,
#ifdef __APPLE__
           ru.ru_maxrss / 1024
#else
           ru.ru_maxrss
#endif
           );
    pkg_entries_free(&all); pkg_entries_free(&hit); free(json);
    return fails != 0;
}
