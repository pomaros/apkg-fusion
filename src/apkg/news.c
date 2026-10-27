/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
struct apkg_news {
    apkg_news_view view;
    apkg_news_entry *entries;
    const apkg_channel *current;
    apkg_channel *previous;
};
static int text_order(apkg_text a, apkg_text b) {
    size_t n=a.size<b.size?a.size:b.size;
    int code=n?memcmp(a.data,b.data,n):0;
    return code?code:a.size<b.size?-1:a.size>b.size;
}
static int same_application(const apkg_channel_entry_view *a,const apkg_channel_entry_view *b) {
    return !text_order(a->authority,b->authority)&&!text_order(a->name,b->name);
}
static int same_bytes(apkg_bytes a,apkg_bytes b) {
    return a.size==b.size&&(!a.size||!memcmp(a.data,b.data,a.size));
}
static int news_order(const void *a,const void *b) {
    const apkg_news_entry *x=a,*y=b;
    int code=(int)x->kind-(int)y->kind;
    if(!code)code=text_order(x->package->name,y->package->name);
    if(!code)code=text_order(x->package->authority,y->package->authority);
    return code?code:text_order(x->package->target,y->package->target);
}
void apkg_news_free(apkg_news *news) {
    if(news){free(news->entries);apkg_channel_free(news->previous);free(news);}
}
const apkg_news_view *apkg_news_get(const apkg_news *news){return news?&news->view:NULL;}
const char *apkg_news_kind_name(apkg_news_kind kind){
    static const char *const names[]={"new-application","new-build","updated","same-release-change","recent-publication"};
    return (unsigned)kind<sizeof names/sizeof names[0]?names[kind]:"unknown";
}
apkg_status apkg_channel_news(const apkg_snapshot *snapshot,const apkg_channel *current,
    apkg_bytes previous,apkg_bytes signature,apkg_news **out,apkg_error **error) {
    if(error)*error=NULL;
    if(!out)return APKG_INVALID;
    *out=NULL;
    apkg_status code=ak_channel_snapshot_check(snapshot,current);
    if(code)return code;
    if((previous.size==0)!=(signature.size==0)||(previous.size&&!previous.data)||(signature.size&&!signature.data))return APKG_INVALID;
    apkg_news *news=calloc(1,sizeof *news);
    ak_channel_choice *now=NULL,*before=NULL;size_t nn=0,bn=0;
    apkg_recent *recent=NULL;
    if(!news)return APKG_NOMEM;
    news->current=current;
    if(!previous.size){
        code=apkg_channel_recent(current,NULL,&recent,error);
        if(code)goto done;
        const apkg_recent_view *r=apkg_recent_get(recent);
        news->entries=calloc(r->count?(size_t)r->count:1,sizeof *news->entries);
        if(!news->entries){code=APKG_NOMEM;goto done;}
        news->view.unknown_dates=r->unknown_dates;news->view.indeterminate=r->indeterminate;
        for(uint64_t i=0;i<r->count;i++)
            news->entries[news->view.count++]=(apkg_news_entry){r->entries[i].index,APKG_NEWS_RECENT,
                r->entries[i].package,{0},0,0,-1};
        news->view.recent=r->count;
        goto ready;
    }
    code=ak_channel_history_read(snapshot,current,previous,signature,&news->previous,error);
    if(code)goto done;
    const apkg_channel_view *cv=apkg_channel_get(current),*pv=apkg_channel_get(news->previous);
    if(pv->sequence>cv->sequence){code=APKG_STALE;goto done;}
    if(pv->sequence==cv->sequence&&memcmp(pv->snapshot_digest.bytes,cv->snapshot_digest.bytes,32)){
        code=APKG_CONFLICT;goto done;
    }
    news->view.has_previous=1;news->view.previous_sequence=pv->sequence;
    news->view.previous_snapshot=pv->snapshot_digest;
    if(!memcmp(pv->snapshot_digest.bytes,cv->snapshot_digest.bytes,32))goto ready;
    code=ak_channel_choices(current,&now,&nn);if(code)goto done;
    code=ak_channel_choices(news->previous,&before,&bn);if(code)goto done;
    news->entries=calloc(nn?nn:1,sizeof *news->entries);
    if(!news->entries){code=APKG_NOMEM;goto done;}
    for(size_t i=0;i<nn;i++){
        if(now[i].ambiguous){news->view.indeterminate++;continue;}
        const apkg_channel_entry_view *r=now[i].row;
        const ak_channel_choice *old=NULL;
        for(size_t j=0;j<bn;j++)if(ak_entry_same_group(r,before[j].row)){old=&before[j];break;}
        apkg_news_entry entry={now[i].index,APKG_NEWS_APPLICATION,r,{0},0,0,-1};
        if(!old){
            int existed=0;
            for(uint64_t j=0;j<pv->entry_count;j++){
                const apkg_channel_entry_view *other=NULL;
                code=apkg_channel_entry_at(news->previous,j,&other,error);if(code)goto done;
                if(same_application(r,other)){existed=1;break;}
            }
            if(existed){entry.kind=APKG_NEWS_BUILD;news->view.builds++;}
            else{news->view.applications++;}
            if(r->app_first_added.size&&pv->published_on.size)
                entry.earlier_publication=text_order(r->app_first_added,pv->published_on)<0;
        }else{
            if(old->ambiguous){news->view.indeterminate++;continue;}
            const apkg_channel_entry_view *p=old->row;
            int same=!memcmp(r->descriptor_digest.bytes,p->descriptor_digest.bytes,32)&&
                !memcmp(r->archive_digest.bytes,p->archive_digest.bytes,32)&&
                r->archive_bytes==p->archive_bytes&&same_bytes(r->signature_envelope,p->signature_envelope);
            if(same)continue;
            int order=ak_entry_version_order(r,p);
            if(order==2){news->view.indeterminate++;continue;}
            entry.previous_version=p->version;entry.previous_revision=p->revision;
            if(!order){entry.kind=APKG_NEWS_CONTENT_CHANGED;news->view.content_changed++;}
            else{entry.kind=APKG_NEWS_UPDATED;entry.direction=order;news->view.updated++;}
        }
        news->entries[news->view.count++]=entry;
    }
    qsort(news->entries,(size_t)news->view.count,sizeof *news->entries,news_order);
ready:
    news->view.entries=news->entries;
    *out=news;news=NULL;code=APKG_OK;
done:
    free(now);free(before);apkg_recent_free(recent);apkg_news_free(news);
    if(code&&error&&!*error)code=ak_fail(error,code,"source comparison refused",NULL);
    return code;
}
apkg_status apkg_news_format(const apkg_news *news,int json,char **out,apkg_error **error){
    if(error)*error=NULL;
    if(!out)return APKG_INVALID;
    *out=NULL;if(!news||(json!=0&&json!=1))return APKG_INVALID;
    ak_buf b={0};b.max=AK_MAX;
    const apkg_channel_view *v=apkg_channel_get(news->current);
    const apkg_news_view *n=&news->view;
    char id[33],sha[65],old[65],seq[32]="null";
    ak_tohex(v->id.bytes,16,id);ak_tohex(v->snapshot_digest.bytes,32,sha);
    ak_tohex(n->previous_snapshot.bytes,32,old);
    if(n->has_previous)snprintf(seq,sizeof seq,"\"%" PRIu64 "\"",n->previous_sequence);
    if(json)ak_buf_add(&b,"{\"channel_id\":\"%s\",\"sequence\":\"%" PRIu64 "\",\"snapshot_sha256\":\"%s\","
        "\"has_previous\":%s,\"previous_sequence\":%s,\"previous_snapshot_sha256\":%s%s%s,"
        "\"applications\":\"%" PRIu64 "\",\"builds\":\"%" PRIu64 "\",\"updated\":\"%" PRIu64 "\","
        "\"content_changed\":\"%" PRIu64 "\",\"recent\":\"%" PRIu64 "\",\"unknown_dates\":\"%" PRIu64 "\","
        "\"indeterminate\":\"%" PRIu64 "\",\"freshness\":\"unknown\",\"entries\":[",id,v->sequence,sha,
        n->has_previous?"true":"false",seq,n->has_previous?"\"":"",n->has_previous?old:"null",n->has_previous?"\"":"",
        n->applications,n->builds,n->updated,n->content_changed,n->recent,n->unknown_dates,n->indeterminate);
    else ak_buf_add(&b,"%s\nChannel: %s\nSequence: %" PRIu64 "\nSnapshot: %s\n"
        "Package changes: %" PRIu64 " applications, %" PRIu64 " target builds, %" PRIu64 " versions, %" PRIu64
        " same-release changes; %" PRIu64 " indeterminate groups. Freshness unknown.\n",
        n->has_previous?"Compared authenticated observations":"First observation: no comparison; recent publisher reports only",
        id,v->sequence,sha,n->applications,n->builds,n->updated,n->content_changed,n->indeterminate);
    for(uint64_t i=0;i<n->count;i++){
        const apkg_news_entry *e=&n->entries[i];const apkg_channel_entry_view *r=e->package;
        char digest[65];ak_tohex(r->descriptor_digest.bytes,32,digest);
        char previous_revision[32]="null";
        if(e->previous_version.size)snprintf(previous_revision,sizeof previous_revision,"\"%" PRIu64 "\"",e->previous_revision);
        const char *direction=e->kind==APKG_NEWS_UPDATED?(e->direction<0?"\"older\"":"\"newer\""):"null";
        if(json){ak_buf_add(&b,"%s{\"kind\":\"%s\",\"authority\":\"%.*s\",\"name\":\"%.*s\",\"target\":\"%.*s\","
            "\"version\":\"%.*s\",\"revision\":\"%" PRIu64 "\",\"descriptor_sha256\":\"%s\","
            "\"previous_version\":%s%.*s%s,\"previous_revision\":%s,\"direction\":%s,\"earlier_publication\":%s",
            i?",":"",apkg_news_kind_name(e->kind),(int)r->authority.size,r->authority.data,(int)r->name.size,r->name.data,
            (int)r->target.size,r->target.data,(int)r->version.size,r->version.data,r->revision,digest,
            e->previous_version.size?"\"":"",e->previous_version.size?(int)e->previous_version.size:4,
            e->previous_version.size?e->previous_version.data:"null",e->previous_version.size?"\"":"",
            previous_revision,direction,e->earlier_publication<0?"null":e->earlier_publication?"true":"false");
            ak_buf_add(&b,",\"app_first_added\":%s%.*s%s,\"first_added\":%s%.*s%s}",
                r->app_first_added.size?"\"":"",r->app_first_added.size?10:4,
                r->app_first_added.size?r->app_first_added.data:"null",r->app_first_added.size?"\"":"",
                r->first_added.size?"\"":"",r->first_added.size?10:4,
                r->first_added.size?r->first_added.data:"null",r->first_added.size?"\"":"");
        }else{ak_buf_add(&b,"%s %.*s/%.*s %.*s revision %" PRIu64 " target %.*s descriptor %s\n",apkg_news_kind_name(e->kind),
            (int)r->authority.size,r->authority.data,(int)r->name.size,r->name.data,
            (int)r->version.size,r->version.data,r->revision,(int)r->target.size,r->target.data,digest);
            if(e->previous_version.size)ak_buf_add(&b,"  Previous: %.*s revision %" PRIu64 "; direction: %s\n",
                (int)e->previous_version.size,e->previous_version.data,e->previous_revision,
                e->direction<0?"older":e->direction>0?"newer":"same release");
            else if(e->kind==APKG_NEWS_RECENT)ak_buf_add(&b,"  Publisher-reported application day: %.*s\n",
                (int)r->app_first_added.size,r->app_first_added.data);
        }
    }
    if(json)ak_buf_add(&b,"]}\n");
    apkg_status code=b.bad;if(!code){*out=b.data;b.data=NULL;}free(b.data);
    return code?ak_fail(error,code,"source comparison formatting refused",NULL):APKG_OK;
}
