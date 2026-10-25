# Using arospkg

Install programs with apkg in the Shell or PkgManager on the desktop, start
them, keep them up to date and remove them.
[Choose the archive for your CPU and ABI](../../README.md#downloads).
0.5.0 is stable for x86_64 ABIv11 (AROS One); i386 ABIv0, mainline x86_64
ABIv1 and native Raspberry Pi aarch64 ABIv1 are experimental builds whose
test coverage is in the [release notes](../../release/notes-0.5.0.md). The
Pi build is compiled and linked, but not runtime-tested.

## Install apkg

Unpack the archive for your target. Close PkgManager and wait for any
apkg operation to finish. Enter the unpacked drawer in the Shell, then run:

```text
Copy apkg C:
apkg --version
apkg update
```

Check that the version and ABI match the archive you downloaded.
Keep your existing `SYS:Packages`. Versions 0.3.x require this manual
upgrade to 0.4.

PkgManager is included in the x86_64 ABIv11, mainline and Raspberry Pi
archives. Keep its unpacked folder on disk. To use the graphical interface,
double-click its icon or run `Run PkgManager` from that folder. Click
**Update index**, select a package and click **Install**.

## Find and install a program

```text
apkg search calculator
apkg show zunecalc
apkg install zunecalc
```

`show` tells you what it needs before anything is downloaded. After an
install, apkg prints where the program is, the command that opens its
drawer, and any notes its author left:

```text
Installed zunecalc 1.0
  Location: SYS:Packages/zunecalc
  Open folder: apkg open zunecalc
```

`apkg open zunecalc` opens that drawer in Wanderer. In PkgManager, select
the package and press **Open folder**. With `--root`, the printed command
carries the same root, so it can be copied as it is.

### What a package needs, recommends or replaces

`apkg show` lists a package's relations with their state here:

```text
  recommends:   limpet >= 0.4.1 -- full-screen front end with colours, scrollback and a clean exit; Bishop also runs in a plain Shell. not installed; the catalogue has 0.5.0 (apkg install limpet)
  replaces:     hardcode: installed: installing this package removes it (files you changed are kept)
```

When an install changes more than the package you named, apkg shows the
plan first and asks:

```text
Plan
  1. install bishop 0.3.0-test2-aros3
  2. remove (replaced) hardcode 0.3.0-test1-aros3 -- replaced by bishop; files you changed in it are kept
Go ahead? [y/N]
```

- A package it needs is installed first, from the same catalogue, for this
  system, and recorded as installed because another needs it. Installing it
  by name later records it as yours.
- A package it replaces is removed after it, as `apkg remove` removes:
  files you changed or added stay where they were.
- A conflict, either way, refuses the install and says which package to
  remove first. Nothing is removed for you.
- A recommendation is only shown. Install it yourself if you want it.

In a script there is nobody to answer, so such a plan needs `--yes`;
without it apkg changes nothing and exits with 5. `apkg --dry-run install`
shows the plan and the files; `--json` gives the plan as one object
(`ok`, `refusal`, `steps` with `action`, `id`, `version`, `note`, and after
a run `done`, `result`, `error`). Each step is its own transaction: if one
fails, the steps before it stay done and apkg lists which ran. A
replacement is recorded before it starts; if it is interrupted after the
new package is in, the next apkg that changes anything in that root removes
the old one and says so.

`apkg remove` refuses a package that another installed package needs, and
names it. Remove that one first. A package installed only because another
needed it stays installed when that one goes; `apkg show` no longer lists
anything that needs it, and you remove it yourself if you want.

What apkg does not do: install a library, a command or a font that a
program needs from the system (those are requirements, which `show`
checks); upgrade a dependency that is too old (it says which `apkg
upgrade` to run); or treat a replacement as the old package for others that
depend on it.

## Start a program

```text
apkg run folio
apkg run folio Work:Docs/manual.pdf
apkg run grep -c TODO Work:notes.txt
apkg run --check grafx2
```

A package that says how it is started can be started by name, from the
Shell or with PkgManager's **Run** button. There are two kinds:

- **Workbench programs** start exactly as a double-click on their icon
  starts them: with the stack and tool types set in the icon, in their own
  drawer. You do not need to type `Stack` first. Files after the name are
  passed as if you had dropped them on the program (Folio opens the PDF).
  apkg says `Started folio from its icon (stack 8388608 bytes from
  Folio.info)` only once the program's process has appeared; if it does
  not appear within 20 seconds, apkg says so instead of claiming success.
- **Shell programs** run in your Shell window, in the directory the package
  names, with the words after the name as their arguments. apkg returns
  the program's return code, so `apkg run grep ...` can be used in scripts.
  PkgManager starts them in a console window of their own.

`apkg run --check <id>` says whether and how it would start, and why not,
without starting anything (return code 0: it can be started, 5: it
cannot). `--json` gives the same as one JSON object.

Before starting, apkg checks the drawer's description, that the program is
there, that it is built for this machine (a description can be wrong, so
the program file itself is checked too), that a Workbench program has its
icon, and that the libraries the package names are present. It refuses
with the reason: *it does not say how to start it* (use Open folder: older
packages and many archives from AROS Archives say nothing about starting),
*its program X is missing*, *built for another system*, *a Workbench start
needs the program's icon*, or *this machine does not have X.library*.

The description is the file `application.json` in the program's drawer.
It travels with the drawer, so a drawer you unpacked or moved yourself
starts by path, without any installation records:

```text
apkg run "Work:My Apps/Folio"
```

## Keep things up to date

```text
apkg update
apkg outdated
apkg upgrade zunecalc
apkg upgrade --all
apkg self-update --check
apkg self-update
```

`update` refreshes the catalogue and ends by saying how many installed
packages have updates, and what the new catalogue brought for this system,
with the commands that list them:

```text
Updated the package catalogue: 110 packages for this system
2 updates available for installed packages. See them: apkg outdated
New applications (2): ferail, zgloom (back)
New build for this system (1): grafx2
Updated in the catalogue (3): limpet 0.4.0 -> 0.4.1, folio 0.4.1 -> 0.5, xrick 050500 -> 050500-aros1
Full list: apkg recent --since-last-update
```

See [What is new](#what-is-new) for what each line counts.

`outdated` lists them: the installed version, the version the catalogue
offers, and the state. Nothing is downloaded to make the list; it compares
your installed packages with the catalogue `update` last fetched, and says
when that was:

```text
Updates for installed packages
Catalogue updated 2026-10-11 10:50
  Package  Installed      Available      State
  upa      1.0-aros1      1.1-aros1      update available
  upe      1.0-aros1      1.1-aros1      update blocked
    this system does not provide what the newer version needs: foo.library
```

An *available* update can be started now. A *blocked* one exists but cannot
be installed on this machine: the newer build is for another CPU or ABI, a
library it requires is missing, or the catalogue has no checksum for it.
Neither is a promise that the upgrade will succeed: whether a file you
changed would be overwritten is found by the upgrade's own preview, which
needs the new archive (`apkg --dry-run --fetch upgrade <id>` shows it).

`outdated --all` lists every installed package and its state, including
those that cannot be compared: installed newer than the catalogue (apkg
never goes back to an older version), versions it cannot order, a package
the catalogue no longer lists, one it lists only for another target, or
several entries for the same target. Each comes with the reason.

`outdated --json` prints the same answer as one JSON object and nothing
else, for scripts and other programs. Its fields: `catalogue` (`state`
`ok`, `missing` or `unreadable`, `updated`, `stale`), the counts
`installed`, `newer` (updates found), `ready` (can be started now) and
`blocked`, and `packages`, one object per installed package with
`installed_version`, `available_version`, `state`, `newer`,
`requirements` and `reason`.

Exit status of `outdated`: 0 when the check ran, with or without updates;
10 when there is no catalogue or it cannot be read (run `apkg update`);
5 when it was given a package name. A catalogue older than two weeks is
reported as such, because an empty list then says little.

`upgrade` installs a newer version of a package, or a newer packaging
revision of the same version, and keeps files you changed. Versions are
compared as numbers (1.10 is newer than 1.9) when they consist of numbers
and dots, optionally with one `-testN`, `-previewN` or `-rcN` tag and a
`-rN` rebuild tag (0.9.0-test2-r2, 0.9.0-test4 and 0.9.0 come in that
order); apkg refuses an older version and any version it cannot order, and
says why. `rollback` returns to what the last upgrade replaced.

`upgrade --all` first prints the plan: every update it will install and
every one it skips, with the reason. Then it upgrades the packages one after
the other, each exactly as `apkg upgrade <id>` would, and reports each.
Each package is its own operation: one that fails, for example because a
file you changed would be overwritten, is left as it was and the others
continue. The group is not undone as a whole. `apkg --dry-run upgrade
--all` prints each package's dry run and changes nothing. Its exit status
is 0 when every planned upgrade succeeded (or there was nothing to do) and
10 when any failed or was stopped; the dry run exits with 10 when any
package's dry run failed.

## What is new

`apkg update` compares the catalogue it fetched with the one this machine
had before, for this system (its CPU and ABI) only:

- **new application**: the catalogue had no entry with this id at all.
  Marked *back* when the catalogue first published it before your previous
  update: it was removed and has returned;
- **new build for this system**: the catalogue listed the application,
  but not for this system; now it has a build you can install;
- **updated**: the build for this system has another version or revision.
  This is not counted as a new application.

A new build for another CPU or ABI is not news here. Updating again with no
change in the catalogue reports nothing new. A failed or cancelled update
changes neither the catalogue nor the record of the last update. When this
machine had no catalogue before, or the catalogue now comes from another
address, nothing is called new: the summary lists the catalogue's five most
recent additions instead and says why.

```text
apkg recent
apkg recent --days 14
apkg recent --since 2026-10-01 --builds
apkg recent --since-last-update
apkg recent --json --limit 0
```

`recent` lists the applications for this system that the catalogue added
most recently, newest first, with the date the catalogue first published
them. That date belongs to the catalogue: it is not the day your machine
fetched it, and a new version, a new revision or a new review does not
change it. An application whose build for this system came later than the
application itself is a new build, listed by that build's date and only
with `--builds`, where it is labelled. Applications the catalogue gives no
date for are counted and not listed; a catalogue made before apkg 0.6 has
no dates at all.

- `--limit N`: at most N rows (default 10; 0 lists all).
- `--days N`: published in the last N days, today included, by this
  machine's date.
- `--since YYYY-MM-DD`: published on or after that date. Not together with
  `--days`.
- `--builds`: also new builds for this system of older applications.
- `--since-last-update`: what the last successful `apkg update` found, as
  above, in full. `--limit N` keeps N rows of each kind. If the catalogue in
  use is not the one that update fetched, it says so instead of listing.
- `--json`: one JSON object and nothing else: `schema` (1), `mode`
  (`recent` or `last-update`), `catalogue` (the address the last update
  used), `refreshed`, `compared_with`, `first_catalogue`, `first_reason`,
  `counts` (`new_applications`, `new_builds`, `updated`, `recently_added`,
  `unknown_dates`), and `items`, each with `id`, `kind`
  (`new-application`, `new-build`, `updated`, `recently-added`),
  `returned`, `version`, `revision`, `old_version`, `old_revision`, `arch`,
  `abi`, `first_added`, `app_first_added`, `installed` and `summary`.
  `--since-last-update` adds `recorded` and `current`.

Exit status of `recent`: 0 when it answered, also with nothing to list; 10
when there is no catalogue, or for `--since-last-update` when no update was
recorded or the catalogue was replaced since; 5 for a wrong combination of
options.

### Listed, Reviewed and runtime tests

`apkg show <id>` says what has been checked about the build it describes:

```text
  status:       Reviewed -- the catalogue's automatic checks passed for this archive (suite 1, 2026-10-14): metadata, download, archive, paths, layout, cpu. Not decidable from the files: abi
  runtime tests: AROS One 1.3 x86_64/v11, QEMU (KVM), 2026-09-29: passed (install, start, remove) -- window open after 15 s
  added:        2026-09-29
  catalogue:    https://raw.githubusercontent.com/tomaszstaniak/arospkg-index/main/index-v2.json
```

- **Listed**: the entry is published. Its download is still checked against
  its size and SHA-256 before anything is installed, as for every package.
- **Reviewed**: the catalogue's automatic checks passed for exactly this
  archive and this entry: the entry's metadata, the downloaded size and
  SHA-256, a readable archive, safe member names that cannot overwrite one
  another, the drawer, icon and program the entry names, and the CPU its
  programs were built for. The ABI cannot be read from the files and is
  never guessed. Nobody installs or starts the program for this.
- **Runtime tests** are a separate record: someone installed and started
  these very bytes on the system named, on that date. A new archive starts
  without any. None recorded means nobody recorded one, not that it fails.

Neither is a reason to refuse an installation, and apkg asks for no extra
confirmation for a package nobody has started.

### Ctrl-C, redirection and terminals

Ctrl-C stops an install or upgrade while that is still free: during the
download, or right after the archive has been verified. Once files are
being changed it is not acted on; the operation finishes, and apkg says
that the Ctrl-C came too late to stop it. `upgrade --all` then stops before
the next package, and exits with 10. Ctrl-C during `apkg update` stops the
download and keeps the catalogue you had. (On mainline AROS the TCP stack
itself breaks the transfer on Ctrl-C; apkg reports that as a cancel too.) A command that was stopped can
simply be run again.

Output redirected to a file (`apkg outdated >RAM:updates.txt`) is plain
text, as is output with `--plain`. In a terminal that tells apkg what it
can show, such as Limpet, apkg uses colour and wraps to the window; in the
standard Shell window it prints plain text. Commands apkg prints for you to
copy carry your `--root`, quoted when needed.

### Updating arospkg itself

`self-update --check` lists apkg and PkgManager as installed and the latest
stable release. `self-update` updates both to that release, keeping the
previous files as `.old` (or, for arospkg installed as a package, as its
previous revision). In PkgManager, **About** > **Check for a newer
version** and then **Update arospkg** do the same; PkgManager closes and
starts again. A copy such as `C:apkg` is updated with the installation you
tie it to; apkg asks once, or takes `--drawer` or `--apkg-only`. When the
latest release has no archive for your machine, `--check` says so and
nothing changes. [Updating arospkg](../self-update.md) has the details.

## Check the installed files

```text
apkg verify folio
```

compares every installed file with what was installed: as installed,
changed locally, missing, or in the drawer without being the package's.
It repairs nothing. PkgManager's **Verify** shows the same at the top of the
details.

## Remove

```text
apkg remove zunecalc
```

Files you changed, and files apkg did not install, are kept; apkg lists
them, and so does PkgManager after **Remove**.

## PkgManager

- The state selector filters the list: **All**, **Installed**, **Not
  installed**, **Updates (N)**, **Recently added**, **Last refresh** and,
  when the demonstration storefront is present, **Demo store**.
- **Recently added** lists the 25 newest applications and new builds for
  this machine, newest first, with the catalogue's date in the State
  column; the same list as `apkg recent --builds --limit 25`. **Last
  refresh** lists what the last catalogue update brought, as
  `apkg recent --since-last-update`. After **Update index** the line under
  the list says how many new applications, new builds and updates it found.
  A catalogue without dates, or no recorded update, is said as such, never
  shown as "nothing new".
- **Install** shows the plan in a confirmation window when it changes more
  than the selected package (what it needs, what it replaces). **Remove**
  refuses a package another one needs, and says which. The details list
  what a package needs, recommends, conflicts with and replaces, with the
  state of each.
- The details show the status (Listed or Reviewed), the runtime tests
  recorded for this archive, when the catalogue first published it and
  where the catalogue came from.
- The details start with what matters first and stay in view: the version
  installed, whether it runs on this machine and how it starts. Below them
  are the location, updates, notes from the package and the requirements.
- **Run**, **Open folder**, **Verify**, **Upgrade** (with the plan shown
  first, and your confirmation), **Roll back** and **Remove** act on the
  selected installed package. Downloads, checks and starts run on a worker
  process, so the window keeps answering; **Cancel** stops a download.
- Without a catalogue, or with one that cannot be read, PkgManager still
  lists what is installed, and Run, Open folder, Verify, Roll back and
  Remove still work.
- Some applications show pictures beside the details: their icon, a
  screenshot and small thumbnails, with a line saying where the pictures
  come from. In 0.5.0 these are a **demonstration**: a few real screenshots
  shipped with PkgManager in its `Demo` drawer, not pictures from the
  catalogue. An application without pictures shows text only.
- **Demo store** lists one fictional paid application with a price. **Buy...**
  shows how buying could work in a later version. It is marked
  "Demo -- purchasing is not available yet": nothing can be bought, nothing
  is installed, and no data is asked for or sent. See
  [RFC 0003](../rfc/0003-storefront-metadata.md).
- PkgManager's ARexx port (`PKGMANAGER`) has `RUN <id>` and `VERIFY <id>`
  besides the earlier commands; see [the ARexx page](../arexx.md).

## When a download does not match the catalogue

apkg downloads into `SYS:Packages/cache/<id>.<ext>.part`, checks its size
and SHA-256 against the catalogue, and only then renames it into the cache.
A download that fails either check is deleted at once, so after
`the download does not match the index` there is no file left to look at.

To keep it for diagnosis, repeat the install with `--keep-rejected`:

```text
apkg --keep-rejected install folio
```

The rejected file is then moved to `SYS:Packages/cache/<id>.<ext>.rejected`
(a later rejection replaces it) and apkg prints its path, its size, the
expected SHA-256, and three received ones: the body as it was in memory
before being written, the file as verification read it, and a second read
of the same file. The file is still refused: nothing is installed from it,
and the next install downloads again rather than using it.
