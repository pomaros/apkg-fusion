/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_target.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int pass, const char *what)
{
    ++checks;
    if (!pass) { ++failures; printf("FAIL: %s\n", what); }
}

int main(void)
{
    static const char *const valid[] = {
        "x86_64-aros-v11-native", "x86_64-aros-v1-native",
        "i386-aros-v0-native", "aarch64-aros-v1-raspi",
        "aarch64-aros-v1-hosted", "arm-aros-v1-raspi",
        "m68k-aros-v0-native", "ppc-aros-v1-native"
    };
    static const char *const invalid[] = {
        "", "x86_64", "generic", "x86_64-aros-v11", "x86_64-aros-v11-",
        "x86_64--v11-native", "-aros-v11-native", "x86_64-aros--native",
        "x86_64-aros-v11-native-extra", "x86_64-aros-v11-native-",
        "X86_64-aros-v11-native", "x86_64-AROS-v11-native",
        "x86_64-linux-v11-native", "x86_64-aros-V11-native",
        "x86_64-aros-v2-native", "x86_64-aros-v01-native",
        "x86_64-aros-v11-Native", "x86_64-aros-v11-unknown",
        "x86_64-aros-v11-raspi", "i386-aros-v0-raspi",
        "ppc-aros-v1-raspi", "m68k-aros-v0-raspi",
        "ppc64-aros-v1-native", "riscv64-aros-v1-native",
        " x86_64-aros-v11-native", "x86_64-aros-v11-native ",
        "x86_64-aros-v11-native\n", "x86_64/aros/v11/native",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "aarch64-aros-v111111111-native"
    };
    struct pkg_target a, b, zero;
    char text[PKG_TARGET_MAX + 1], why[100], tiny[1];
    size_t i;
    memset(&zero, 0, sizeof zero);
    for (i = 0; i < sizeof valid / sizeof valid[0]; ++i) {
        check(!pkg_target_parse(valid[i], &a, why, sizeof why), valid[i]);
        check(!why[0], "successful parse clears diagnostic");
        check(!pkg_target_format(&a, text, sizeof text) && !strcmp(text, valid[i]),
              "canonical round trip");
        check(pkg_target_matches(&a, &a), "valid target matches itself");
        check(pkg_target_cpu_matches(valid[i], a.cpu), "qualified target matches its ELF CPU");
    }
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        memset(&a, 0x5a, sizeof a);
        check(pkg_target_parse(invalid[i], &a, why, sizeof why) == -1, invalid[i]);
        check(why[0] && !memcmp(&a, &zero, sizeof a), "refusal gives reason and clears target");
    }
    check(pkg_target_parse(NULL, &a, why, sizeof why) == -1, "NULL target refused");
    check(!pkg_target_parse(valid[0], NULL, NULL, 0), "validation without output buffers");
    tiny[0] = 'x';
    check(pkg_target_parse("wrong", &a, tiny, sizeof tiny) == -1 && !tiny[0], "tiny error buffer terminated");
    pkg_target_parse(valid[0], &a, NULL, 0);
    pkg_target_parse(valid[1], &b, NULL, 0);
    check(!pkg_target_matches(&a, &b), "same CPU with different ABI cannot match");
    pkg_target_parse(valid[3], &a, NULL, 0);
    pkg_target_parse(valid[4], &b, NULL, 0);
    check(!pkg_target_matches(&a, &b), "same CPU and ABI with different runtime cannot match");
    check(!pkg_target_matches(&zero, &zero), "two invalid targets cannot match");
    check(!pkg_target_matches(NULL, &a), "NULL target cannot match");
    check(!pkg_target_cpu_matches(valid[0], "i386"), "wrong ELF CPU rejected");
    check(!pkg_target_cpu_matches("x86_64-aros-v2-native", "x86_64"), "malformed target cannot pass CPU check");
    check(pkg_target_cpu_matches("x86_64", "x86_64"), "legacy CPU matching preserved");
    check(pkg_target_cpu_matches("ppc64", "ppc64"), "legacy CPU outside qualified set preserved");
    check(!pkg_target_cpu_matches("generic", "x86_64"), "executable cannot become generic data");
    check(!pkg_target_cpu_matches(NULL, "x86_64") && !pkg_target_cpu_matches("x86_64", NULL), "NULL CPU inputs rejected");
    check(!pkg_target_cpu_matches("", ""), "empty CPU inputs rejected");
    check(pkg_target_format(&a, text, 2) == -1 && !text[0], "small format buffer refused and cleared");
    check(pkg_target_format(&a, NULL, 0) == -1, "NULL format buffer refused");
    check(pkg_target_format(NULL, text, sizeof text) == -1 && !text[0], "NULL structure refused");
    memset(&a, 'x', sizeof a);
    check(pkg_target_format(&a, text, sizeof text) == -1 && !text[0], "unterminated structure fields refused");
    printf("target: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
