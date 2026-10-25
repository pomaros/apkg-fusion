# Updating arospkg

In the Shell:

```
apkg self-update --check
apkg self-update
```

In PkgManager: **About** > **Check for a newer version**, then **Update
arospkg**.

Both update apkg and the PkgManager of the same installation to the same
release. Your packages, the catalogue, your package root and your icons
stay as they are.

arospkg 0.6.0 and earlier update apkg only; see [From 0.6.0 and
earlier](#from-060-and-earlier).

## What is updated

Where arospkg is installed decides what is replaced:

| Installation | What is updated |
|---|---|
| The release archive, unpacked into a drawer | `apkg`, `PkgManager`, `BUILD.txt` and `application.json` in that drawer, where they are |
| Installed by apkg as the package `arospkg` | the whole package, as `apkg upgrade` would; `apkg verify arospkg` stays clean |
| apkg copied elsewhere, such as `C:apkg` | that copy, together with the installation it belongs to |
| apkg on its own, without PkgManager | that apkg; no PkgManager is added |

A file the release does not contain is left as it is. Icons (`.info`) are
never replaced in a drawer you unpacked; in an installed package, an icon
you changed (its position or tool types) is kept.

The previous version stays: as `<file>.old` beside each replaced file, or
for an installed package as its previous revision (`apkg rollback
arospkg`).

## Which installation an apkg belongs to

apkg started from a drawer with PkgManager in it belongs to that drawer. A
copy elsewhere, usually `C:apkg`, belongs to whichever installation you say,
and apkg remembers it in `ENVARC:arospkg/copies.json`. It never searches
your disks for copies, and it never guesses:

- When the copy is not recorded and an installation is in sight (the
  package `arospkg` in the package root, or a drawer a running PkgManager
  comes from), apkg lists them and asks which one, or none. In a script it
  stops, changes nothing and says how to answer.
- `apkg self-update --drawer "<drawer>"` ties the copy to that drawer.
- `apkg self-update --apkg-only` updates the copy on its own.
- With nothing recorded and nothing in sight, only the copy is updated, and
  apkg says how to include PkgManager.

When you update from an installation's drawer, or from PkgManager, and
`C:apkg` is not recorded with any installation, you are asked once whether
it belongs to this one.

## In PkgManager

**Check for a newer version** shows the latest stable release and the
versions of PkgManager, the apkg beside it and the copies tied to it. When
any of them is behind the release, and none is newer, **Update arospkg**:

1. downloads and checks the release; the progress and **Cancel** are at the
   bottom of the main window, which keeps responding;
2. shows what will be updated and that PkgManager will close and start
   again;
3. after **Proceed**, closes PkgManager; apkg from the same installation
   replaces the files and starts PkgManager again, from its icon if you
   started it that way, otherwise with the same root and options;
4. PkgManager then says what happened.

If PkgManager cannot be started again, a requester says so, apart from
whether the update itself succeeded. Start it from its drawer.

Cancelling at any point before **Proceed** leaves everything as it was.

## What is checked

- The release is the latest **stable** release on
  https://github.com/tomaszstaniak/arospkg/releases (never a pre-release).
- The archive is the one for this CPU, ABI and system. A Raspberry Pi build
  takes only Raspberry Pi archives, and a hosted aarch64 build never takes
  them. If the release has none for your machine, `--check` says so and
  nothing changes.
- The archive must match the release's `SHA256SUMS`, and apkg and PkgManager
  must match the archive's `BUILD.txt` and be built for this machine.
  Everything is downloaded and checked before any file is replaced. These
  files come from the same release; releases are not signed.
- Every place a file goes must accept a new file and a rename, and must have
  room, before PkgManager closes or anything is replaced.

## When it refuses

Nothing is changed when:

- an apkg or PkgManager operation is running in the package root (install,
  upgrade, rollback, refresh). Try again when it has finished;
- PkgManager is running from the installation a Shell update would replace.
  Close it, or update from its About window. With two PkgManager windows
  open, close the other one;
- an installed file is newer than the latest stable release;
- a previous backup (`<file>.old`) is protected from deletion;
- an earlier update did not finish: settle it first (see below);
- the package `arospkg` changed while the update was downloading, for
  example because `apkg upgrade arospkg` ran meanwhile. Check again;
- the choice of installation for a copy cannot be recorded.

## Going back

For a drawer you unpacked, or a copy:

```
Delete "Work:arospkg/PkgManager"
Rename "Work:arospkg/PkgManager.old" "Work:arospkg/PkgManager"
```

for each file the update listed. For the package `arospkg`:

```
apkg rollback arospkg
```

## If an update was interrupted

Before replacing anything, the update writes `ENVARC:arospkg/self-update.json`
with the files and their checksums, and keeps each new file beside its
target as `<file>.new`. If the machine stops halfway:

- the next `apkg self-update`, or `apkg self-update --recover`, finishes
  the update when every new file is still there, and otherwise puts the
  previous files back;
- PkgManager, when it starts, offers **Repair now**, which does the same and
  starts it again.

If neither is possible from what is on the disk, nothing is touched and
apkg prints, file by file, what to rename. An installed package is first
settled by its own recovery, as for any interrupted package operation; if
the root's lock was left by the stopped run, `apkg unlock` clears it.

## From 0.6.0 and earlier

These versions replace only the apkg you start. Run `apkg self-update` once
more after it, and the new apkg updates PkgManager too. If you run it as
`C:apkg`, say which drawer it belongs to the first time:

```
apkg self-update --drawer "Work:arospkg-0.6.0.x86_64-aros-v11"
```

arospkg 0.3.2 and earlier have no `self-update`: install a newer release by
hand from its archive, as the README describes.

## Limits

- A resident apkg (`Resident`) is not updated; start it from its file.
- It needs the network and the certificate bundle, as downloads do.
- A PkgManager from 0.6.0 or earlier does not say where it runs from, so
  while one is open a Shell update refuses until it is closed.
