/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "internal.h"
#include "pkg_ameta.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *parent;
    struct pkg_ameta data;
} source_meta;
static int meta_cmp(const void *a, const void *b) {
    return strcmp(((const source_meta *)a)->parent,
                  ((const source_meta *)b)->parent);
}
static int control_path(const char *path) {
    const char *name = strrchr(path, '/');
    return !strcmp(name ? name + 1 : path, ".ameta");
}
static apkg_status convert(const struct pkg_ameta_entry *e, uint64_t mode,
                           char **out) {
    ak_buf b = {0};
    ak_json j = {0};
    apkg_status code = APKG_OK;
    char *comment = NULL;
    b.max = 16384;
    *out = NULL;
    if (e->comment_len > 158)
        return APKG_UNSUPPORTED;
    comment = malloc(e->comment_len * 2 + 1);
    if (!comment)
        return APKG_NOMEM;
    ak_tohex(e->comment, e->comment_len, comment);
    ak_buf_add(&b, "{\"comment\":\"%s\",\"extra\":[", comment);
    free(comment);
    for (size_t i = 0; i < e->nunknown; i++) {
        size_t kn = strlen(e->unknown[i].key), vn = strlen(e->unknown[i].value);
        if (kn > 255 || vn > 4096) {
            code = APKG_LIMIT;
            goto done;
        }
        char key[511], value[8193];
        ak_tohex((const unsigned char *)e->unknown[i].key, kn, key);
        ak_tohex((const unsigned char *)e->unknown[i].value, vn, value);
        ak_buf_add(&b, "%s{\"key\":\"%s\",\"value\":\"%s\"}", i ? "," : "", key,
                   value);
    }
    ak_buf_add(&b, "],\"gid\":");
    if (e->has_gid)
        ak_buf_add(&b, "\"%lu\"", e->gid);
    else
        ak_buf_add(&b, "null");
    ak_buf_add(&b, ",\"protection\":\"%llu\",\"uid\":",
               pkg_ameta_publish_word(e, 1, (mode & 0100) != 0));
    if (e->has_uid)
        ak_buf_add(&b, "\"%lu\"", e->uid);
    else
        ak_buf_add(&b, "null");
    ak_buf_add(&b, "}");
    if (b.bad) {
        code = (apkg_status)b.bad;
        goto done;
    }
    int parsed = ak_json_parse(b.data, b.size, &j);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    if (!ak_attributes_valid(&j, 0, mode)) {
        code = errno == ENOMEM ? APKG_NOMEM : APKG_UNSUPPORTED;
        goto done;
    }
    code = ak_attributes_normalize(b.data, mode, out);
done:
    ak_json_free(&j);
    free(b.data);
    return code;
}
/* Source observation is read-only. .ameta is interpreted only here: ordinary
 * installed-tree scans never guess that a personal file is authoring control.
 */
apkg_status ak_source_inventory(const char *base, char **out,
                                apkg_error **error) {
    struct stat identity;
    char *raw = NULL;
    ak_json tree = {0};
    ak_buf result = {0};
    source_meta *metadata = NULL;
    size_t count = 0, used = 0, control_bytes = 0;
    int arr;
    apkg_status code;
    *out = NULL;
    if (error)
        *error = NULL;
    result.max = AK_MAX / 8;
    if (!base || strlen(base) >= AK_PATH ||
        !ak_text_path_valid(base, strlen(base)))
        return APKG_INVALID;
    code = ak_tree_source_read(base, result.max, &raw, &identity);
    if (code)
        goto done;
    int parsed = ak_json_parse(raw, strlen(raw), &tree);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    arr = ak_member(&tree, 0, "files");
    if (arr < 0) {
        code = APKG_NOT_FOUND;
        goto done;
    }
    for (size_t i = (size_t)arr + 1; i < tree.tokens[arr].next;
         i = tree.tokens[i].next) {
        char path[AK_PATH];
        ak_string(&tree, ak_member(&tree, (int)i, "path"), path, sizeof path);
        if (control_path(path))
            count++;
    }
    metadata = calloc(count ? count : 1, sizeof *metadata);
    if (!metadata) {
        code = APKG_NOMEM;
        goto done;
    }
    for (size_t i = (size_t)arr + 1; i < tree.tokens[arr].next;
         i = tree.tokens[i].next) {
        char path[AK_PATH], full[AK_PATH];
        unsigned char *bytes = NULL;
        size_t size = 0;
        ak_string(&tree, ak_member(&tree, (int)i, "path"), path, sizeof path);
        if (!control_path(path))
            continue;
        if (!ak_equal(&tree, ak_member(&tree, (int)i, "type"), "file") ||
            ak_path(full, base, path)) {
            code = APKG_UNSUPPORTED;
            goto done;
        }
        char *slash = strrchr(path, '/');
        if (slash)
            *slash = 0;
        else
            *path = 0;
        source_meta *m = &metadata[used++];
        m->parent = ak_copy(path, strlen(path));
        if (!m->parent) {
            code = APKG_NOMEM;
            goto done;
        }
        if (ak_read(full, &bytes, &size, 1024 * 1024)) {
            free(bytes);
            code = errno == ENOMEM ? APKG_NOMEM : APKG_IO;
            goto done;
        }
        if (size > result.max - control_bytes) {
            free(bytes);
            code = APKG_LIMIT;
            goto done;
        }
        control_bytes += size;
        pkg_ameta_init(&m->data);
        int read = pkg_ameta_parse(bytes, size, &m->data);
        free(bytes);
        if (read) {
            code = APKG_NOMEM;
            goto done;
        }
        if (!m->data.usable || m->data.nr) {
            code = APKG_INVALID;
            goto done;
        }
        for (size_t e = 0; e < m->data.n; e++) {
            struct pkg_ameta_entry *entry = &m->data.e[e];
            char rel[AK_PATH];
            if (entry->name_len >= AK_PATH) {
                code = APKG_LIMIT;
                goto done;
            }
            char name[AK_PATH];
            memcpy(name, entry->name, entry->name_len);
            name[entry->name_len] = 0;
            if (entry->name_len != strlen(name) || !ak_valid_path(name) ||
                strchr(name, '/') || !strcmp(name, ".ameta") ||
                (*m->parent ? ak_path(rel, m->parent, name)
                            : (strcpy(rel, name), 0)) ||
                ak_tree_find(&tree, rel) < 0) {
                code = APKG_INVALID;
                goto done;
            }
        }
    }
    if (used > 1)
        qsort(metadata, used, sizeof *metadata, meta_cmp);
    ak_buf_add(&result, "{");
    int rootattrs = ak_member(&tree, 0, "amiga");
    if (rootattrs >= 0)
        ak_buf_add(
            &result, "\"amiga\":%.*s,",
            (int)(tree.tokens[rootattrs].end - tree.tokens[rootattrs].start),
            tree.text + tree.tokens[rootattrs].start);
    /* Bind source snapshots to the actual control bytes as well as their
     * effective attributes. Controls are not payload entries. */
    ak_buf_add(&result, "\"controls\":[");
    size_t control_count = 0;
    for (size_t i = (size_t)arr + 1; i < tree.tokens[arr].next;
         i = tree.tokens[i].next) {
        char path[AK_PATH], sha[65], size[24];
        ak_string(&tree, ak_member(&tree, (int)i, "path"), path, sizeof path);
        if (!control_path(path))
            continue;
        ak_string(&tree, ak_member(&tree, (int)i, "sha256"), sha, sizeof sha);
        ak_string(&tree, ak_member(&tree, (int)i, "size"), size, sizeof size);
        ak_buf_add(&result,
                   "%s{\"path\":\"%s\",\"sha256\":\"%s\",\"size\":\"%s\"}",
                   control_count++ ? "," : "", path, sha, size);
    }
    ak_buf_add(&result, "],\"files\":[");
    size_t emitted = 0;
    for (size_t i = (size_t)arr + 1; i < tree.tokens[arr].next;
         i = tree.tokens[i].next) {
        char path[AK_PATH], parent[AK_PATH];
        char *attributes = NULL;
        ak_string(&tree, ak_member(&tree, (int)i, "path"), path, sizeof path);
        if (control_path(path))
            continue;
        strcpy(parent, path);
        char *slash = strrchr(parent, '/');
        const char *name = slash ? slash + 1 : parent;
        char leaf[AK_PATH];
        strcpy(leaf, name);
        if (slash)
            *slash = 0;
        else
            *parent = 0;
        source_meta lookup = {parent, {0}};
        source_meta *m =
            used ? bsearch(&lookup, metadata, used, sizeof *metadata, meta_cmp)
                 : NULL;
        struct pkg_ameta_entry *e =
            m ? pkg_ameta_find(&m->data, (const unsigned char *)leaf,
                               strlen(leaf))
              : NULL;
        uint64_t mode;
        ak_u64(&tree, ak_member(&tree, (int)i, "mode"), &mode);
        if (mode > 0777 || !(mode & 0400) ||
            (ak_equal(&tree, ak_member(&tree, (int)i, "type"), "directory") &&
             !(mode & 0100))) {
            code = APKG_UNSUPPORTED;
            goto done;
        }
        if (e) {
            code = convert(e, mode, &attributes);
            if (code)
                goto done;
            int existing = ak_member(&tree, (int)i, "amiga");
            if (existing >= 0) {
                char *live = ak_slice(&tree, existing);
                if (!live) {
                    free(attributes);
                    code = APKG_NOMEM;
                    goto done;
                }
                int same = attributes && !strcmp(live, attributes);
                free(live);
                if (!same) {
                    free(attributes);
                    code = APKG_CONFLICT;
                    goto done;
                }
            }
        }
        ak_buf_add(&result, "%s{", emitted++ ? "," : "");
        if (attributes)
            ak_buf_add(&result, "\"amiga\":%s,", attributes);
        int existing = ak_member(&tree, (int)i, "amiga");
        size_t start = tree.tokens[i].start + 1;
        if (attributes && existing >= 0)
            start = tree.tokens[existing].end + 1;
        ak_buf_add(&result, "%.*s", (int)(tree.tokens[i].end - start),
                   tree.text + start);
        free(attributes);
        if (result.bad) {
            code = (apkg_status)result.bad;
            goto done;
        }
    }
    uint64_t rootmode;
    ak_u64(&tree, ak_member(&tree, 0, "mode"), &rootmode);
    ak_buf_add(&result, "],\"mode\":\"%" PRIu64 "\"}", rootmode);
    code = (apkg_status)result.bad;
    if (!code) {
        *out = result.data;
        result.data = NULL;
    }
done:
    for (size_t i = 0; i < used; i++) {
        free(metadata[i].parent);
        pkg_ameta_free(&metadata[i].data);
    }
    free(metadata);
    free(raw);
    free(result.data);
    ak_json_free(&tree);
    if (code && error)
        return ak_fail(error, code,
                       "source inventory or .ameta cannot be preserved",
                       base ? base : "");
    return code;
}

/* Parent controls describe siblings outside the selected payload too. Parse
 * their syntax, but select only the named entry; never inventory other files.
 */
apkg_status ak_source_entry(const char *path, char **out, apkg_error **error) {
    char parent[AK_PATH], control[AK_PATH], *raw = NULL, *attributes = NULL,
                                            *live = NULL;
    unsigned char *bytes = NULL;
    size_t size = 0;
    struct pkg_ameta metadata;
    ak_json entry = {0};
    ak_buf result = {0};
    apkg_status code = APKG_INVALID;
    *out = NULL;
    if (error)
        *error = NULL;
    pkg_ameta_init(&metadata);
    result.max = 32768;
    if (!path || strlen(path) >= AK_PATH ||
        !ak_text_path_valid(path, strlen(path)))
        goto done;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    const char *name = slash ? slash + 1 : parent;
    char leaf[AK_PATH];
    strcpy(leaf, name);
    if (!ak_valid_path(leaf) || strchr(leaf, '/') || !strcmp(leaf, ".ameta"))
        goto done;
    if (slash) {
        if (slash == parent)
            slash[1] = 0;
        else
            *slash = 0;
    } else
        strcpy(parent, ".");
    code = ak_tree_entry(path, leaf, &raw);
    if (code)
        goto done;
    int parsed = ak_json_parse(raw, strlen(raw), &entry);
    if (parsed) {
        code = parsed == -2 ? APKG_NOMEM : APKG_INVALID;
        goto done;
    }
    if (ak_path(control, parent, ".ameta")) {
        code = APKG_LIMIT;
        goto done;
    }
    int present = ak_exists(control);
    if (present < 0) {
        code = APKG_IO;
        goto done;
    }
    if (present) {
        if (ak_read(control, &bytes, &size, 1024 * 1024)) {
            code = errno == ENOMEM  ? APKG_NOMEM
                   : errno == EFBIG ? APKG_LIMIT
                                    : APKG_IO;
            goto done;
        }
        if (pkg_ameta_parse(bytes, size, &metadata)) {
            code = APKG_NOMEM;
            goto done;
        }
        if (!metadata.usable || metadata.nr) {
            code = APKG_INVALID;
            goto done;
        }
        struct pkg_ameta_entry *selected = pkg_ameta_find(
            &metadata, (const unsigned char *)leaf, strlen(leaf));
        if (selected) {
            uint64_t mode;
            ak_u64(&entry, ak_member(&entry, 0, "mode"), &mode);
            code = convert(selected, mode, &attributes);
            if (code)
                goto done;
            int at = ak_member(&entry, 0, "amiga");
            if (at >= 0) {
                live = ak_slice(&entry, at);
                if (!live) {
                    code = APKG_NOMEM;
                    goto done;
                }
                if (!attributes || strcmp(live, attributes)) {
                    code = APKG_CONFLICT;
                    goto done;
                }
            }
            ak_buf_add(&result, "{");
            if (attributes)
                ak_buf_add(&result, "\"amiga\":%s,", attributes);
            size_t start = at >= 0 ? entry.tokens[at].end + 1 : 1;
            ak_buf_add(&result, "%s", raw + start);
        }
    }
    if (!result.data)
        ak_buf_add(&result, "%s", raw);
    code = (apkg_status)result.bad;
    if (!code) {
        *out = result.data;
        result.data = NULL;
    }
done:
    free(raw);
    free(attributes);
    free(live);
    free(bytes);
    free(result.data);
    pkg_ameta_free(&metadata);
    ak_json_free(&entry);
    if (code && error)
        return ak_fail(error, code, "selected source entry cannot be preserved",
                       path ? path : "");
    return code;
}
