#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Exercise libpkg's actual AROS atomic-write path and a refusing handler."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'src/pkg_fs_posix.c').read_text()
def function(name):
    start=source.rindex(name)
    opening=source.index('{',start)
    depth=1;end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
prefix=r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define __AROS__ 1
typedef const char *CONST_STRPTR;
struct Library { unsigned lib_Version, lib_Revision; } library = {50,84};
static struct Library *DOSBase = &library;
#define __aros_getbase_DOSBase() (DOSBase)
static int refuse, packets;
#define ReplaceObject ReplaceObject
static int ReplaceObject(const char *from,const char *to) {
 char old[4]={0};int fd=open(to,O_RDONLY);
 assert(fd>=0);assert(read(fd,old,3)==3);close(fd);assert(!strcmp(old,"old"));
 packets++;if(refuse)return 0;return rename(from,to)==0;
}
static int mkparents(const char *path) { (void)path;return 0; }
static int pkg_fs_random(void *data,size_t n) { (void)data;(void)n;return -1; }
'''
suffix=r'''
static void seed(void) { FILE *f=fopen("prefs","w");assert(f);assert(fwrite("old",1,3,f)==3);assert(!fclose(f)); }
static void contents(const char *wanted) { char b[8]={0};FILE *f=fopen("prefs","r");assert(f);assert(fread(b,1,3,f)==3);fclose(f);assert(!strcmp(b,wanted)); }
int main(void) {
 seed();library.lib_Revision=83;assert(!pkg_fs_atomic_replace_supported());assert(write_atomic_mode("prefs","new",3,0644)==-1);assert(errno==ENOTSUP&&packets==0);contents("old");
 library.lib_Version=49;library.lib_Revision=99;assert(write_atomic_mode("prefs","new",3,0644)==-1);assert(errno==ENOTSUP&&packets==0);contents("old");
 DOSBase=NULL;assert(write_atomic_mode("prefs","new",3,0644)==-1);assert(errno==ENOTSUP&&packets==0);contents("old");DOSBase=&library;
 library.lib_Version=50;library.lib_Revision=84;assert(pkg_fs_atomic_replace_supported());
 seed();refuse=1;assert(write_atomic_mode("prefs","new",3,0644)==-1);contents("old");assert(packets==1);
 refuse=0;assert(write_atomic_mode("prefs","new",3,0644)==0);contents("new");assert(packets==2);
 puts("libpkg AROS publication: PASS replacement_packets=2 missing_destination_observations=0 failure_target_loss=0 old_DOS_vector_calls=0");
}
'''
actual='\n'.join(function(name) for name in ('int pkg_fs_atomic_replace_supported(', 'static int replace_file(', 'static int open_tmp_beside(', 'static int write_atomic_mode('))
# A forward declaration precedes the implementation in the source.
if 'malloc' not in actual:
    raise SystemExit('missing production write implementation')
with tempfile.TemporaryDirectory(prefix='pkg-atomic-aros-') as directory:
    work=Path(directory);(work/'test.c').write_text(prefix+actual+suffix)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],cwd=work,check=True)

# A missing admission check must expose the unsafe old-DOS publication path.
start=actual.index('    const struct Library *dos =',actual.index('static int replace_file('))
end=actual.index('#ifdef ReplaceObject',start)
unguarded=actual[:start]+actual[end:]
with tempfile.TemporaryDirectory(prefix='pkg-atomic-aros-revision-negative-') as directory:
    work=Path(directory);(work/'test.c').write_text(prefix+unguarded+suffix)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(work/'test.c'),'-o',str(work/'test')],check=True)
    result=subprocess.run([str(work/'test')],cwd=work,capture_output=True,text=True)
    assert result.returncode != 0, 'old DOS admission control did not reject a removed guard'
    print('libpkg DOS revision negative control: PASS removed guard rejected')

# An older SDK may have no ReplaceObject declaration/vector at all. It must
# compile and refuse safely even on a runtime claiming a sufficiently new DOS.
no_sdk_prefix=prefix.replace('#define ReplaceObject ReplaceObject\n','')
a=no_sdk_prefix.index('static int ReplaceObject(')
b=no_sdk_prefix.index('static int mkparents(',a)
no_sdk_prefix=no_sdk_prefix[:a]+no_sdk_prefix[b:]
no_sdk_suffix=suffix[:suffix.index('int main(void)')]+r"""
int main(void) {
 seed();library.lib_Version=51;library.lib_Revision=0;refuse=0;
 assert(!pkg_fs_atomic_replace_supported());
 assert(write_atomic_mode("prefs","new",3,0644)==-1);
 assert(errno==ENOTSUP&&packets==0);contents("old");
 puts("libpkg AROS missing SDK operation: PASS preserved destination without vector call");
}
"""
with tempfile.TemporaryDirectory(prefix='pkg-atomic-aros-no-sdk-') as directory:
    work=Path(directory);(work/'test.c').write_text(no_sdk_prefix+actual+no_sdk_suffix)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],cwd=work,check=True)
