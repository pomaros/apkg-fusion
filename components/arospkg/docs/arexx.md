# Controlling PkgManager from ARexx

While PkgManager is running, any ARexx script can use its catalogue and its
installer. A script does not have to read `apkg` output or install anything
itself; it sends a command to the port `PKGMANAGER` and gets an answer.

## What you can do with it

- **Check whether a program is installed**, and which version, before a
  script relies on it.
- **Open a program's details in PkgManager**, so the user sees what it is,
  what it needs from the machine and how big the download is, and can decide
  there.
- **Install or remove a program, and find out what really happened.** The
  command starts the job and answers with its number. The job then runs in
  PkgManager's own worker, as if the button had been pressed, and the script
  asks for the outcome.
- **Show progress and offer a cancel** while a download runs.
- **Ask for an upgrade or a rollback.** PkgManager shows its plan and the
  user confirms it in the window. A script cannot answer for the user.

Everything goes through the same code the window uses. The same rules apply
to a script: one operation at a time, and the same checks on the ABI, the
system requirements, the root lock, conflicts with changed files and
recovery of an interrupted operation. A script cannot skip any of them.

## The examples

`examples/arexx/` has five scripts that run as they are:

| script | what it does | needs someone at the window? |
|---|---|---|
| `IsInstalled.rexx <package>` | prints whether the package is installed and which version; returns 0 if installed, 5 if not, 10 on error | no |
| `ShowPackage.rexx <package>` | brings PkgManager to the front with the package selected | no |
| `Install.rexx <package>` | installs and waits for the real outcome; returns 0 only when installed | no |
| `InstallWatch.rexx <package> [seconds]` | installs, prints progress every second, and after `seconds` asks for a cancel if the job still allows it | no |
| `Upgrade.rexx <package>` | asks for an upgrade and waits while the user confirms or cancels it in the window | **yes** |

```
1> rx IsInstalled.rexx soliton
soliton is installed, version 2.2
1> rx Install.rexx cls
job 3: installing cls
cls is installed
```

When a script ends with a return code other than 0, `rx` adds a line such as
`Error executing script 5/0`. That is `rx` reporting the code, which a Shell
script can test with `If WARN`; nothing went wrong.

Each example has its own `ask` procedure of six lines, which you can copy
into your own scripts. [Results and errors](#results-and-errors) explains how
it works.

## The port

- **Name:** `PKGMANAGER`, in capitals: `ADDRESS PKGMANAGER` works as
  written, `ADDRESS 'pkgmanager'` does not. It exists only while PkgManager
  runs.
  PkgManager does not start itself, and nothing runs in the background when
  it is closed. `SHOW('P', 'PKGMANAGER')` tells a script whether it is there.
- **A second PkgManager** keeps its window but has no port. Its title says
  "PkgManager (no ARexx port)", so scripts always reach the first one. When
  the first quits, the name is free again for the next PkgManager started.
- **Without ARexx**, for example when `rexxsyslib.library` cannot be opened,
  PkgManager works as before with the same title note. The window and `apkg`
  never depend on ARexx.
- **Which interface:** `VERSION` answers
  `PkgManager 0.3 ARexx 1 sha256 <hash of the running binary>`. The fourth
  word is the interface version. It changes only when an existing command
  changes meaning; new commands can appear without changing it, and `HELP`
  lists the commands there are.

## Commands

Names are not case-sensitive and must be written in full. Arguments follow
AmigaDOS rules: `"double quotes"` hold spaces, `*"` is a quote inside them,
and keywords can be written `FIELD state` or `FIELD=state`.

| command | arguments | result on success | typical errors |
|---|---|---|---|
| `VERSION` | | `PkgManager 0.5.0 ARexx 1 sha256 <hex>` | |
| `HELP` | | every command name, space separated | |
| `LIST` | `[INSTALLED] [MATCH <text>]` | package ids, space separated; empty when none | `NOINDEX` |
| `INFO` | `<package> [FIELD <name>]` | one `name value` line per field, or only the value of `FIELD` | `NOTFOUND`, `BADARGS` (unknown field) |
| `REQUIREMENTS` | `<package>` | one line per requirement, probed now: `crt.library: satisfied (already open, version 5)`; empty when none | `NOTFOUND` |
| `SHOW` | `<package>` | empty; the window comes to the front with the package selected | `NOTFOUND` |
| `UPDATE` | | job number | `BUSY`, `CLOSING` |
| `INSTALL` | `<package>` | job number | `NOTFOUND`, `BUSY`, `CLOSING` |
| `REMOVE` | `<package>` | job number | `NOTFOUND`, `BUSY`, `CLOSING` |
| `UPGRADE` | `<package>` | job number; the user confirms the plan in the window | `NOTFOUND`, `BUSY`, `CLOSING` |
| `ROLLBACK` | `<package>` | job number; the user confirms the plan in the window | `NOTFOUND`, `BUSY`, `CLOSING` |
| `JOB` | `[<job>] [FIELD <name>]` | the job's fields (below); the latest job when no number is given | `NOJOB` |
| `WAIT` | `[<job>] [FIELD <name>]` | the same, but only once the job has ended | `NOJOB` |
| `CANCEL` | `[<job>]` | `requested`, or `declined` for a job waiting for confirmation | `NOTCANCELLABLE`, `FINISHED`, `NOJOB` |
| `QUIT` | | `closing`; the same as the close gadget | |
| `LASTERROR` | | why this script's last command failed: `CODE text`; empty after a success. Ask right after the failure | |
| `RUN` | `<package>` | job number; the job's `message` says what was started, as the Run button does. Starting needs the package's description (see [using arospkg](guide/using.md#start-a-program)); a Shell program gets a console window of its own | `NOTFOUND`, `BUSY`, `CLOSING`; the job fails with `CANNOTRUN`, `REQUIRES` or `CONFLICT` (another CPU or ABI) |
| `VERIFY` | `<package>` | job number; the job's `message` gives the counts (as installed, changed locally, missing, not the package's) | `NOTFOUND`, `BUSY`, `CLOSING` |
| `OUTDATED` | `[SHOW]` | the updates of installed packages, one `name value` line each: `catalogue` (`ok`, `missing`, `unreadable`), `updated`, `stale`, `installed`, `newer`, `ready`, `blocked`, and `packages` with the ids that have something newer. `SHOW` also brings the window forward on the Updates list | |

`INFO` fields: `id version revision state installedversion installedrevision
summary category arch abi kind size requires canupgrade canrollback
compatibility requirements`.
`compatibility` is `native`, `incompatible` or `undetermined`: whether the
package's declared CPU and ABI match this machine. It is a judgement from
the catalogue, not a test that the program runs. `requirements` is what
this machine's check of the package's requirements found just now:
`satisfied`, `missing`, `undetermined`, `none` (it states none), or `not
checked` (the package is not compatible). Both are the same verdicts `apkg
show` prints. `INFO` describes a package built for another target too;
`LIST` leaves such packages out.
`state` is `installed` or `available`. `requires` lists the machine
requirements as the catalogue names them. `canupgrade` and `canrollback` are
`1` or `0`, answered as the window's buttons are.

`OUTDATED` answers from the same reading of the catalogue and the installed
packages as `apkg outdated` and the window's Updates list; it downloads
nothing. `newer` counts updates found, `ready` those that can be started
now, `blocked` those this machine cannot take. `canupgrade` in `INFO` is the
window's Upgrade button: 1 only for an update that can be started now.

`LIST` reads the catalogue as PkgManager last fetched it. `LIST INSTALLED`
reads the record of installed packages, including any the catalogue no
longer has; with `MATCH` it searches the catalogue and keeps the installed
ones. `INFO` finds a package in the catalogue first and in the record of
installed packages after that. None of these commands changes anything.

## Results and errors

A command that succeeds sets `RESULT` (with `OPTIONS RESULTS`) and RC 0. A
command that fails sets RC 5, 10 or 20, and `LASTERROR` then says why, as a
code and a sentence:

| RC | code | meaning |
|---|---|---|
| 20 | `BADCOMMAND` | no such command |
| 20 | `BADARGS` | missing, extra or malformed argument, or an unknown field |
| 10 | `NOTFOUND` | the package is not in the catalogue (and, for `INFO`, `REMOVE`, `UPGRADE`, `ROLLBACK`, not installed either) |
| 10 | `NOINDEX` | no catalogue yet; `UPDATE` fetches it |
| 10 | `NOJOB` | no job with that number; the last 32 jobs are kept |
| 10 | `CLOSING` | the window is closing and takes no new work |
| 10 | `CANNOTSTART` | the worker process could not be started |
| 10 | `TOOLONG` | the answer would exceed 65535 bytes; narrow `LIST` with `MATCH` |
| 10 | `NOMEM` | there was no memory for the answer. A command that changes something (`UPDATE`, `INSTALL`, `REMOVE`, `UPGRADE`, `ROLLBACK`, `CANCEL`, `QUIT`) has its answer allocated before it acts, so for those `NOMEM` means nothing was done. A `WAIT` that fails this way leaves the job as it was. A job refused because its own bookkeeping cannot be allocated answers `CANNOTSTART` |
| 10 | `LOCKED` and the other status codes | the package root could not be read, as for a job below |
| 5 | `BUSY` | another operation is running or waiting for confirmation; the text names it |
| 5 | `NOTCANCELLABLE` | the job is past the point where it can be cancelled |
| 5 | `FINISHED` | the job has already ended |

**RC on current AROS distributions.** The Regina interpreter in AROS One
1.3, and in other ABIv11 systems built before the fix in AROS contrib
`0258b5de4` (2026-09-18), puts a successful command's result text into RC
and leaves RC empty after a failure. A script that tests `IF RC = 0` there
misreads every answer. What holds everywhere is that `RESULT` is set after
success and dropped after failure. The examples test that, and ask
`LASTERROR` for the reason:

```rexx
ask: PROCEDURE EXPOSE answer error
  DROP result
  ADDRESS PKGMANAGER arg(1)
  IF symbol('RESULT') = 'VAR' THEN DO; answer = result; error = ''; RETURN 1; END
  ADDRESS PKGMANAGER 'LASTERROR'
  answer = ''; error = result
  RETURN 0
```

`LASTERROR` belongs to the script that asks. Two scripts running at once
each get their own: PkgManager tells them apart by the port their commands
come back to and the task that owns it. That holds for as long as the error
is kept. A success from the same script clears it, and once 32 other
scripts have failed after it, it is dropped to make room. So ask right after
the failure. The same check has one gap: a new script asking `LASTERROR`
before it has failed at all could read a finished script's leftover, if its
port and task happen to land at the same addresses.

PkgManager never sets a variable in the calling script. A version that set `RC2` that way crashed RexxMast after about thirty calls
made from inside a `PROCEDURE`.

## Jobs

`UPDATE`, `INSTALL`, `REMOVE`, `UPGRADE` and `ROLLBACK` start a job and
answer at once with its number. **Accepting a job does not mean it
succeeded.** A job started by pressing a button in the window gets a
number too, so a script can follow it.

`JOB` and `WAIT` answer with these fields:
`id op package origin state phase done total cancancel error message`.

| state | meaning |
|---|---|
| `preview` | an upgrade or rollback is working out its plan |
| `confirm` | the plan is shown in the window, waiting for the user's decision |
| `running` | the operation runs; `phase` is `download`, `verify`, `extract` or `publish`, with `done`/`total` in bytes where known |
| `done` | it succeeded |
| `failed` | it did not; `error` is the code and `message` is libpkg's explanation |
| `cancelled` | a cancel was honoured and nothing was installed |
| `declined` | the plan was not confirmed; nothing changed |

`origin` is `arexx` or `window`. `error` for a failed job is one of `LOCKED
STALELOCK UNRESOLVED NOTFOUND EXISTS VERIFY CONFLICT ARCHIVE IO NOMEM
NETWORK REQUIRES`. For an upgrade or rollback refused by its plan it is
`NOTALLOWED` or `CONFLICT`.

- **One job at a time**, whoever starts it. While one runs or waits for
  confirmation, the window's buttons are disabled and new jobs from ARexx get
  `BUSY`. A job counts as running until the window has recorded its outcome,
  so the next one can never be handed the previous one's result. An `apkg` using the same root at the same moment is refused by the
  root lock, in either direction. A job that meets the lock fails with
  `LOCKED`.
- **Cancelling is libpkg's decision.** `CANCEL` is accepted only while the
  job's last report said it could still stop (`cancancel 1`): during the
  download and after verification, before anything is installed. Otherwise
  it answers `NOTCANCELLABLE`. `requested` means the request was passed on.
  The final state, `cancelled` or `done`, comes from the job itself: ask
  `WAIT`. A job waiting for confirmation is declined by `CANCEL`, as by the
  window's Cancel.
- **Confirmation stays in the window.** An upgrade or rollback from ARexx
  opens the same plan the button does, and waits for Proceed or Cancel there.
  There is no way to confirm from ARexx.
- **Polling.** A script may ask `JOB` in a loop without any pause: the job
  still finishes and the window keeps answering clicks. PkgManager takes a
  few commands at a time and lets its own input and the worker's messages
  in between, and the worker runs at the window's priority. A pause of a
  second, as `InstallWatch.rexx` has, is still kinder to the machine.
- **The script may end** while its job runs. The job continues, and any later
  script can ask for it by number.
- **Closing the window** by the close gadget or `QUIT` is safe during a
  job. A cancellable job is cancelled, any other is finished first. The
  window then quits after the job has ended, and a `WAIT` in progress gets the
  final state before the port closes.

## Limits

- Tested on AROS x86_64 ABIv11 (AROS One 1.3), from a pool machine. Nothing
  has been run on mainline (ABIv1) or aarch64.
- PkgManager has to be running already. A script that needs it can start it
  with `Run >NIL: <drawer>/PkgManager`. Nothing starts it on demand.
- There is no unattended upgrade or rollback, and no way to skip a
  confirmation.
- Messages in `message` and after `LASTERROR`'s code are for people; the
  codes are what a script should compare.
- An answer is at most 65535 bytes, the limit of an ARexx string.
