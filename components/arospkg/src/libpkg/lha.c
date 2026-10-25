/* LHA reader: the Amiga's archive format, and so not optional here.
 *
 * Supports what the archives actually contain -- header levels 0, 1 and 2,
 * and the methods -lh0- (stored), -lh5-, -lh6-, -lh7- and -lhd- (a directory
 * entry). Measured across the AROS Archives LHA uploads and our own Micropolis
 * release: -lh0-, -lh5- and -lhd- are all that appear. lh6 and lh7 differ from
 * lh5 only in dictionary size, so they come almost free and are accepted.
 *
 * The decompressor is the classic LHA static-Huffman-over-LZSS scheme. It
 * decodes canonically, by code length, rather than through the usual 4096-entry
 * jump table: the table version is faster and is the part of every LHA
 * implementation that is hardest to verify by reading. Here the archives are a
 * few megabytes and the guest is a package manager, not a demo, so the version
 * that can be checked line by line wins. Correctness is tested against the
 * system `lha` over every real archive we have -- see tests/test_lha.c.
 *
 * Validation matches zip.c exactly, because the danger is identical: a member
 * path is refused if it is absolute in the POSIX sense, absolute in the AROS
 * sense (a ':' ANYWHERE names a volume), escapes with '..', or carries control
 * characters. Nothing is written until every member in the archive has passed.
 */
#include "lha.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LHA_MAXNAME 512

/* ------------------------------------------------------------ bit reader */

typedef struct {
    const unsigned char *p;
    long len, pos;
    unsigned long bits;      /* right-aligned bit reservoir */
    int nbits;
    int overrun;
} bitreader;

static void br_init(bitreader *b, const unsigned char *p, long len)
{ b->p = p; b->len = len; b->pos = 0; b->bits = 0; b->nbits = 0; b->overrun = 0; }

/* Past the end reads as zero bits and is remembered: a truncated member must
 * fail rather than decode into whatever follows. */
static unsigned br_peek(bitreader *b, int n)
{
    while (b->nbits < n) {
        unsigned char c = 0;
        if (b->pos < b->len) c = b->p[b->pos++];
        else b->overrun = 1;
        b->bits = (b->bits << 8) | c;
        b->nbits += 8;
    }
    return (unsigned)((b->bits >> (b->nbits - n)) & ((1UL << n) - 1));
}

static unsigned br_get(bitreader *b, int n)
{
    unsigned v;
    if (n == 0) return 0;
    v = br_peek(b, n);
    b->nbits -= n;
    return v;
}

/* ------------------------------------------------------- canonical huffman */

#define NC   510                 /* literals 0..255 and lengths 256..509 */
#define NT   19
#define NPT  20

typedef struct {
    unsigned short count[17];
    unsigned short symbol[NC];
    int single;                  /* >= 0: the tree holds one symbol only */
} huff;

static int huff_build(huff *h, const unsigned char *len, int n)
{
    int i, l, left, index[17];
    memset(h->count, 0, sizeof h->count);
    h->single = -1;
    for (i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    for (left = 1, l = 1; l <= 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;         /* over-subscribed: corrupt */
    }
    index[1] = 0;
    for (l = 1; l < 16; l++) index[l + 1] = index[l] + h->count[l];
    for (i = 0; i < n; i++) if (len[i]) h->symbol[index[len[i]]++] = (unsigned short)i;
    return 0;
}

static void huff_single(huff *h, int symbol)
{ memset(h->count, 0, sizeof h->count); h->single = symbol; }

static int huff_decode(huff *h, bitreader *b)
{
    int l, first = 0, index = 0, code = 0, count;
    if (h->single >= 0) return h->single;
    for (l = 1; l <= 16; l++) {
        code |= (int)br_get(b, 1);
        count = h->count[l];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* --------------------------------------------------------- lh5/6/7 decoder */

typedef struct {
    bitreader br;
    huff c, pt;
    unsigned char c_len[NC], pt_len[NPT];
    int blocksize;
    int np, pbit;
} lzh;

static int read_pt_len(lzh *z, int nn, int nbit, int i_special)
{
    int i, c, n = (int)br_get(&z->br, nbit);
    if (n == 0) {
        huff_single(&z->pt, (int)br_get(&z->br, nbit));
        return 0;
    }
    if (n > nn) return -1;
    i = 0;
    while (i < n) {
        /* A length below 7 is three bits. Seven is three ones followed by a
           unary tail: each further 1 adds one, and a 0 ENDS it -- that
           terminating zero has to be consumed too. Leaving it in the stream
           decoded the first members of an archive correctly and then walked
           off the rails on the first file whose tree used a long code. */
        c = (int)br_peek(&z->br, 3);
        if (c != 7) br_get(&z->br, 3);
        else {
            br_get(&z->br, 3);
            while (br_peek(&z->br, 1)) { br_get(&z->br, 1); c++; if (c > 16) return -1; }
            br_get(&z->br, 1);
        }
        z->pt_len[i++] = (unsigned char)c;
        if (i == i_special) {
            c = (int)br_get(&z->br, 2);
            while (--c >= 0) { if (i >= nn) return -1; z->pt_len[i++] = 0; }
        }
    }
    while (i < nn) z->pt_len[i++] = 0;
    return huff_build(&z->pt, z->pt_len, nn);
}

static int read_c_len(lzh *z)
{
    int i, c, n = (int)br_get(&z->br, 9);
    if (n == 0) {
        huff_single(&z->c, (int)br_get(&z->br, 9));
        return 0;
    }
    if (n > NC) return -1;
    i = 0;
    while (i < n) {
        c = huff_decode(&z->pt, &z->br);
        if (c < 0) return -1;
        if (c <= 2) {
            if (c == 0) c = 1;
            else if (c == 1) c = (int)br_get(&z->br, 4) + 3;
            else c = (int)br_get(&z->br, 9) + 20;
            while (--c >= 0) { if (i >= NC) return -1; z->c_len[i++] = 0; }
        } else {
            z->c_len[i++] = (unsigned char)(c - 2);
        }
        if (z->br.overrun) return -1;
    }
    while (i < NC) z->c_len[i++] = 0;
    return huff_build(&z->c, z->c_len, NC);
}

static int decode_c(lzh *z)
{
    if (z->blocksize == 0) {
        z->blocksize = (int)br_get(&z->br, 16);
        if (z->blocksize == 0) return -1;
        if (read_pt_len(z, NT, 5, 3) != 0) return -1;
        if (read_c_len(z) != 0) return -1;
        /* The offset tree replaces the code-length tree in z->pt: the code
           lengths have done their work by now, inside read_c_len. */
        if (read_pt_len(z, z->np, z->pbit, -1) != 0) return -1;
    }
    z->blocksize--;
    return huff_decode(&z->c, &z->br);
}

static int decode_p(lzh *z)
{
    int j = huff_decode(&z->pt, &z->br);
    if (j < 0) return -1;
    if (j == 0) return 0;
    j--;
    return (int)((1U << j) | br_get(&z->br, j));
}

/* Decompress `csize` bytes at `in` into `out`, which must hold `usize`. */
static int lzh_decode(const unsigned char *in, long csize,
                      unsigned char *out, long usize, int dicbits, char why[160])
{
    lzh z;
    long opos = 0;
    memset(&z, 0, sizeof z);
    br_init(&z.br, in, csize);
    z.blocksize = 0;
    z.np = dicbits + 1;
    z.pbit = dicbits == 13 ? 4 : 5;
    z.c.single = z.pt.single = -1;

    while (opos < usize) {
        int c = decode_c(&z);
        if (c < 0 || z.br.overrun) { snprintf(why, 160, "damaged compressed data"); return -1; }
        if (c < 256) {
            out[opos++] = (unsigned char)c;
        } else {
            int len = c - 256 + 3;
            int off = decode_p(&z);
            long from;
            if (off < 0) { snprintf(why, 160, "damaged compressed data"); return -1; }
            from = opos - off - 1;
            if (from < 0) { snprintf(why, 160, "match before the start of the file"); return -1; }
            while (len-- > 0 && opos < usize) out[opos++] = out[from++];
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- headers */

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long rd32(const unsigned char *p)
{ return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24); }

/* Amiga and MS-DOS LHA both separate directories with 0xFF inside the stored
 * name; level 2 keeps the directory in an extended header with the same
 * separator. Everything downstream expects '/'. */
static void fix_separators(char *s)
{
    for (; *s; s++) if ((unsigned char)*s == 0xFF || *s == '\\') *s = '/';
}

static int unsafe_name(const char *n)
{
    const char *p;
    if (!*n) return 1;
    if (*n == '/') return 1;
    if (strchr(n, ':')) return 1;                 /* absolute, AROS style */
    for (p = n; *p; p++) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0) && (p == n || p[-1] == '/'))
            return 1;
        if ((unsigned char)*p < 0x20) return 1;
    }
    return 0;
}


/* The extended-header chain. Its shape is easy to get wrong and I did: the size
 * of the NEXT header sits in the last two bytes of the current one (and of the
 * base header), not at the start of each. Measured against a real archive.
 * Returns the total bytes the chain occupies, or -1. Fills in the name when the
 * chain carries one (type 0x01 filename, 0x02 directory, separator 0xFF). */
static long ext_chain(const unsigned char *d, long len, long start, long nextsize,
                      char *name, size_t namesz, char why[160])
{
    long p = start, total = 0;
    char dir[LHA_MAXNAME] = "";
    while (nextsize) {
        unsigned type;
        long dlen;
        if (nextsize < 3 || p + nextsize > len) { snprintf(why, 160, "truncated extended header"); return -1; }
        type = d[p];
        dlen = nextsize - 3;
        if (type == 0x01 && dlen > 0) {
            if ((size_t)dlen >= namesz) { snprintf(why, 160, "member name too long"); return -1; }
            memcpy(name, d + p + 1, (size_t)dlen); name[dlen] = 0;
        } else if (type == 0x02 && dlen > 0) {
            if ((size_t)dlen >= sizeof dir) { snprintf(why, 160, "member path too long"); return -1; }
            memcpy(dir, d + p + 1, (size_t)dlen); dir[dlen] = 0;
        }
        total += nextsize;
        p += nextsize;
        nextsize = rd16(d + p - 2);
    }
    if (dir[0]) {
        char full[LHA_MAXNAME];
        fix_separators(dir);
        if (snprintf(full, sizeof full, "%s%s", dir, name) >= (int)sizeof full) {
            snprintf(why, 160, "member path too long"); return -1;
        }
        snprintf(name, namesz, "%s", full);
    }
    return total;
}

typedef struct {
    char name[LHA_MAXNAME];
    char method[6];
    long csize, usize, data;
    int is_dir;
} lha_ent;

/* Parse the header at `off`. Returns the offset of the next header, 0 at the
 * end of the archive, or -1 on a malformed one. */
static long parse_header(const unsigned char *d, long len, long off,
                         lha_ent *e, char why[160])
{
    int level;
    long base;
    memset(e, 0, sizeof *e);
    if (off >= len) return 0;
    if (d[off] == 0) return 0;                    /* end marker */
    if (off + 24 > len) { snprintf(why, 160, "truncated header"); return -1; }

    memcpy(e->method, d + off + 2, 5);
    e->method[5] = 0;
    if (e->method[0] != '-' || e->method[4] != '-') {
        snprintf(why, 160, "not an LHA header at offset %ld", off);
        return -1;
    }
    level = d[off + 20];
    e->usize = (long)rd32(d + off + 11);
    e->is_dir = strcmp(e->method, "-lhd-") == 0;

    if (level == 0 || level == 1) {
        int namelen = d[off + 21];
        long hsize = d[off], skip = (long)rd32(d + off + 7);
        if (off + hsize + 2 > len || 22 + namelen > hsize + 2) {
            snprintf(why, 160, "truncated header"); return -1;
        }
        if (namelen >= LHA_MAXNAME) { snprintf(why, 160, "member name too long"); return -1; }
        memcpy(e->name, d + off + 22, namelen);
        e->name[namelen] = 0;
        base = off + hsize + 2;
        if (level == 0) {
            e->csize = skip;
            e->data = base;
        } else {
            /* level 1: the size field counts the data AND the extended headers
               that follow the base header, so the data begins after them. */
            long extbytes = ext_chain(d, len, base, (long)rd16(d + base - 2),
                                      e->name, sizeof e->name, why);
            if (extbytes < 0) return -1;
            e->csize = skip - extbytes;
            e->data = base + extbytes;
            if (e->csize < 0) { snprintf(why, 160, "bad header sizes"); return -1; }
        }
        } else if (level == 2) {
        long total = (long)rd16(d + off);
        e->csize = (long)rd32(d + off + 7);
        if (off + total > len) { snprintf(why, 160, "truncated header"); return -1; }
        if (total > 26 && ext_chain(d, off + total, off + 26, (long)rd16(d + off + 24),
                                    e->name, sizeof e->name, why) < 0) return -1;
        e->data = off + total;
    } else {
        snprintf(why, 160, "unsupported LHA header level %d", level);
        return -1;
    }

    fix_separators(e->name);
    if (e->data + e->csize > len) { snprintf(why, 160, "member runs past the end of the archive"); return -1; }
    return e->data + e->csize;
}

static int method_dicbits(const char *m)
{
    if (!strcmp(m, "-lh5-")) return 13;
    if (!strcmp(m, "-lh6-")) return 15;
    if (!strcmp(m, "-lh7-")) return 16;
    return -1;
}

/* Write one member's contents to `path`. */
static int write_member(const unsigned char *d, const lha_ent *e,
                        const char *path, char why[160])
{
    unsigned char *out = NULL;
    FILE *f;
    int rc = -1;

    if (!strcmp(e->method, "-lh0-")) {
        if (e->csize != e->usize) { snprintf(why, 160, "stored member with a bad size"); return -1; }
        out = (unsigned char *)(e->usize ? malloc((size_t)e->usize) : malloc(1));
        if (!out) { snprintf(why, 160, "out of memory"); return -1; }
        memcpy(out, d + e->data, (size_t)e->usize);
    } else {
        int bits = method_dicbits(e->method);
        if (bits < 0) { snprintf(why, 160, "unsupported compression %s", e->method); return -1; }
        out = (unsigned char *)malloc((size_t)(e->usize ? e->usize : 1));
        if (!out) { snprintf(why, 160, "out of memory"); return -1; }
        if (lzh_decode(d + e->data, e->csize, out, e->usize, bits, why) != 0) { free(out); return -1; }
    }
    f = fopen(path, "wb");
    if (!f) { snprintf(why, 160, "cannot write %s", path); free(out); return -1; }
    if (e->usize && fwrite(out, 1, (size_t)e->usize, f) != (size_t)e->usize) {
        snprintf(why, 160, "write failed: %s", path);
        fclose(f); free(out); return -1;
    }
    rc = fclose(f) == 0 ? 0 : -1;
    if (rc != 0) snprintf(why, 160, "close failed: %s", path);
    free(out);
    return rc;
}

static void mk_parents(const char *dest, const char *rel)
{
    char buf[LHA_MAXNAME], full[LHA_MAXNAME];
    size_t i;
    snprintf(buf, sizeof buf, "%s", rel);
    for (i = 0; buf[i]; i++)
        if (buf[i] == '/') {
            buf[i] = 0;
            u_join(full, sizeof full, dest, buf);
            u_mkdir(full);
            buf[i] = '/';
        }
}

static int extract(const char *archive, const char *dest, const char *strip,
                   const char *only, long max_file, long max_total,
                   lha_cb cb, void *user, char why[160])
{
    FILE *f = fopen(archive, "rb");
    unsigned char *d = NULL;
    long size, off, total = 0;
    size_t striplen = strip ? strlen(strip) : 0;
    int rc = -1, found = 0;
    lha_ent e;

    why[0] = 0;
    if (!f) { snprintf(why, 160, "cannot open %s", archive); return -1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0) { snprintf(why, 160, "empty archive"); fclose(f); return -1; }
    d = (unsigned char *)malloc((size_t)size);
    if (!d) { snprintf(why, 160, "out of memory"); fclose(f); return -1; }
    if (fread(d, 1, (size_t)size, f) != (size_t)size) {
        snprintf(why, 160, "short read on %s", archive); goto out;
    }

    /* Pass 1: every member is validated before a byte is written, as in zip.c. */
    for (off = 0; (off = parse_header(d, size, off, &e, why)) > 0; ) {
        if (unsafe_name(e.name)) { snprintf(why, 160, "unsafe member path: %s", e.name); goto out; }
        if (e.is_dir) continue;
        if (only) { if (!strcmp(e.name, only) && max_file > 0 && e.usize > max_file) {
                        snprintf(why, 160, "member over size limit: %s", e.name); goto out; }
                    continue; }
        if (max_file > 0 && e.usize > max_file) {
            snprintf(why, 160, "member over size limit: %s", e.name); goto out;
        }
        total += e.usize;
        if (max_total > 0 && total > max_total) {
            snprintf(why, 160, "archive over total size limit"); goto out;
        }
    }
    if (off < 0) goto out;

    /* Pass 2: extract. */
    for (off = 0; (off = parse_header(d, size, off, &e, why)) > 0; ) {
        char rel[LHA_MAXNAME], full[LHA_MAXNAME];
        const char *use;
        if (only) {
            if (strcmp(e.name, only) != 0) continue;
            found = 1;
            if (write_member(d, &e, dest, why) != 0) goto out;
            break;
        }
        use = e.name;
        if (striplen && strncmp(e.name, strip, striplen) == 0 && e.name[striplen] == '/')
            use = e.name + striplen + 1;
        else if (striplen) continue;
        if (!*use) continue;
        snprintf(rel, sizeof rel, "%s", use);
        mk_parents(dest, rel);
        u_join(full, sizeof full, dest, rel);
        if (e.is_dir) {
            /* An -lhd- member is a directory the archive means to exist even
               when nothing is in it. Skipping these left OpenLoco without its
               empty objects/ drawer, which the game warns about at every
               start; the system's lha creates it. A trailing '/' is stripped
               so the mkdir gets the directory's own path. */
            size_t n = strlen(full);
            if (n && full[n - 1] == '/') full[n - 1] = 0;
            u_mkdir(full);
            continue;
        }
        if (write_member(d, &e, full, why) != 0) goto out;
        if (cb && cb(rel, full, user) != 0) { snprintf(why, 160, "aborted at %s", rel); goto out; }
    }
    if (off < 0) goto out;
    if (only && !found) { snprintf(why, 160, "no member %s in the archive", only); goto out; }
    rc = 0;
out:
    free(d);
    fclose(f);
    return rc;
}

int lha_extract(const char *archive, const char *dest, const char *strip,
                long max_file, long max_total, lha_cb cb, void *user, char why[160])
{ return extract(archive, dest, strip, NULL, max_file, max_total, cb, user, why); }

int lha_extract_member(const char *archive, const char *member,
                       const char *dest_file, long max_file, char why[160])
{ return extract(archive, dest_file, "", member, max_file, 0, NULL, NULL, why); }

int lha_is_lha(const char *archive)
{
    FILE *f = fopen(archive, "rb");
    unsigned char h[8];
    int ok;
    if (!f) return 0;
    ok = fread(h, 1, sizeof h, f) == sizeof h;
    fclose(f);
    /* The method id sits at offset 2 in every header level. */
    return ok && h[2] == '-' && h[6] == '-' && (h[3] == 'l' || h[3] == 'L');
}
