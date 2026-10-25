#ifndef PKGMANAGER_DEMO_H
#define PKGMANAGER_DEMO_H
/* The demonstration storefront: pictures for applications and fictional
 * paid offers, read from a local file shipped beside PkgManager
 * (Demo/storefront.json). It shows a direction, not a feature: nothing here
 * comes from the catalogue, nothing can be bought, and an offer is never a
 * package that can be installed. docs/rfc/0003-storefront-metadata.md says
 * which fields a future catalogue might carry. Pure logic, no AROS calls,
 * so the host tests read the same file (tests/test_demo.c). */
#include "../libpkg/pkg.h"
#include <stddef.h>

#define DEMO_MAXSHOTS   4
#define DEMO_MAXOFFERS  4
#define DEMO_MAXMEDIA   16
/* Limits a picture must keep to be shown: the panel has fixed places for
   them, so a larger one would push the buttons out of a 1024x768 screen. */
#define DEMO_SHOT_W     320
#define DEMO_SHOT_H     200
#define DEMO_THUMB_W    80
#define DEMO_THUMB_H    50
#define DEMO_ICON_W     64
#define DEMO_ICON_H     64
#define DEMO_PIC_MAXBYTES (512L * 1024)

typedef struct {
    char file[PKG_MAXPATH];      /* full path; checked when shown, not here */
    char thumb[PKG_MAXPATH];     /* full path of its 80x50 version */
    char caption[120];
} demo_pic;

typedef struct {
    char id[PKG_MAXID];          /* the package (or offer) it belongs to */
    char icon[PKG_MAXPATH];      /* "" when none */
    demo_pic shots[DEMO_MAXSHOTS];
    int  nshots;
    char provenance[160];        /* where the pictures come from, shown under them */
    int  mockup;                 /* 1: drawn for the demo, not a screenshot of the program */
} demo_media;

typedef struct {
    pkg_entry e;                 /* the row the list shows; kind is "demo-offer" */
    char name[64];
    char price[16];              /* "9.99", as written; never computed with */
    char currency[8];            /* "EUR" */
    char seller[64];
    char description[480];
} demo_offer;

/* Reads the storefront. 0 on success; -1 with the reason (the window then
   shows no pictures and no demo store, and says why in its log). Paths in
   the file are relative to the file's drawer. */
int  demo_load(const char *path, char *why, size_t n);
void demo_clear(void);
int  demo_loaded(void);
const char *demo_banner(void);   /* "Demo -- purchasing is not available yet" */

const demo_media *demo_media_for(const char *id);
int  demo_noffers(void);
demo_offer *demo_offer_at(int i);
int  demo_is_offer(const pkg_entry *e);
demo_offer *demo_offer_of(const pkg_entry *e);

/* A picture's size from its PNG header, without decoding it. 0 and the size
   on success; -1 with the reason when it is not a PNG this panel shows (not
   PNG, unreadable, larger than max_w x max_h or DEMO_PIC_MAXBYTES). */
int  demo_png_check(const unsigned char *head, size_t n, long file_size,
                    int max_w, int max_h, int *w, int *h, char *why, size_t whyn);
#endif
