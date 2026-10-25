/* What is new for this machine (src/libpkg/news.c) on the host: two
 * catalogue texts in, the rows the update summary, `apkg recent` and
 * PkgManager show out. And the review and runtime-test fields of a
 * catalogue entry as `apkg show` reads them (entries.c). */
#include "../src/libpkg/news.h"
#include "../src/libpkg/entries.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); bad++; } else printf("ok   %s\n", msg); } while (0)

#define E(id, ver, rev, arch, abi, first, app) \
    "{\"id\":\"" id "\",\"version\":\"" ver "\",\"revision\":" #rev ",\"arch\":\"" arch \
    "\",\"abi\":\"" abi "\",\"url\":\"https://x/" id "-" arch ".zip\",\"size\":1000,\"summary\":\"" id " app\"," \
    "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"" \
    first app "}"
#define F(d) ",\"first_added\":\"" d "\""
#define A(d) ",\"app_first_added\":\"" d "\""
#define CAT(body) "{\"schema\":1,\"packages\":[" body "]}"

static const char *OLD = CAT(
    E("kept",     "1.0", 1, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")) ","
    E("bumped",   "1.0", 1, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")) ","
    E("revved",   "2.0", 1, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")) ","
    E("i386only", "1.0", 1, "i386",   "v0",  F("2026-09-12"), A("2026-09-12")) ","
    E("v1only",   "1.0", 1, "x86_64", "v1",  F("2026-09-12"), A("2026-09-12")) ","
    E("gone",     "1.0", 1, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")));

/* bumped: new version; revved: new revision; i386only: gains a build for
   here; v1only gains another ABIv1 build only (not news on v11); gone is
   removed; fresh and Fresh2 are new; back returns (first published before
   the old catalogue's date); nodate is new without dates. */
static const char *NEW = CAT(
    E("kept",     "1.0", 1, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")) ","
    E("bumped",   "1.1", 1, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")) ","
    E("revved",   "2.0", 2, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")) ","
    E("i386only", "1.0", 1, "i386",   "v0",  F("2026-09-12"), A("2026-09-12")) ","
    E("i386only", "1.0", 1, "x86_64", "v11", F("2026-10-13"), A("2026-09-12")) ","
    E("v1only",   "1.0", 1, "x86_64", "v1",  F("2026-09-12"), A("2026-09-12")) ","
    E("v1only",   "1.0", 1, "aarch64","v1",  F("2026-10-13"), A("2026-09-12")) ","
    E("fresh",    "0.1", 0, "x86_64", "v11", F("2026-10-13"), A("2026-10-13")) ","
    E("Fresh2",   "0.2", 0, "x86_64", "v11", F("2026-10-12"), A("2026-10-12")) ","
    E("otherarch","0.1", 0, "aarch64","v1",  F("2026-10-13"), A("2026-10-13")) ","
    E("back",     "1.0", 1, "x86_64", "v11", F("2026-09-10"), A("2026-09-10")) ","
    E("nodate",   "1.0", 1, "x86_64", "v11", "", "") ","
    E("baddate",  "1.0", 1, "x86_64", "v11", F("2026-13"), A("yesterday")));

static const pkg_news_row *find(const pkg_news *n, const char *id)
{
    int i;
    for (i = 0; i < n->n; i++) if (!strcmp(n->rows[i].id, id)) return &n->rows[i];
    return NULL;
}

static void kind_is(const pkg_news *n, const char *id, int kind, const char *msg)
{
    const pkg_news_row *r = find(n, id);
    char m[200];
    snprintf(m, sizeof m, "%s: %s (got %s)", id, msg, r ? pkg_news_word(r->kind) : "no row");
    CHECK(kind < 0 ? r == NULL : r && (int)r->kind == kind, m);
}

int main(void)
{
    pkg_news n; pkg_recent_opts o; char *js; int i;

    /* An update that changed the catalogue. */
    CHECK(news_compare(OLD, strlen(OLD), NEW, strlen(NEW), "x86_64", "v11", "2026-10-01", &n) == 0, "compare parses");
    CHECK(!n.first, "an earlier catalogue: not a first one");
    kind_is(&n, "fresh", PKG_NEWS_APP, "new application");
    kind_is(&n, "Fresh2", PKG_NEWS_APP, "new application, id as the catalogue spells it");
    kind_is(&n, "nodate", PKG_NEWS_APP, "new without dates is still new to this machine");
    kind_is(&n, "back", PKG_NEWS_APP, "a returned application is new to this catalogue");
    CHECK(find(&n, "back") && find(&n, "back")->returned, "back: marked returned (first published before)");
    CHECK(find(&n, "fresh") && !find(&n, "fresh")->returned, "fresh: not returned");
    CHECK(find(&n, "nodate") && !find(&n, "nodate")->returned, "nodate: unknown date is not 'returned'");
    kind_is(&n, "i386only", PKG_NEWS_BUILD, "a build for here of an application listed before");
    kind_is(&n, "bumped", PKG_NEWS_UPDATED, "new version is an update, not a new application");
    kind_is(&n, "revved", PKG_NEWS_UPDATED, "new revision is an update");
    CHECK(find(&n, "bumped") && !strcmp(find(&n, "bumped")->old_version, "1.0"), "bumped: old version kept");
    CHECK(find(&n, "revved") && find(&n, "revved")->old_revision == 1 && find(&n, "revved")->revision == 2,
          "revved: revisions 1 -> 2");
    kind_is(&n, "kept", -1, "unchanged: not listed");
    kind_is(&n, "v1only", -1, "builds for other machines only: not news here");
    kind_is(&n, "otherarch", -1, "a new application for another CPU: not news here");
    kind_is(&n, "gone", -1, "removed: not listed as new");
    kind_is(&n, "baddate", PKG_NEWS_APP, "new with malformed dates: new, dates unknown");
    CHECK(n.apps == 5 && n.builds == 1 && n.updated == 2, "counts: 5 applications, 1 build, 2 updated");
    CHECK(n.n == 8 && n.rows[0].kind == PKG_NEWS_APP && n.rows[n.n - 1].kind == PKG_NEWS_UPDATED,
          "rows grouped by kind");

    /* The record round trip: what --since-last-update replays. */
    snprintf(n.catalogue, sizeof n.catalogue, "https://example.invalid/index-v2.json");
    snprintf(n.when, sizeof n.when, "2026-10-14 10:00");
    snprintf(n.catalogue_sha256, sizeof n.catalogue_sha256, "%064d", 7);
    js = pkg_news_json(&n, "last-update", 0);
    CHECK(js != NULL, "record written");
    {
        pkg_news back;
        CHECK(js && news_from_json(js, strlen(js), &back) == 0, "record read back");
        CHECK(back.n == n.n && back.apps == n.apps && back.builds == n.builds && back.updated == n.updated,
              "record keeps every row and count");
        CHECK(!strcmp(back.catalogue, n.catalogue) && !strcmp(back.catalogue_sha256, n.catalogue_sha256),
              "record keeps the address and catalogue hash");
        CHECK(find(&back, "back") && find(&back, "back")->returned, "record keeps 'returned'");
        pkg_news_free(&back);
    }
    free(js);
    js = pkg_news_json(&n, "last-update", 1);
    CHECK(js && strstr(js, "\"new_applications\": 5") && strstr(js, "\"kind\": \"new-build\"")
          && !strstr(js, "\"id\": \"nodate\""), "limit 1 keeps one row per kind, counts stay whole");
    free(js);
    pkg_news_free(&n);

    /* Update again with nothing changed. */
    CHECK(news_compare(NEW, strlen(NEW), NEW, strlen(NEW), "x86_64", "v11", "2026-10-14", &n) == 0 && n.n == 0,
          "same catalogue again: nothing is new");
    pkg_news_free(&n);

    /* First catalogue: the newest additions, not 'everything is new'. */
    CHECK(news_compare(NULL, 0, NEW, strlen(NEW), "x86_64", "v11", NULL, &n) == 0, "first: parses");
    CHECK(n.first && n.first_why[0], "first: says so, with the reason");
    CHECK(n.apps == 0 && n.builds == 0 && n.updated == 0, "first: nothing called new");
    CHECK(n.recent == NEWS_FIRST_LIMIT && n.n == NEWS_FIRST_LIMIT, "first: at most NEWS_FIRST_LIMIT recent additions");
    CHECK(n.n && !strcmp(n.rows[0].id, "fresh") && !strcmp(n.rows[1].id, "Fresh2"), "first: newest first");
    CHECK(!find(&n, "nodate") && !find(&n, "baddate") && n.unknown_dates == 2,
          "first: unknown and malformed dates never listed, counted");
    pkg_news_free(&n);

    /* An unreadable previous catalogue is no basis either. */
    CHECK(news_compare("{not json", 9, NEW, strlen(NEW), "x86_64", "v11", NULL, &n) == 0 && n.first,
          "unreadable previous catalogue: treated as first");
    pkg_news_free(&n);
    CHECK(news_compare(OLD, strlen(OLD), "{\"x\":1}", 7, "x86_64", "v11", NULL, &n) != 0,
          "a new text that is not a catalogue is refused");

    /* apkg recent */
    memset(&o, 0, sizeof o);
    CHECK(news_recent(NEW, strlen(NEW), "x86_64", "v11", &o, &n) == 0, "recent parses");
    CHECK(n.apps == 6 && n.unknown_dates == 2, "recent: every dated application for here; unknown counted");
    CHECK(!find(&n, "i386only"), "recent: an older application with a new build is not listed by default");
    CHECK(!find(&n, "otherarch") && !find(&n, "v1only"), "recent: other machines' applications not listed");
    for (i = 1; i < n.n; i++)
        if (strcmp(n.rows[i - 1].app_first_added, n.rows[i].app_first_added) < 0) break;
    CHECK(i == n.n, "recent: newest first");
    pkg_news_free(&n);
    o.builds = 1;
    news_recent(NEW, strlen(NEW), "x86_64", "v11", &o, &n);
    kind_is(&n, "i386only", PKG_NEWS_BUILD, "recent --builds: the new build, labelled");
    pkg_news_free(&n);
    o.builds = 0; snprintf(o.since, sizeof o.since, "2026-10-12");
    news_recent(NEW, strlen(NEW), "x86_64", "v11", &o, &n);
    CHECK(n.n == 2 && find(&n, "fresh") && find(&n, "Fresh2"), "recent --since 2026-10-12: two");
    pkg_news_free(&n);
    o.builds = 1;
    news_recent(NEW, strlen(NEW), "x86_64", "v11", &o, &n);
    CHECK(n.n == 3 && find(&n, "i386only"), "recent --since with --builds: the build too");
    pkg_news_free(&n);
    memset(&o, 0, sizeof o); o.limit = 2;
    news_recent(NEW, strlen(NEW), "x86_64", "v11", &o, &n);
    CHECK(n.n == 2 && n.apps == 2, "recent --limit 2");
    js = pkg_news_json(&n, "recent", 0);
    CHECK(js && strstr(js, "\"mode\": \"recent\"") && strstr(js, "\"unknown_dates\": 2"), "recent as JSON");
    free(js);
    pkg_news_free(&n);
    news_recent(NEW, strlen(NEW), "i386", "v0", &o, &n);
    CHECK(n.n == 1 && find(&n, "i386only"), "recent on i386/v0: its own builds");
    pkg_news_free(&n);

    /* A date with more after it is not a date; a catalogue string that
       would need an escape is made plain before it is written again. */
    {
        static const char *X = CAT(
            "{\"id\":\"long\",\"version\":\"1\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"https://x/l\","
            "\"summary\":\"say \\\"hi\\\"\",\"first_added\":\"2026-10-14T00:00\",\"app_first_added\":\"2026-10-14T00:00\"}");
        CHECK(news_compare(NULL, 0, X, strlen(X), "x86_64", "v11", NULL, &n) == 0 && n.unknown_dates == 1 && n.n == 0,
              "a date with a time after it is unknown, not cut to a date");
        pkg_news_free(&n);
        CHECK(news_compare(CAT(""), strlen(CAT("")), X, strlen(X), "x86_64", "v11", NULL, &n) == 0 && n.n == 1
              && !strchr(n.rows[0].summary, '"') && !strchr(n.rows[0].summary, '\\'),
              "a summary with quotes and backslashes is made plain");
        js = pkg_news_json(&n, "last-update", 0);
        {
            pkg_news back;
            CHECK(js && news_from_json(js, strlen(js), &back) == 0 && back.n == 1
                  && !strcmp(back.rows[0].summary, n.rows[0].summary), "and it reads back as written");
            pkg_news_free(&back);
        }
        free(js);
        pkg_news_free(&n);
    }

    /* review and runtime tests, as show reads them */
    {
        static const char *R = CAT(
            "{\"id\":\"rv\",\"version\":\"1\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"https://x/a\","
            "\"size\":1,\"sha256\":\"00\",\"first_added\":\"2026-09-29\",\"app_first_added\":\"2026-09-20\","
            "\"review\":{\"status\":\"reviewed\",\"suite\":1,\"date\":\"2026-10-14\","
            "\"checked\":[\"metadata\",\"download\",\"cpu\"],\"unchecked\":[\"abi\"]},"
            "\"runtime_tests\":[{\"system\":\"AROS One 1.3\",\"arch\":\"x86_64\",\"abi\":\"v11\","
            "\"machine\":\"QEMU (KVM)\",\"date\":\"2026-09-29\",\"result\":\"passed\","
            "\"checked\":[\"install\",\"start\"],\"note\":\"window\"}]},"
            "{\"id\":\"ls\",\"version\":\"1\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"https://x/b\","
            "\"size\":1,\"sha256\":\"00\",\"review\":{\"status\":\"listed\"},\"runtime_tests\":[]},"
            "{\"id\":\"odd\",\"version\":\"1\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"https://x/c\","
            "\"size\":1,\"sha256\":\"00\",\"review\":{\"status\":\"verified\"}},"
            "{\"id\":\"old\",\"version\":\"1\",\"arch\":\"x86_64\",\"abi\":\"v11\",\"url\":\"https://x/d\","
            "\"size\":1,\"sha256\":\"00\"}");
        pkg_details d;
        details_from_index(R, strlen(R), "rv", "x86_64", "v11", &d);
        CHECK(!strcmp(d.review, "reviewed") && d.review_suite == 1 && !strcmp(d.review_date, "2026-10-14"),
              "show: reviewed, suite and date");
        CHECK(!strcmp(d.review_checked, "metadata, download, cpu") && !strcmp(d.review_unchecked, "abi"),
              "show: what was checked and what could not be");
        CHECK(d.ntests == 1 && strstr(d.tests[0], "AROS One 1.3 x86_64/v11, QEMU (KVM), 2026-09-29: passed (install, start) -- window"),
              "show: the runtime test, its system and date");
        CHECK(!strcmp(d.first_added, "2026-09-29") && !strcmp(d.app_first_added, "2026-09-20"), "show: both dates");
        details_from_index(R, strlen(R), "ls", "x86_64", "v11", &d);
        CHECK(!strcmp(d.review, "listed") && d.ntests == 0 && !d.first_added[0], "show: listed, no tests, no date");
        details_from_index(R, strlen(R), "odd", "x86_64", "v11", &d);
        CHECK(!d.review[0], "show: an unknown review word is not read as reviewed");
        details_from_index(R, strlen(R), "old", "x86_64", "v11", &d);
        CHECK(!d.review[0] && !d.ntests, "show: a catalogue before 0.6 says nothing");
    }

    printf(bad ? "\n%d FAILED\n" : "\nall passed\n", bad);
    return bad != 0;
}
