#ifndef AROSPKG_ABOUT_H
#define AROSPKG_ABOUT_H

#define AROSPKG_AUTHOR "Tomasz Staniak"
/* In three parts so PkgManager can show the address as a link between
   them; apkg --about prints them joined, as before. */
#define AROSPKG_ABOUT_HEAD \
    "Find, install and manage software for AROS.\n\n" \
    "By Tomasz Staniak"
#define AROSPKG_HOMEPAGE "https://tomaszstaniak.com/arospkg/"
#define AROSPKG_ABOUT_TAIL \
    "Copyright (c) 2026 Tomasz Staniak\n" \
    "arospkg is free software under the MIT licence.\n" \
    "Third-party components have their own licences;\n" \
    "see the licenses directory included with the download."
#define AROSPKG_ABOUT \
    AROSPKG_ABOUT_HEAD "\n" AROSPKG_HOMEPAGE "\n\n" AROSPKG_ABOUT_TAIL

#endif
