/* Known-answer tests. Run on the host: the algorithm is not AROS-specific and
 * a wrong hash here would silently corrupt every verification in libpkg. */
#include "../src/libpkg/sha256.h"
#include <stdio.h>
#include <string.h>

static int check(const char *msg, size_t len, const char *want, const char *label)
{
    sha256_ctx c; char got[65];
    sha256_init(&c); sha256_update(&c, msg, len); sha256_final(&c, got);
    if (strcmp(got, want)) { printf("FAIL %s\n  want %s\n  got  %s\n", label, want, got); return 1; }
    printf("ok   %s\n", label);
    return 0;
}

int main(void)
{
    static char million[1000000];
    int bad = 0;
    memset(million, 'a', sizeof million);
    bad += check("", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    bad += check("abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    bad += check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit");
    bad += check(million, sizeof million,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "a x 1e6");
    /* 55, 56, 57 bytes: the padding boundary, where the first version was wrong */
    bad += check("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 55,
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "55 bytes");
    bad += check("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 56,
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "56 bytes");
    /* A read error is a failure, not a hash of what came before it. A
       directory opens on POSIX and fails on the first read. */
    {
        char got[65]; long n = -1;
        if (sha256_file_count(".", got, &n) == 0) {
            printf("FAIL read error hashed as %s\n", got); bad++;
        } else printf("ok   read error reported\n");
    }
    printf(bad ? "\nFAILED\n" : "\nall vectors pass\n");
    return bad != 0;
}
