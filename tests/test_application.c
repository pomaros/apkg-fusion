/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */
#include "pkg_application.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char valid[] =
    "{\"schema\":1,\"id\":\"folio\",\"version\":\"0.4.1\",\"revision\":2,"
    "\"targets\":[{\"os\":\"aros\",\"arch\":\"x86_64\",\"abi\":\"v11\"}],"
    "\"requires_system\":[{\"type\":\"library\",\"id\":\"muimaster.library\",\"min_version\":19}],"
    "\"launch\":{\"executable\":\"bin/Folio\",\"mode\":\"workbench\"}}";
static unsigned checks;

static void refuses(const char *text, size_t len)
{
    struct pkg_application a, zero;
    char why[256];
    memset(&a, 0xa5, sizeof a);
    memset(&zero, 0, sizeof zero);
    assert(pkg_application_parse(text, len, &a, why, sizeof why) == -1);
    assert(why[0]);
    assert(!memcmp(&a, &zero, sizeof a));
    checks++;
}
static void replaced(const char *old, const char *replacement, int accepted)
{
    char text[2048], why[256];
    struct pkg_application a;
    const char *p = strstr(valid, old);
    assert(p);
    snprintf(text, sizeof text, "%.*s%s%s", (int)(p - valid), valid, replacement, p + strlen(old));
    if (accepted) {
        assert(pkg_application_parse(text, strlen(text), &a, why, sizeof why) == 0);
        checks++;
    } else refuses(text, strlen(text));
}
int main(void)
{
    struct pkg_application a;
    char why[256], text[2048], requirements[2048];
    const char *unsafe[] = { "", "/abs", "Work:Foo", "../Foo", "a/../Foo", "a//Foo",
        "a/./Foo", "a/", "a\\Foo", "a\nFoo", "a\"Foo", "a\177Foo", "caf\303\251", NULL };
    unsigned char elf[64] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    size_t i;
    assert(pkg_application_parse(valid, strlen(valid), &a, why, sizeof why) == 0);
    assert(!strcmp(a.id, "folio") && !strcmp(a.name, "folio") && !strcmp(a.version, "0.4.1"));
    assert(a.revision == 2 && !strcmp(a.os, "aros") && !strcmp(a.arch, "x86_64") && !strcmp(a.abi, "v11"));
    assert(a.mode == PKG_APPLICATION_WORKBENCH && !strcmp(a.executable, "bin/Folio"));
    assert(!strcmp(a.working_directory, "bin") && a.nrequirements == 1);
    assert(!strcmp(a.requirements[0].id, "muimaster.library") && a.requirements[0].min_version == 19);
    checks++;
    replaced("\"mode\":\"workbench\"", "\"mode\":\"shell\",\"working_directory\":\".\"", 1);
    replaced("\"mode\":\"workbench\"", "\"mode\":\"workbench\",\"working_directory\":\"bin\"", 1);
    replaced("\"id\":\"folio\"", "\"id\":\"folio\",\"name\":\"Caf\\u00e9 \\ud83d\\udc08\"", 1);
    replaced("bin/Folio", "bin\\u002fFolio", 1);
    replaced("\"mode\"", "\"\\u006dode\"", 1);
    replaced("\"schema\":1", "\"schema\":1,\"presentation\":{\"value\":[true,false,null,1.2e-3]}", 1);
    replaced("\"schema\":1", "\"schema\":1,\"schema\":1", 0);
    replaced("\"mode\":\"workbench\"", "\"mode\":\"workbench\",\"\\u006dode\":\"shell\"", 0);
    replaced("\"mode\":\"workbench\"", "\"mode\":\"workbench\",\"args\":\"evil\"", 0);
    replaced("\"mode\":\"workbench\"", "\"mode\":\"workbench\",\"working_directory\":\".\"", 0);
    replaced("\"mode\":\"workbench\"", "\"mode\":\"guess\"", 0);
    replaced("\"mode\":\"workbench\"", "\"mode\":null", 0);
    replaced("\"schema\":1", "\"schema\":1.0", 0);
    replaced("\"schema\":1", "\"schema\":1e0", 0);
    replaced("\"schema\":1", "\"schema\":2", 0);
    replaced("\"schema\":1", "\"schema\":\"1\"", 0);
    replaced("\"schema\":1", "\"schema\":true", 0);
    replaced("\"revision\":2", "\"revision\":-1", 0);
    replaced("\"revision\":2", "\"revision\":99999999999999999999999999999", 0);
    replaced("\"version\":\"0.4.1\"", "\"version\":\"\"", 0);
    replaced("\"os\":\"aros\"", "\"os\":\"linux\"", 0);
    replaced("\"arch\":\"x86_64\"", "\"arch\":false", 0);
    replaced("\"abi\":\"v11\"", "\"abi\":\"\"", 0);
    replaced("\"abi\":\"v11\"", "\"abi\":\"v11\",\"runtime\":\"unknown\"", 0);
    replaced("[{\"os\":\"aros\",\"arch\":\"x86_64\",\"abi\":\"v11\"}]", "[]", 0);
    replaced("[{\"os\":\"aros\",\"arch\":\"x86_64\",\"abi\":\"v11\"}]", "[{},{}]", 0);
    replaced("[{\"type\":\"library\",\"id\":\"muimaster.library\",\"min_version\":19}]", "{}", 0);
    replaced("[{\"type\":\"library\",\"id\":\"muimaster.library\",\"min_version\":19}]", "[1]", 0);
    replaced("\"min_version\":19", "\"min_version\":-1", 0);
    replaced("\"min_version\":19", "\"min_version\":\"19\"", 0);
    replaced("\"min_version\":19", "\"min_version\":19,\"optional\":true", 0);
    replaced("bin/Folio", "../Folio", 0);
    replaced("bin/Folio", "\\u002e\\u002e/Folio", 0);
    replaced("bin/Folio", "Folio.info", 0);
    replaced("bin/Folio", ".info", 0);
    replaced("bin/Folio", ".", 0);
    replaced("bin/Folio", "bin\\u0000/Folio", 0);
    replaced("bin/Folio", "bin\\nFolio", 0);
    replaced("bin/Folio", "bin\\xFolio", 0);
    replaced("bin/Folio", "bin\\ud800Folio", 0);
    replaced("bin/Folio", "bin\\udc00Folio", 0);
    replaced("bin/Folio", "bin\300\257Folio", 0);
    replaced("bin/Folio", "bin\355\240\200Folio", 0);
    replaced("\"schema\":1,", "\"schema\" 1,", 0);
    replaced("\"schema\":1,", "\"schema\":1 ", 0);
    replaced("\"schema\":1,", "\"schema\":01,", 0);
    replaced("\"schema\":1,", "\"schema\":1,,", 0);
    replaced("\"mode\":\"workbench\"", "\"mode\":\"workbench\",", 0);
    snprintf(text, sizeof text, "%s {}", valid);
    refuses(text, strlen(text));
    memcpy(text, valid, sizeof valid); text[sizeof valid] = 'x';
    refuses(text, strlen(valid) + 2);
    strcpy(requirements, "[");
    for (i = 0; i < PKG_APPLICATION_MAX_REQUIREMENTS; i++) {
        if (i) strcat(requirements, ",");
        strcat(requirements, "{\"type\":\"library\",\"id\":\"mui.library\"}");
    }
    strcat(requirements, "]");
    replaced("[{\"type\":\"library\",\"id\":\"muimaster.library\",\"min_version\":19}]", requirements, 1);
    requirements[strlen(requirements) - 1] = 0;
    strcat(requirements, ",{\"type\":\"library\",\"id\":\"mui.library\"}]");
    replaced("[{\"type\":\"library\",\"id\":\"muimaster.library\",\"min_version\":19}]", requirements, 0);
    for (i = 0; i < strlen(valid); i++) refuses(valid, i);
    refuses(NULL, 10);
    refuses(valid, PKG_APPLICATION_MAX_SIZE + 1);
    for (i = 0; unsafe[i]; i++) {
        assert(pkg_application_path_valid(unsafe[i], why, sizeof why) == -1);
        checks++;
    }
    assert(!pkg_application_path_valid("a folder/My Program", NULL, 0));
    assert(!pkg_application_path_valid(".", NULL, 0));
    assert(pkg_application_parse(NULL, 0, NULL, NULL, 0) == -1);
    memset(text, '[', 40);
    memset(text + 40, ']', 40);
    refuses(text, 80);
    elf[18] = 62;
    assert(!strcmp(pkg_application_binary_arch(elf, sizeof elf), "x86_64"));
    elf[5] = 2; elf[18] = 0; elf[19] = 183;
    assert(!strcmp(pkg_application_binary_arch(elf, sizeof elf), "aarch64"));
    elf[5] = 0;
    assert(!*pkg_application_binary_arch(elf, sizeof elf));
    assert(!*pkg_application_binary_arch(elf, 4));
    assert(!*pkg_application_binary_arch(NULL, 64));
    assert(!strcmp(pkg_application_binary_arch((const unsigned char *)"\0\0\3\363", 4), "m68k"));
    printf("application: %u parsing and path checks passed\n", checks);
    return 0;
}
