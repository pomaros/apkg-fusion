/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define APKG_ALLOC_IMPL
#include "internal.h"
#ifdef APKG_TESTING
#include <errno.h>
static unsigned until_failure;
static size_t largest_allocation;
size_t apkg_test_max_alloc(int reset) {
    size_t value = largest_allocation;
    if (reset)
        largest_allocation = 0;
    return value;
}
void apkg_test_fail_alloc(unsigned nth) { until_failure = nth; }
static int fail(void) {
    if (until_failure && --until_failure == 0) {
        errno = ENOMEM;
        return 1;
    }
    return 0;
}
void *ak_test_malloc(size_t n) {
    if (n > largest_allocation)
        largest_allocation = n;
    return fail() ? NULL : malloc(n);
}
void *ak_test_calloc(size_t n, size_t size) {
    if (size && n <= SIZE_MAX / size && n * size > largest_allocation)
        largest_allocation = n * size;
    return fail() ? NULL : calloc(n, size);
}
#endif
