<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Roots written by released arospkg 0.7.4

These package roots were written by the released `apkg` of arospkg 0.7.4 for
x86_64 ABIv1 (`arospkg-0.7.4.x86_64-aros-v1.zip`, binary SHA-256
`c7e3b4525b81ffa98352631453f7525ac4fba716f2fda9ff3c39248b8442a210`) on mainline
AROS x86_64 nightly 20261105 (`dos.library` 50.86, SFS) and copied back
unchanged. The packages `mini` and `minilib` are synthetic test packages; their
archives are in each root's `cache/`, as the client cached them.

`managed-upgraded` (`SYS:MigA` on the target):

1. `install mini --yes` with a catalogue of mini 1.0 revision 2, which depends
   on `minilib >= 1.0` and requires `muimaster.library`: both installed, mini
   explicit, minilib as a dependency (`a2-install.txt`).
2. The user changed `mini/Mini.cfg`, replaced the drawer icon `mini.info` and
   added `mini/notes.txt`.
3. `upgrade mini --yes` to 1.1 revision 3 (`a2-upgrade.txt`): the changed
   configuration and the user's file are kept, the changed icon is kept, and
   the previous record and archive are retained in `db/previous` and
   `cache/previous`.
4. `verify mini` (`a2-verify.txt`).

The upgraded record keeps the installed icon's state with size `-1` after the
user replaced it: that is what the released writer produced.

`interrupted-install` (`SYS:MigB`): `install minilib`, then `install mini
--interrupt-at 3` (the client's test checkpoint after the drawer is published,
before the registry): the transaction directory, the staged icon and the
unregistered drawer remain (`b-interrupt.txt`).

Paths inside records name the target volume. `tests/apkg/test_migration_captured.py`
runs the legacy-copy rehearsal over both roots.
