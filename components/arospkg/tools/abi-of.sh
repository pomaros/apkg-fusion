#!/bin/sh
# Which ABI a pkg binary was built for, read from the binary itself rather than
# from which script was run last. The tag comes from build.c.
t=$(strings -a "$1" | sed -n 's/^arospkg-abi=//p' | head -1)
echo "${t:-unknown}"
