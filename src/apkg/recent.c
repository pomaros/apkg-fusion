/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <inttypes.h>
#include <string.h>

struct apkg_recent {
    apkg_recent_view view;
    apkg_recent_entry *entries;
    const apkg_channel *channel;
};
int ak_day_valid(apkg_text text) {
    if (!text.data || text.size != 10) return 0;
    const char *s = text.data;
    for (size_t i = 0; i < 10; i++)
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    unsigned year = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + s[3]-'0';
    unsigned month = (s[5]-'0')*10 + s[6]-'0', day = (s[8]-'0')*10 + s[9]-'0';
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!year || !month || month > 12 || !day) return 0;
    unsigned max = days[month-1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) max++;
    return day <= max;
}
static int text_order(apkg_text a, apkg_text b) {
    size_t n = a.size < b.size ? a.size : b.size;
    int code = n ? memcmp(a.data,b.data,n) : 0;
    return code ? code : a.size < b.size ? -1 : a.size > b.size;
}
int ak_entry_same_group(const apkg_channel_entry_view *a, const apkg_channel_entry_view *b) {
    return !text_order(a->authority,b->authority) && !text_order(a->name,b->name) &&
        !text_order(a->target,b->target);
}
int ak_entry_version_order(const apkg_channel_entry_view *a, const apkg_channel_entry_view *b) {
    char av[256], bv[256];
    memcpy(av,a->version.data,a->version.size); av[a->version.size]=0;
    memcpy(bv,b->version.data,b->version.size); bv[b->version.size]=0;
    int code = ak_version_compare(av,bv);
    if (!code) code = a->revision < b->revision ? -1 : a->revision > b->revision;
    return code;
}
apkg_status ak_channel_choices(const apkg_channel *channel, ak_channel_choice **out, size_t *n) {
    if (!out || !n) return APKG_INVALID;
    *out = NULL; *n = 0;
    if (!channel) return APKG_INVALID;
    uint64_t count = apkg_channel_get(channel)->entry_count;
    if (count > 4096) return APKG_LIMIT;
    const apkg_channel_entry_view **rows = calloc(count ? (size_t)count : 1,sizeof *rows);
    ak_channel_choice *choices = calloc(count ? (size_t)count : 1,sizeof *choices);
    apkg_status code = APKG_NOMEM;
    if (!rows || !choices) goto done;
    for (uint64_t i=0;i<count;i++) {
        code = apkg_channel_entry_at(channel,i,&rows[i],NULL);
        if (code) goto done;
    }
    for (uint64_t i=0;i<count;i++) {
        if (!rows[i]->target_matches) continue;
        int seen=0;
        for (uint64_t j=0;j<i;j++)
            if (rows[j]->target_matches && ak_entry_same_group(rows[i],rows[j])) {seen=1;break;}
        if (seen) continue;
        uint64_t best=i;int ambiguous=0;
        for(uint64_t j=i+1;j<count;j++) {
            if (!rows[j]->target_matches || !ak_entry_same_group(rows[i],rows[j])) continue;
            int order=ak_entry_version_order(rows[j],rows[best]);
            if(order==2)ambiguous=1;else if(order>0)best=j;
        }
        choices[(*n)++]=(ak_channel_choice){best,rows[best],ambiguous};
    }
    *out=choices;choices=NULL;code=APKG_OK;
done:
    free(rows);free(choices);if(code)*n=0;return code;
}
static int recent_order(const void *a, const void *b) {
    const apkg_recent_entry *x = a, *y = b;
    int code = -text_order(x->date,y->date);
    if (!code) code = text_order(x->package->name,y->package->name);
    if (!code) code = text_order(x->package->authority,y->package->authority);
    if (!code) code = text_order(x->package->target,y->package->target);
    return code;
}
void apkg_recent_free(apkg_recent *recent) {
    if (recent) { free(recent->entries); free(recent); }
}
const apkg_recent_view *apkg_recent_get(const apkg_recent *recent) {
    return recent ? &recent->view : NULL;
}
apkg_status apkg_channel_recent(const apkg_channel *channel,
    const apkg_recent_options *supplied, apkg_recent **out, apkg_error **error) {
    if (error) *error = NULL;
    if (!out) return APKG_INVALID;
    *out = NULL;
    if (!channel) return APKG_INVALID;
    apkg_recent_options defaults = {0}; defaults.limit = 25;
    const apkg_recent_options *options = supplied ? supplied : &defaults;
    if ((options->builds != 0 && options->builds != 1) ||
        (options->since.size && !ak_day_valid(options->since))) return APKG_INVALID;
    uint64_t count = apkg_channel_get(channel)->entry_count;
    if (count > 4096 || options->limit > 4096) return APKG_LIMIT;
    apkg_recent *recent = calloc(1,sizeof *recent);
    ak_channel_choice *choices = NULL; size_t groups = 0;
    apkg_status code = APKG_NOMEM;
    if (!recent) goto done;
    code = ak_channel_choices(channel,&choices,&groups);
    if(code)goto done;
    recent->entries = calloc(groups ? groups : 1,sizeof *recent->entries);
    if (!recent->entries) {code=APKG_NOMEM;goto done;}
    recent->channel = channel; recent->view.entries = recent->entries;
    for (size_t i = 0; i < groups; i++) {
        if (choices[i].ambiguous) { recent->view.indeterminate++; continue; }
        const apkg_channel_entry_view *row = choices[i].row;
        if (!row->app_first_added.size) { recent->view.unknown_dates++; continue; }
        apkg_recent_kind kind = APKG_RECENT_APPLICATION;
        apkg_text date = row->app_first_added;
        if (row->first_added.size && text_order(row->first_added,date) > 0) {
            if (!options->builds) continue;
            kind = APKG_RECENT_BUILD; date = row->first_added;
        }
        if (options->since.size && text_order(date,options->since) < 0) continue;
        recent->entries[recent->view.count++] = (apkg_recent_entry){choices[i].index,kind,date,row};
    }
    qsort(recent->entries,(size_t)recent->view.count,sizeof *recent->entries,recent_order);
    if (options->limit && recent->view.count > options->limit) recent->view.count = options->limit;
    *out = recent; recent = NULL; code = APKG_OK;
done:
    free(choices); apkg_recent_free(recent);
    return code ? ak_fail(error,code,"recent publication query refused",NULL) : APKG_OK;
}
apkg_status apkg_recent_format(const apkg_recent *recent, int json,
    char **out, apkg_error **error) {
    if (error) *error = NULL;
    if (!out) return APKG_INVALID;
    *out = NULL;
    if (!recent || (json != 0 && json != 1)) return APKG_INVALID;
    ak_buf b = {0}; b.max = AK_MAX;
    const apkg_channel_view *channel = apkg_channel_get(recent->channel);
    char identity[33], snapshot[65];
    ak_tohex(channel->id.bytes,16,identity); ak_tohex(channel->snapshot_digest.bytes,32,snapshot);
    if (json) ak_buf_add(&b,"{\"channel_id\":\"%s\",\"snapshot_sha256\":\"%s\",\"sequence\":\"%" PRIu64
        "\",\"date_source\":\"channel-publisher\",\"freshness\":\"unknown\",\"unknown_dates\":\"%" PRIu64
        "\",\"indeterminate\":\"%" PRIu64 "\",\"entries\":[",identity,snapshot,channel->sequence,
        recent->view.unknown_dates,recent->view.indeterminate);
    else ak_buf_add(&b,"Publisher-reported publication dates; freshness unknown; runtime not asserted.\n"
        "Channel: %s\nSequence: %" PRIu64 "\nSnapshot: %s\nUnknown dates: %" PRIu64
        "\nIndeterminate build groups: %" PRIu64 "\n",identity,channel->sequence,snapshot,
        recent->view.unknown_dates,recent->view.indeterminate);
    for (uint64_t i = 0; i < recent->view.count; i++) {
        const apkg_recent_entry *item = &recent->entries[i];
        const apkg_channel_entry_view *r = item->package;
        char digest[65]; ak_tohex(r->descriptor_digest.bytes,32,digest);
        const char *kind = item->kind == APKG_RECENT_BUILD ? "new-build" : "recently-added";
        if (json) ak_buf_add(&b,"%s{\"authority\":\"%.*s\",\"name\":\"%.*s\",\"target\":\"%.*s\","
            "\"version\":\"%.*s\",\"revision\":\"%" PRIu64 "\",\"descriptor_sha256\":\"%s\","
            "\"kind\":\"%s\",\"date\":\"%.*s\"}",i ? "," : "",
            (int)r->authority.size,r->authority.data,(int)r->name.size,r->name.data,
            (int)r->target.size,r->target.data,(int)r->version.size,r->version.data,
            r->revision,digest,kind,(int)item->date.size,item->date.data);
        else ak_buf_add(&b,"%s %.*s %.*s/%.*s %.*s revision %" PRIu64 " target %.*s descriptor %s\n",
            kind,(int)item->date.size,item->date.data,(int)r->authority.size,r->authority.data,
            (int)r->name.size,r->name.data,(int)r->version.size,r->version.data,r->revision,
            (int)r->target.size,r->target.data,digest);
    }
    if (json) ak_buf_add(&b,"]}\n");
    apkg_status code = b.bad;
    if (!code) { *out = b.data; b.data = NULL; }
    free(b.data);
    return code ? ak_fail(error,code,"recent publication formatting refused",NULL) : APKG_OK;
}
