/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "apkg.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    char path[] = "/tmp/apkg-memory-XXXXXX";
    apkg_open_options o = {0};
    apkg_ctx *c = NULL;
    apkg_snapshot *s = NULL;
    assert(mkdtemp(path));
    o.contract_version = 1;
    o.struct_size = sizeof o;
    o.root.data = path;
    o.root.size = strlen(path);
    assert(apkg_open(&o, &c, NULL) == APKG_OK);
    assert(apkg_snapshot_read(c, &s, NULL) == APKG_OK);
    apkg_close(c);
    assert(apkg_snapshot_root(s)->state == APKG_ROOT_ABSENT);
    apkg_snapshot_free(s);
    {
        apkg_event e = {0};
        apkg_event_copy *copy = NULL;
        char subject[] = "worker data";
        e.subject.data = subject;
        e.subject.size = strlen(subject);
        e.completed = UINT64_C(4294967297);
        e.total.state = APKG_UNKNOWN;
        assert(apkg_event_clone(&e, &copy) == APKG_OK);
        subject[0] = 'X';
        assert(apkg_event_get(copy)->subject.data[0] == 'w');
        assert(apkg_event_get(copy)->completed == UINT64_C(4294967297));
        apkg_event_free(copy);
    }
    assert(rmdir(path) == 0);
    return 0;
}
