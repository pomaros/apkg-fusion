#!/bin/sh
# Copy the Linktext class from a zunekit checkout at a commit, and record it:
#   tools/update-zunekit.sh /path/to/zunekit
# PkgManager compiles these files in; change them in zunekit, not here.
set -e
Z=${1:?usage: update-zunekit.sh ZUNEKIT}
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$HERE/third_party/zunekit
[ -z "$(git -C "$Z" status --porcelain -- src/linktext.h src/linktext_core.c src/linktext_core.h src/linktext_class.c src/linktext_private.h src/zk_linktext.c src/zk_linktext.h LICENSE)" ] \
    || { echo "zunekit has uncommitted changes in these files"; exit 1; }
mkdir -p "$T"
cp "$Z"/src/linktext.h "$Z"/src/linktext_core.c "$Z"/src/linktext_core.h "$Z"/src/linktext_class.c \
   "$Z"/src/linktext_private.h "$Z"/src/zk_linktext.c "$Z"/src/zk_linktext.h "$Z"/LICENSE "$T"/
{ echo "zunekit $(git -C "$Z" rev-parse HEAD)"
  echo "The Linktext class, compiled into PkgManager. Files as in that commit;"
  echo "change them there and run tools/update-zunekit.sh."
  (cd "$T" && shasum -a 256 *.c *.h); } > "$T/PIN"
