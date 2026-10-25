#ifndef PKG_LIMITS_H
#define PKG_LIMITS_H
/* What one package may cost, in one place. Every download and every
 * extraction is checked against these; an archive over any of them is
 * refused before a byte is written.
 *
 * A single member may be larger than the download: compressed programs
 * expand. Up to 0.5.0 a member was limited to 64 MiB because the ZIP reader
 * held the whole member, compressed and expanded, in memory. It now inflates
 * through fixed 64 KiB buffers, so the member limit no longer decides how
 * much memory an install takes, and it is 128 MiB: statically linked Rust
 * programs with their fonts built in (the ArtCraft ports, 2026-10) are 80-95
 * MiB. LHA archives are still read whole, so their members stay at 64 MiB. */
#define PKG_MAX_DOWNLOAD        (64L * 1024 * 1024)   /* one archive, as downloaded */
#define PKG_MAX_MEMBER          (128L * 1024 * 1024)  /* one file, unpacked (ZIP) */
#define PKG_MAX_MEMBER_BUFFERED (64L * 1024 * 1024)   /* one file, unpacked (LHA, held in memory) */
#define PKG_MAX_TOTAL           (256L * 1024 * 1024)  /* all files of one archive, unpacked */
#endif
