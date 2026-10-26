/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 * Exercise the actual imported core with a host-only DOS adapter. */
#define _POSIX_C_SOURCE 200809L
#include "pkg.h"
#include "internal.h"
#include "util.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int writes, recoveries, fail_listing;
void net_set_slow(int n) { (void)n; }
void net_set_break(long n, int how) { (void)n; (void)how; }
struct Task *FindTask(void *x) { static struct Task t; (void)x; return &t; }
int Rename(CONST_STRPTR a, CONST_STRPTR b) { (void)a; (void)b; writes++; return 0; }
int u_mkdir(const char *p) { (void)p; writes++; return 0; }
int u_mkpath(const char *p) { (void)p; writes++; return 0; }
int u_delete(const char *p) { (void)p; writes++; return 0; }
int u_deltree(const char *p) { (void)p; writes++; return 0; }
int u_exists(const char *p) { struct stat st; return stat(p,&st)==0; }
int u_isdir(const char *p) { struct stat st; return stat(p,&st)==0 && S_ISDIR(st.st_mode); }
long u_size(const char *p) { struct stat st; return stat(p,&st)==0 ? (long)st.st_size : -1; }
void u_join(char *o,size_t n,const char *a,const char *b) { snprintf(o,n,"%s/%s",a,b); }
char *u_read_all(const char *p,size_t *n) { (void)p; (void)n; return NULL; }
int u_listdir(const char *p,char ***out,int *count)
{
    DIR *dir; struct dirent *entry; char **v=NULL; int n=0;
    if (fail_listing) return -1;
    dir=opendir(p);
    if (!dir) return -1;
    while ((entry=readdir(dir))) {
        char **next;
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        next=realloc(v,(size_t)(n+1)*sizeof *v); assert(next); v=next;
        v[n]=strdup(entry->d_name); assert(v[n]); n++;
    }
    closedir(dir); *out=v; *count=n; return 0;
}
void u_freelist(char **v,int n) { int i; for(i=0;i<n;i++) free(v[i]); free(v); }
void pkg_recover_all(struct pkg_ctx *c) { (void)c; recoveries++; }
void rel_finish_replacements(struct pkg_ctx *c) { (void)c; recoveries++; }
static void make_dir(const char *p) { assert(mkdir(p,0700)==0); }
static void path(char *out,size_t n,const char *a,const char *b)
{ int r=snprintf(out,n,"%s/%s",a,b); assert(r>0 && (size_t)r<n); }
int main(void)
{
    char base[]="/tmp/apkg-context-XXXXXX", a[512], b[512], p[512], q[512];
    pkg_ctx *ca=NULL,*cb=NULL; pkg_err e;
    assert(mkdtemp(base)); path(a,sizeof a,base,"absent");
    assert(pkg_open(a,PKG_OPEN_READ,&ca,&e)==PKG_OK);
    assert(!u_exists(a)); assert(writes==0 && recoveries==0);
    assert(pkg_pending_txns(ca)==0);
    path(b,sizeof b,base,"present"); make_dir(b);
    path(p,sizeof p,b,"db"); make_dir(p);
    path(q,sizeof q,p,"transactions"); make_dir(q);
    path(p,sizeof p,q,"pending"); make_dir(p);
    assert(pkg_open(b,PKG_OPEN_READ,&cb,&e)==PKG_OK);
    assert(pkg_pending_txns(cb)==1 && pkg_pending_txns(ca)==0);
    assert(writes==0 && recoveries==0);
    pkg_close(cb); cb=NULL; fail_listing=1;
    assert(pkg_open(b,PKG_OPEN_READ,&cb,&e)==PKG_E_IO);
    assert(cb==NULL && writes==0 && recoveries==0);
    fail_listing=0;
    pkg_close(ca); pkg_close(cb);
    assert(writes==0 && recoveries==0 && !u_exists(a));
    assert(rmdir(p)==0 && rmdir(q)==0);
    path(p,sizeof p,b,"db"); assert(rmdir(p)==0 && rmdir(b)==0 && rmdir(base)==0);
    puts("arospkg read-only contexts: absent root, independent roots, pending transaction, failed listing and no writes/recovery passed");
    return 0;
}
