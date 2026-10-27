/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#include "apkg_client.h"
#include "internal.h"
#include "pkg_fs.h"
#include <errno.h>
#include <string.h>
#define CONFIG_MAX (16u * 1024u * 1024u)
static const unsigned char magic[] = "APKG-SOURCE\0\1";
#define HEADER (sizeof magic - 1 + 16 + 12)
struct apkg_source_config {
    apkg_source_config_view view;
    unsigned char *bytes;
    char endpoint[4096];
};
static uint32_t get32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put32(unsigned char *p, size_t n) {
    for (int i = 3; i >= 0; i--) {
        p[i] = (unsigned char)n;
        n >>= 8;
    }
}
static int endpoint_valid(const char *s, size_t n) {
    if (!n || n >= 4096)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 || s[i] == 127)
            return 0;
    return 1;
}
static apkg_status io_status(void) {
    return errno == ENOENT   ? APKG_NOT_FOUND
           : errno == ENOMEM ? APKG_NOMEM
           : errno == EFBIG  ? APKG_LIMIT
                             : APKG_IO;
}
void apkg_source_config_free(apkg_source_config *c) {
    if (c) {
        free(c->bytes);
        free(c);
    }
}
const apkg_source_config_view *
apkg_source_config_get(const apkg_source_config *c) {
    return c ? &c->view : NULL;
}
static apkg_status decode_config(apkg_source_config *c,size_t size) {
    if(size<HEADER||memcmp(c->bytes,magic,sizeof magic-2))return APKG_INVALID;
    unsigned version=c->bytes[sizeof magic-2];
    if(version!=1&&version!=2)return APKG_INVALID;
    size_t header=HEADER+(version==2?8:0);
    if(size<header)return APKG_INVALID;
    const unsigned char *lengths=c->bytes+sizeof magic-1+16;
    size_t n[5]={get32(lengths),get32(lengths+4),get32(lengths+8),0,0};
    if(version==2){n[3]=get32(lengths+12);n[4]=get32(lengths+16);}
    size_t remaining=size-header;
    for(size_t i=0;i<5;i++){if(n[i]>remaining)return APKG_INVALID;remaining-=n[i];}
    if(remaining||!endpoint_valid((const char *)c->bytes+header,n[0])||
        ((n[1]==0)!=(n[2]==0))||((n[3]==0)!=(n[4]==0))||(n[3]&&!n[1]))return APKG_INVALID;
    memcpy(c->view.id.bytes,c->bytes+sizeof magic-1,16);
    memcpy(c->endpoint,c->bytes+header,n[0]);
    c->view.endpoint=(apkg_text){c->endpoint,n[0]};
    size_t at=header+n[0];
    apkg_bytes *parts[]={&c->view.observation,&c->view.signature,&c->view.previous_observation,&c->view.previous_signature};
    for(size_t i=0;i<4;i++){*parts[i]=(apkg_bytes){c->bytes+at,n[i+1]};at+=n[i+1];}
    return APKG_OK;
}
apkg_status apkg_source_config_read(const char *path,apkg_source_config **out,apkg_error **error){
    if(error)*error=NULL;
    if(!out)return APKG_INVALID;
    *out=NULL;
    if(!path||!*path||strlen(path)>=AK_PATH)return APKG_INVALID;
    apkg_source_config *c=calloc(1,sizeof *c);if(!c)return APKG_NOMEM;
    size_t size=0;apkg_status code=APKG_OK;
    if(ak_read(path,&c->bytes,&size,CONFIG_MAX))code=io_status();
    else code=decode_config(c,size);
    if(!code){*out=c;c=NULL;}
    apkg_source_config_free(c);
    return code?ak_fail(error,code,"source profile refused",path):APKG_OK;
}
static apkg_status write_config(const char *path,const char *endpoint,const apkg_id *identity,
    const apkg_channel *channel,apkg_bytes previous,apkg_bytes previous_signature,
    apkg_source_config **out,apkg_error **error){
    if(error)*error=NULL;
    if(out)*out=NULL;
    if(!path||!*path||strlen(path)>=AK_PATH||!endpoint||!identity||!endpoint_valid(endpoint,strlen(endpoint))||
        ((previous.size==0)!=(previous_signature.size==0))||(!channel&&previous.size)||
        (previous.size&&!previous.data)||(previous_signature.size&&!previous_signature.data))return APKG_INVALID;
    if(!pkg_fs_atomic_replace_supported())return APKG_UNSUPPORTED;
    apkg_bytes object={0},signature={0};apkg_status code=APKG_OK;
    if(channel){
        if(memcmp(identity->bytes,apkg_channel_get(channel)->id.bytes,16))return APKG_CONFLICT;
        code=apkg_channel_observation(channel,&object,&signature);if(code)return code;
    }
    size_t n=strlen(endpoint),header=HEADER+(previous.size?8:0),total=header+n;
    apkg_bytes parts[]={object,signature,previous,previous_signature};
    for(size_t i=0;i<4;i++){if(parts[i].size>CONFIG_MAX-total)return APKG_LIMIT;total+=parts[i].size;}
    apkg_source_config *c=calloc(1,sizeof *c);if(!c)return APKG_NOMEM;
    c->bytes=malloc(total);if(!c->bytes){apkg_source_config_free(c);return APKG_NOMEM;}
    memcpy(c->bytes,magic,sizeof magic-1);c->bytes[sizeof magic-2]=previous.size?2:1;
    memcpy(c->bytes+sizeof magic-1,identity->bytes,16);
    unsigned char *lengths=c->bytes+sizeof magic-1+16;
    put32(lengths,n);
    for(size_t i=0;i<(previous.size?4:2);i++)put32(lengths+4+i*4,parts[i].size);
    memcpy(c->bytes+header,endpoint,n);size_t at=header+n;
    for(size_t i=0;i<4;i++){if(parts[i].size)memcpy(c->bytes+at,parts[i].data,parts[i].size);at+=parts[i].size;}
    code=decode_config(c,total);
    if(!code){
        if(pkg_fs_path_is_link(path)!=0)code=APKG_IO;
        else if(pkg_fs_write_private(path,c->bytes,total))code=io_status();
    }
    if(!code&&out){*out=c;c=NULL;}
    apkg_source_config_free(c);
    return code?ak_fail(error,code,"source profile save failed",path):APKG_OK;
}
apkg_status apkg_source_config_save(const char *path,const char *endpoint,const apkg_id *id,
    const apkg_channel *channel,apkg_error **error){
    return write_config(path,endpoint,id,channel,(apkg_bytes){0},(apkg_bytes){0},NULL,error);
}
apkg_status apkg_source_config_save_pair(const char *path,const char *endpoint,const apkg_id *id,
    const apkg_channel *channel,apkg_bytes previous,apkg_bytes signature,apkg_error **error){
    return write_config(path,endpoint,id,channel,previous,signature,NULL,error);
}
apkg_status apkg_source_config_refresh(const char *path,const char *endpoint,const apkg_id *id,
    const apkg_snapshot *snapshot,const apkg_channel *channel,apkg_source_config **out,apkg_error **error){
    if(error)*error=NULL;
    if(!out)return APKG_INVALID;
    *out=NULL;
    if(!path||!endpoint||!id||!snapshot||!channel)return APKG_INVALID;
    if(memcmp(id->bytes,apkg_channel_get(channel)->id.bytes,16))return APKG_CONFLICT;
    apkg_source_config *old=NULL;apkg_news *news=NULL;
    apkg_bytes previous={0},signature={0};
    apkg_status code=apkg_source_config_read(path,&old,error);
    if(code==APKG_NOT_FOUND){apkg_error_free(error?*error:NULL);if(error)*error=NULL;code=APKG_OK;}
    if(code)goto done;
    if(old&&!strcmp(old->endpoint,endpoint)&&!memcmp(old->view.id.bytes,id->bytes,16)){
        previous=old->view.observation;signature=old->view.signature;
    }
    code=apkg_channel_news(snapshot,channel,previous,signature,&news,error);
    if(!code)code=write_config(path,endpoint,id,channel,previous,signature,out,error);
done:
    apkg_news_free(news);apkg_source_config_free(old);return code;
}
apkg_status apkg_source_config_open(const apkg_source_config *config,
                                    const char *root,
                                    apkg_snapshot **snapshot_out,
                                    apkg_channel **channel_out,
                                    apkg_error **error) {
    if (error)
        *error = NULL;
    if (snapshot_out)
        *snapshot_out = NULL;
    if (channel_out)
        *channel_out = NULL;
    if (!config || !root || !snapshot_out || !channel_out ||
        (void *)snapshot_out == (void *)channel_out)
        return APKG_INVALID;
    if (!config->view.observation.size)
        return APKG_NOT_FOUND;
    apkg_open_options options = {0};
    options.contract_version = APKG_CONTRACT_VERSION;
    options.struct_size = sizeof options;
    options.root = (apkg_text){root, strlen(root)};
    apkg_ctx *ctx = NULL;
    apkg_snapshot *snapshot = NULL;
    apkg_channel *channel = NULL;
    apkg_status code = apkg_open(&options, &ctx, error);
    if (!code)
        code = apkg_channel_read(ctx, &config->view.id, 0,
                                 config->view.observation,
                                 config->view.signature, &channel, error);
    if (!code)
        code = apkg_snapshot_read(ctx, &snapshot, error);
    if (!code)
        code = ak_channel_snapshot_capture(snapshot, channel);
    if (!code) {
        *snapshot_out = snapshot;
        snapshot = NULL;
        *channel_out = channel;
        channel = NULL;
    }
    apkg_snapshot_free(snapshot);
    apkg_channel_free(channel);
    apkg_close(ctx);
    return code;
}
