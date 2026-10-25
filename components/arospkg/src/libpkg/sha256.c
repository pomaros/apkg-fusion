/* SHA-256. Written in-tree rather than linked from OpenSSL so that `pkg` stays
 * ~100 KB instead of ~8 MB: at this stage the binary is rebuilt and pushed to a
 * VM many times a day, and the vvfat drive is only re-read at boot, so size is
 * iteration speed. When networking lands and OpenSSL is linked anyway, this can
 * go. Verified against the standard vectors in tests/. */
#include "sha256.h"
#include <string.h>
#include <stdio.h>

#define ROR(x,n) (((x) >> (n)) | ((x) << (32-(n))))

static const unsigned int K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static void block(sha256_ctx *c, const unsigned char *p)
{
    unsigned int w[64], a,b,cc,d,e,f,g,h,t1,t2; int i;
    for (i = 0; i < 16; i++)
        w[i] = (unsigned int)p[i*4]<<24 | (unsigned int)p[i*4+1]<<16 |
               (unsigned int)p[i*4+2]<<8 | (unsigned int)p[i*4+3];
    for (; i < 64; i++) {
        unsigned int s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        unsigned int s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0];b=c->h[1];cc=c->h[2];d=c->h[3];e=c->h[4];f=c->h[5];g=c->h[6];h=c->h[7];
    for (i = 0; i < 64; i++) {
        unsigned int S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        unsigned int mj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + K[i] + w[i];
        t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d;
    c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;
}

void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667;c->h[1]=0xbb67ae85;c->h[2]=0x3c6ef372;c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f;c->h[5]=0x9b05688c;c->h[6]=0x1f83d9ab;c->h[7]=0x5be0cd19;
    c->len = 0; c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->len += (unsigned long long)len * 8;
    while (len) {
        size_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { block(c, c->buf); c->n = 0; }
    }
}

void sha256_final(sha256_ctx *c, char out_hex[65])
{
    unsigned long long bits = c->len;   /* saved: update() below bumps it */
    unsigned char pad[64];
    size_t padlen;
    int i;

    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    padlen = (c->n < 56) ? (56 - c->n) : (120 - c->n);
    sha256_update(c, pad, padlen);      /* leaves exactly 56 bytes buffered */

    for (i = 0; i < 8; i++)
        c->buf[56 + i] = (unsigned char)(bits >> (56 - i * 8));
    block(c, c->buf);
    c->n = 0;

    for (i = 0; i < 8; i++)
        sprintf(out_hex + i * 8, "%08x", c->h[i]);
    out_hex[64] = 0;
}

int sha256_file_count(const char *path, char out_hex[65], long *bytes)
{
    FILE *f = fopen(path, "rb");
    sha256_ctx c; unsigned char buf[8192]; size_t n;
    long total = 0;
    int bad;
    if (bytes) *bytes = 0;
    if (!f) return -1;
    sha256_init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) { sha256_update(&c, buf, n); total += (long)n; }
    /* fread returns 0 on an error as well as at the end. Without this check a
       read that failed half way hashed the part before it, and the caller
       reported a file that does not match instead of one it could not read. */
    bad = ferror(f);
    fclose(f);
    if (bytes) *bytes = total;
    if (bad) return -1;
    sha256_final(&c, out_hex);
    return 0;
}

int sha256_file(const char *path, char out_hex[65])
{
    return sha256_file_count(path, out_hex, NULL);
}
