# Self-update tests

`apkg self-update` and PkgManager's **Update arospkg** (`src/libpkg/selfup*.c`)
tested on an ABIv11 pool machine against a controlled release source. No
release is published for them.

- The decisions (which installation, what is replaced, how an interruption
  is settled) are host tests: `tests/test_selfup.c`, run by
  `tests/run-host-tests.sh`.
- `dirren.c`: what AROS lets a running program do to its own drawer (the
  measurement the in-process swap relies on).

## The release source

`make-test-release.sh VERSION OUTDIR [BASE]` builds a labelled test release
(`0.7.0-sutest2`; the version must contain `-sutest`) laid out as
`tools/make-rc.sh` lays out a release. `BASE` is compiled in as the default
release source, so a PkgManager started from its icon can be tested too.

`relserver.py CERTDIR RELDIR` serves those releases as `https://10.0.2.2/`
on port 443, to `127.0.0.1` peers only (QEMU's user network), with a
certificate from `make-ca.sh`. `--fail NAME:HOW` breaks chosen downloads
(an HTTP status, `cut:<bytes>`, `slow:<ms>`). The guest trusts the test CA
only in the RAM copy of its bundle; `setup.script` appends it there.

## A run

```
sh tests/selfupdate/make-ca.sh CERTS
sh tests/selfupdate/make-test-release.sh 0.6.99-sutest1 B/b1 https://10.0.2.2/r2/
sh tests/selfupdate/make-test-release.sh 0.7.0-sutest2  B/r2 https://10.0.2.2/r3/
sh tests/selfupdate/make-test-release.sh 0.7.1-sutest3  B/r3 https://10.0.2.2/r3/
python3 tests/selfupdate/make-stage.py RUNID arospkg-0.6.0.x86_64-aros-v11.zip \
    B/b1 B/r2 B/r3 CERTS REL STAGE
python3 tests/selfupdate/relserver.py CERTS REL \
    --fail cut/arospkg-0.7.0-sutest2.x86_64-aros-v11.zip:cut:300000 \
    --fail slow/arospkg-0.7.0-sutest2.x86_64-aros-v11.zip:slow:700 \
    --fail slow3/arospkg-0.7.1-sutest3.x86_64-aros-v11.zip:slow:700 \
    --fail slowidx/index-v3.json:slow:400 &
python3 tests/tools/recv.py RECV 8765 &
```

The first archive is the published 0.6.0 for x86_64 ABIv11. Reserve a
machine (`~/Work/AROS/documentation/vm-pool.md`), boot it with STAGE
(`boot_staged` in `tests/arexx/pool-lib.sh`), then in its Shell:

```
Execute RAM:rxt/setup.script
Execute RAM:rxt/shell.script
Execute RAM:rxt/managed.script
```

and from the host `run-gui.sh RUNID SHOTS RECV` for the window, then
`Execute RAM:rxt/cleanup.script`. Every result goes into the run's own
drawer, `RESULTS:arospkg-RUNID/`. `grade.py RECV RUNID` grades the Shell
and package results; the window checks print PASS/FAIL into
`RECV/RUNID-g1..g4.txt`.

| Script | What it shows |
|---|---|
| `shell.script` | 0.6.0 as published, then the new apkg with `C:apkg` and `--drawer`; 0.6.0's own apkg run twice; apkg on its own; refusals (bad checksum, broken download, no archive for this machine, protected backup, write and rename failures, an operation in the root, a record that cannot be written); an update stopped between apkg and PkgManager, finished or undone; two updates at once, the second refusing to write over the first one's journal |
| `managed.script` | arospkg installed as a package in a root with spaces: 0.6.0's own self-update, the package upgrade, a user's icon kept, `verify`, rollback, a copy tied with `--drawer`, a stop inside the package transaction; the package changed by another revision and by a newer version while a self-update waits; a copy with two possible installations; a Shell update refused while PkgManager runs from the installation |
| `run-gui.sh` + `gui.script.in` | the window: available, the `C:apkg` question, progress, confirmation, restart and result; a second window refused; Cancel during the download; a package started from its icon; Repair now after an interrupted update; a current PkgManager whose apkg and C:apkg are behind |
