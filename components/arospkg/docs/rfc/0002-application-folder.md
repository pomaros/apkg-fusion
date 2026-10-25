---
title: "RFC 0002: application folder format"
status: implemented (schema 1)
schema: 1
created: 2026-09-27
updated: 2026-10-12
---

# RFC 0002: application folder format

| | |
|---|---|
| **Status** | **Implemented, schema 1**, in arospkg 0.5.0: `apkg run`, PkgManager's Run button, ARexx `RUN`, `apkg-pack` and `tools/mkindex.py`. |
| **Companion** | [RFC 0001](0001-package-format.md), the package format; [RFC 0003](0003-storefront-metadata.md), proposed storefront fields |
| **What it replaces** | The schema 0 draft of 2026-09-27, which proposed `application.toml`. Section 10 records what changed and why. |

## 1. Goal

Make an installed application recognisable and startable as one thing,
the way macOS bundles and RISC OS application directories are, while it
stays an ordinary drawer that every AmigaOS and AROS file manager can open,
browse and start programs from, with no arospkg involved.

The package format answers *how does this get onto the disk*. This format
answers *how is this drawer started*. It travels with the drawer, so it
also works for an application that was unpacked by hand:
`apkg run "Work:My Apps/Folio"` reads the drawer and nothing else.

## 2. One source of truth, and copies with one job each

The author writes the start information **once**, as the `[launch]` table
of the package manifest. Everything else is generated from it and checked
against it:

| where | written by | read by | job |
|---|---|---|---|
| `[launch]` in the package manifest (`.arospkg/manifest.toml`, or the catalogue manifest for an archive we do not build) | the author | `apkg-pack`, `tools/mkindex.py`, the catalogue's CI | **the source** |
| `<drawer>/application.json` in the archive | `apkg-pack build` | the runtime | lets a drawer unpacked by hand start |
| `"launch"` in `index-v2.json` | `tools/mkindex.py` | `apkg show`, PkgManager before installing; the installer | says Run will be offered; lets apkg write the file for an archive that has none |
| `<drawer>/application.json` written at installation | apkg, from the catalogue entry, **only when the archive has none** | the runtime | the same file for third-party archives that are not rebuilt |
| `"launch"` in the registry entry (`db/installed/<id>.json`) | apkg at installation | apkg's rollback | a rollback re-extracts an archive that may not carry the file, and writes it again |

The **runtime reads only the drawer's `application.json`**. It needs no
catalogue and no registry, so losing arospkg's records does not stop an
application from starting.

When an archive carries its own `application.json`, it stands. The
catalogue's checks (`tools/mkindex.py`, run by the catalogue's CI on every
pull request) refuse an entry whose archive's file says something
different from the manifest.

Stack size and tool types are **not** in any of these. They stay in the
program's icon, which is authoritative for a Workbench start (section 6).

## 3. The folder

```
Folio/                       an ordinary drawer; any name
├── application.json         this description
├── Folio                    the program
├── Folio.info               its icon: tool types and stack stay here
└── …
Folio.info                   the drawer icon, beside the drawer as usual
```

- `application.json` sits directly inside the drawer: UTF-8 JSON, at most
  64 KiB, written by a tool, not by hand.
- The drawer can have any name and can be moved. Paths in the description
  are relative to the drawer.
- A file manager that knows nothing of this format still shows a drawer,
  and the program inside still starts from its icon. Wanderer does not
  show `application.json`, which has no icon.

## 4. The manifest table

<!-- validate: none -->
```toml
[launch]
executable = "Folio"          # relative to the drawer (subdir)
mode       = "workbench"      # or "shell"
# working_directory = "."     # shell mode only; default: the program's drawer
```

| key | required | meaning |
|---|---|---|
| `executable` | yes | The program, relative to the drawer. A regular file; not an icon, not the drawer. |
| `mode` | yes | `"workbench"`: started as a double-click on its icon starts it. `"shell"`: started as a Shell command. |
| `working_directory` | no | The current directory it starts in, relative to the drawer. Default: the drawer holding the program. In workbench mode only that default is possible, because Workbench always starts a program in its own drawer. |

No other key is accepted. The client refuses a table with a key it does
not know instead of ignoring it: a later key could change what "start it"
means (an argument, another program), and starting it anyway would be
guessing.

## 5. `application.json`

What `apkg-pack` writes for Folio 0.4.1 (shortened):

```json
{
  "schema": 1,
  "id": "folio",
  "version": "0.4.1",
  "revision": 1,
  "targets": [ { "os": "aros", "arch": "x86_64", "abi": "v11" } ],
  "requires_system": [ { "type": "library", "id": "muimaster.library" } ],
  "launch": { "executable": "Folio", "mode": "workbench", "working_directory": "." }
}
```

| field | meaning |
|---|---|
| `schema` | `1`. Any other value makes the description unusable for this client. |
| `id`, `version`, `revision` | As in the package manifest. `name` may be added; without it the id is shown. |
| `targets` | Exactly one: the CPU and ABI the program is built for. Per-target executables are deferred (section 9). |
| `requires_system` | As in the package manifest: what the machine must provide to start it. |
| `launch` | Section 4, with `working_directory` always written out. |

## 6. Starting

1. **Find the drawer.** By id: the drawer the registry entry records. By
   path (anything containing `:` or `/`): that drawer, without a registry.
2. **Read and validate.** The schema, the fields, and every path: no
   absolute path, no `:`, `\`, `..` component, empty or `.` component, no
   control or non-ASCII character. Then on the disk: the program is a
   file and, through its lock's real name, inside the drawer (a link that
   leads elsewhere is refused); the working directory is a drawer inside it.
3. **Check the target.** The description's CPU and ABI against this
   machine; then the program file's own header (ELF machine, or an Amiga
   68k hunk file), because a description can be wrong. A Shell script is
   accepted in shell mode when it has the `s` protection bit.
4. **Check the machine.** Every `requires_system` library is opened and
   its version read. A missing one refuses the start; an undetermined one
   is reported as undetermined, never as satisfied.
5. **Start it as declared**, never in the other mode:
   - **workbench**: the program needs its icon, `<executable>.info`, of
     type tool. `workbench.library`'s `OpenWorkbenchObjectA` is given the
     program's full path, and documents to open as Workbench arguments
     (`WBOPENA_ArgLock`/`WBOPENA_ArgName`). The library takes the stack
     from the icon (never less than the desktop's default), honours
     `TOOLPRI`, passes the tool types in the WBStartup message and makes
     the program's drawer its current directory, exactly as for a
     double-click. Without an icon it would start the program as a Shell
     command with the default stack, which crashes programs such as Folio
     and GrafX2 that need 8 MiB; that is why the icon is required.
   - **shell**: `SystemTags` runs `"<program>" <arguments>` in the working
     directory, with the icon's stack when there is a tool icon, otherwise
     the Shell's. Arguments are quoted for ReadArgs (`*` escapes `"` and
     `*`); control characters are refused. `apkg run` uses its own console
     and waits, and its return code is the program's; PkgManager gives the
     program a console window of its own and does not wait.
6. **Say only what was seen.** `workbench.library` loads the program on
   its own process and reports a failed load to nobody (its handler has a
   `FIXME: report error somehow?`), while the call has already returned
   success. So a workbench start is reported as started only when a new
   process of the program's name appears within 20 seconds; otherwise the
   user is told it was not seen running. A shell start reports the
   program's return code (apkg) or that it was started in its own window
   (PkgManager).
7. **Nothing else.** Starting installs nothing, changes no settings and
   writes nothing in the drawer.

Errors name the cause in the user's words: the description is missing
(Open folder still works), the program or its icon is missing, a path
leads out of the drawer, the program is for another CPU or ABI, a library
is missing.

## 7. What the tools check

- **`apkg-pack check` / `build`** (the author's machine): the table
  (section 4); that the program exists in the drawer and is an ELF
  program for the manifest's CPU (or, in shell mode, a script, with a
  note); that a workbench start has a tool icon beside the program,
  reading classic DiskObject icons and AROS PNG icons, and saying which
  stack it sets. `build` writes `application.json` into the archive. A
  drawer that already has one must agree with the manifest.
- **`tools/mkindex.py`** (the catalogue): the same table rules; that the
  archive contains the program and, for workbench mode, its icon; that an
  `application.json` in the archive agrees with the manifest. The table
  goes into `index-v2.json` only; `index.json`, for clients up to 0.3.2,
  never carries it.
- **The client**: everything in section 6, at every start.

## 8. Packages without a description

A package that says nothing about starting installs and works as before:
`apkg run` and the Run button say that it does not say how to start it,
and Open folder shows its drawer. Nothing guesses a program from the
package's name or from the first executable in the drawer.

## 9. Deferred

Not defined by schema 1:

- **Document types** and *Open with…*.
- **Actions**: named entry points beyond the one program.
- **Optional features** with their own requirements.
- **Per-target executables**: one drawer with builds for several targets.
- **Settings and data paths**, for backup and reset.
- **Presentation** (pictures, offers): proposed separately in
  [RFC 0003](0003-storefront-metadata.md), not here.

## 10. Changes from the schema 0 draft

- **The file is JSON, `application.json`, and generated** (A1, A5). The
  draft proposed `application.toml` written by hand. libpkg already reads
  JSON; a TOML reader on AROS would have been a second parser to maintain
  for a file nobody needs to edit, because the author's single place is
  the manifest's `[launch]`.
- **One source** (A1): the manifest; the drawer's file and the catalogue
  entry are generated from it and compared by the tools.
- **Workbench start** (A2): through `OpenWorkbenchObjectA`, which is what
  Wanderer does; the icon is required and authoritative. A description and
  an icon cannot disagree about the stack, because the description has no
  stack.
- **Limits** (A3): 64 KiB, unknown keys in `launch` refused, paths checked
  again at every start; one target per drawer.
- **Moving and renaming** (A4): unchanged. A drawer moved by hand starts by
  path; the registry still records the old place, so `apkg run <id>` says
  the drawer is not there.
