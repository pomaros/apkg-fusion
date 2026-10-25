/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

#include "pkg_target.h"

#include <stdio.h>
#include <string.h>

static int one_of(const char *value, const char *const *choices)
{
    size_t i;
    for (i = 0; choices[i]; ++i)
        if (!strcmp(value, choices[i])) return 1;
    return 0;
}

static int fail(char *why, size_t size, const char *reason)
{
    if (why && size) snprintf(why, size, "%s", reason);
    return -1;
}

static int part(const char **cursor, char *out, size_t size, int final)
{
    const char *s = *cursor;
    size_t n = 0;
    while (s[n] && s[n] != '-') ++n;
    if (!n || n >= size || (final ? s[n] != '\0' : s[n] != '-')) return -1;
    memcpy(out, s, n);
    out[n] = '\0';
    *cursor = s + n + (final ? 0 : 1);
    return 0;
}

int pkg_target_parse(const char *text, struct pkg_target *out,
                     char *why, size_t why_size)
{
    static const char *const cpus[] = { "x86_64", "i386", "aarch64", "arm", "m68k", "ppc", NULL };
    static const char *const abis[] = { "v0", "v1", "v11", NULL };
    static const char *const runtimes[] = { "native", "hosted", "raspi", NULL };
    struct pkg_target t;
    const char *p = text;
    size_t n;
    if (out) memset(out, 0, sizeof *out);
    if (why && why_size) why[0] = '\0';
    if (!text) return fail(why, why_size, "provide CPU-aros-ABI-runtime");
    for (n = 0; n <= PKG_TARGET_MAX && text[n]; ++n) {}
    if (!n || n > PKG_TARGET_MAX)
        return fail(why, why_size, "target must contain 1 to 31 characters");
    memset(&t, 0, sizeof t);
    if (part(&p, t.cpu, sizeof t.cpu, 0) || part(&p, t.os, sizeof t.os, 0)
        || part(&p, t.abi, sizeof t.abi, 0) || part(&p, t.runtime, sizeof t.runtime, 1))
        return fail(why, why_size, "target must be CPU-aros-ABI-runtime");
    if (!one_of(t.cpu, cpus)) return fail(why, why_size, "unknown target CPU");
    if (strcmp(t.os, "aros")) return fail(why, why_size, "target OS must be aros");
    if (!one_of(t.abi, abis)) return fail(why, why_size, "target ABI must be v0, v1 or v11");
    if (!one_of(t.runtime, runtimes))
        return fail(why, why_size, "target runtime must be native, hosted or raspi");
    if (!strcmp(t.runtime, "raspi") && strcmp(t.cpu, "arm") && strcmp(t.cpu, "aarch64"))
        return fail(why, why_size, "raspi requires an arm or aarch64 CPU");
    if (out) *out = t;
    return 0;
}

int pkg_target_format(const struct pkg_target *target, char *out, size_t size)
{
    char text[PKG_TARGET_MAX + 1];
    int n;
    if (out && size) out[0] = '\0';
    if (!target || !out || !size
        || !memchr(target->cpu, '\0', sizeof target->cpu)
        || !memchr(target->os, '\0', sizeof target->os)
        || !memchr(target->abi, '\0', sizeof target->abi)
        || !memchr(target->runtime, '\0', sizeof target->runtime)) return -1;
    n = snprintf(text, sizeof text, "%s-%s-%s-%s", target->cpu, target->os, target->abi, target->runtime);
    if (n < 0 || (size_t)n >= sizeof text || (size_t)n >= size
        || pkg_target_parse(text, NULL, NULL, 0)) return -1;
    memcpy(out, text, (size_t)n + 1);
    return 0;
}

int pkg_target_matches(const struct pkg_target *a, const struct pkg_target *b)
{
    char left[PKG_TARGET_MAX + 1], right[PKG_TARGET_MAX + 1];
    return !pkg_target_format(a, left, sizeof left)
        && !pkg_target_format(b, right, sizeof right) && !strcmp(left, right);
}

int pkg_target_cpu_matches(const char *declared, const char *binary_cpu)
{
    struct pkg_target target;
    if (!declared || !*declared || !binary_cpu || !*binary_cpu) return 0;
    if (!strchr(declared, '-')) return !strcmp(declared, binary_cpu);
    return !pkg_target_parse(declared, &target, NULL, 0) && !strcmp(target.cpu, binary_cpu);
}
