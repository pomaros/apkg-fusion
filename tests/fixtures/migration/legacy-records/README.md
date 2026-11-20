<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Installed records from each arospkg release line

`aros/openloco-0.1-line.json` was written on AROS (ABIv11, QEMU) on 2026-09-25 by
an apkg of the 0.1 line, which records no `subdir`, `revision`, `abi` or
`previous`. Its package is the public OpenLoco 0.1.0-rc1 build.

`host-stand-in/` holds records written by the unchanged `libpkg` and `apkg`
sources of each release tag, compiled for the host against a POSIX stand-in for
the DOS calls. They show the record format of each line; they are not AROS
evidence. Each line installed `mini` 1.0 (with a drawer icon), and every line
with an upgrade command upgraded it. From 0.6.0 on, `mini` depends on `minilib`.

| Lines | Record fields |
|---|---|
| 0.1 | no `revision`, `abi`, `subdir`, `previous`; icon without `member` |
| 0.2 | adds `revision`, `subdir`, `previous`, icon `member` |
| 0.3.1 to 0.5.0 | adds `abi` |
| 0.6.0 to 0.7.5 | adds `relations_schema`, and `depends` when not empty |

arospkg 0.7.5 reads all of these and writes none of them again: running its
read and write commands over each root left every record byte-identical.
Updating to the last classic release therefore does not normalize old records.
