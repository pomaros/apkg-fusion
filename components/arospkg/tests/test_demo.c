/* The demo storefront reader (src/pkgmanager/demo.c), on the host:
 *   test_demo STOREFRONT_DIR
 * reads STOREFRONT_DIR/storefront.json as tools/make-demo-store.py writes
 * it, then the refusals: offers that could pass for packages, pictures
 * without provenance, paths out of the drawer, pictures too large. */
#include "../src/pkgmanager/demo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static int load_text(const char *text, char *why, size_t n)
{
    FILE *f = fopen("/tmp/arospkg-demo-test.json", "w");
    fputs(text, f); fclose(f);
    return demo_load("/tmp/arospkg-demo-test.json", why, n);
}

int main(int argc, char **argv)
{
    char path[512], why[200];
    const demo_media *m;
    demo_offer *o;
    unsigned char png[26] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                              0, 0, 1, 64, 0, 0, 0, 200, 8, 2 };
    int w, h;

    if (argc != 2) { printf("usage: test_demo DIR\n"); return 2; }
    snprintf(path, sizeof path, "%s/storefront.json", argv[1]);
    CHECK(demo_load(path, why, sizeof why) == 0, "the generated storefront reads");
    CHECK(demo_noffers() == 1, "one offer");
    o = demo_offer_at(0);
    CHECK(o && !strcmp(o->e.id, "demo-paintstudio") && demo_is_offer(&o->e), "the offer is a demo offer");
    CHECK(o && !strcmp(o->e.version, "9.99 EUR"), "the price shows where a version would");
    CHECK(o && demo_offer_of(&o->e) == o, "an offer is found from its row");
    m = demo_media_for("folio");
    CHECK(m && m->nshots == 2 && !m->mockup && strstr(m->provenance, "Real screenshot"), "Folio's pictures are real and say so");
    CHECK(m && strstr(m->shots[0].file, "/media/folio/1.png"), "picture paths are made relative to the file");
    m = demo_media_for("demo-paintstudio");
    CHECK(m && m->mockup, "the offer's picture is a mockup");
    CHECK(demo_media_for("grep") == NULL, "a package without pictures has none");

    CHECK(load_text("{\"schema\":1,\"demo\":true,\"offers\":[{\"id\":\"folio\",\"name\":\"x\",\"price\":\"1\",\"currency\":\"EUR\"}]}", why, sizeof why) != 0,
          "an offer named like a package is refused");
    CHECK(load_text("{\"schema\":1,\"demo\":true,\"offers\":[{\"id\":\"demo-x\",\"name\":\"x\",\"currency\":\"EUR\"}]}", why, sizeof why) != 0,
          "an offer without a price is refused");
    CHECK(load_text("{\"schema\":1,\"demo\":true,\"media\":[{\"id\":\"a\",\"kind\":\"screenshot\",\"screenshots\":[]}]}", why, sizeof why) != 0,
          "pictures without provenance are refused");
    CHECK(load_text("{\"schema\":1,\"demo\":true,\"media\":[{\"id\":\"a\",\"kind\":\"photo\",\"provenance\":\"p\"}]}", why, sizeof why) != 0,
          "an unknown kind is refused");
    CHECK(load_text("{\"schema\":1,\"demo\":true,\"media\":[{\"id\":\"a\",\"kind\":\"screenshot\",\"provenance\":\"p\","
                    "\"screenshots\":[{\"file\":\"../x.png\",\"thumb\":\"t.png\"}]}]}", why, sizeof why) != 0,
          "a picture outside the drawer is refused");
    CHECK(load_text("{\"schema\":1,\"offers\":[]}", why, sizeof why) != 0, "a file not marked as a demo is refused");
    CHECK(!demo_loaded(), "after a refusal nothing is loaded");

    CHECK(demo_png_check(png, sizeof png, 1000, DEMO_SHOT_W, DEMO_SHOT_H, &w, &h, why, sizeof why) == 0 && w == 320 && h == 200,
          "a 320x200 PNG fits the main place");
    CHECK(demo_png_check(png, sizeof png, 1000, DEMO_THUMB_W, DEMO_THUMB_H, &w, &h, why, sizeof why) != 0,
          "and not a thumbnail place");
    CHECK(demo_png_check(png, sizeof png, -1, 320, 200, &w, &h, why, sizeof why) != 0 && strstr(why, "missing"),
          "a missing file says so");
    CHECK(demo_png_check(png, sizeof png, 600L * 1024, 320, 200, &w, &h, why, sizeof why) != 0, "a file too large");
    png[25] = 3;
    CHECK(demo_png_check(png, sizeof png, 1000, 320, 200, &w, &h, why, sizeof why) != 0 && strstr(why, "truecolour"),
          "a palette PNG is refused");
    png[25] = 2;
    png[1] = 'X';
    CHECK(demo_png_check(png, sizeof png, 1000, 320, 200, &w, &h, why, sizeof why) != 0 && strstr(why, "not a PNG"),
          "not a PNG");
    if (fails) { printf("test_demo: %d failure(s)\n", fails); return 1; }
    printf("DEMO: storefront read, offers kept apart from packages, pictures checked before showing\n");
    return 0;
}
