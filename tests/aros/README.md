<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Isolated AROS rehearsal

`prepare_rehearsal.py` prepares signed synthetic fixtures and an AmigaDOS
script without contacting a target. Build the host test engine with
`make build/apkg-core build/libapkg-test.dylib` first. Supply explicit native
CLI, GUI and helper binaries:

```sh
python3 tests/aros/prepare_rehearsal.py --output /tmp/apkg-trial \
  --cli /path/to/apkg-core --gui /path/to/PkgManager \
  --launch /path/to/Launch --norequesters /path/to/NoRequesters
```

The helpers follow the target's existing launch/requester protocol: Launch
starts its single command argument asynchronously; NoRequesters suppresses
requesters on the calling Shell. They are explicit external inputs, not part
of the common engine. No production key enters the payload.

For a separate attempt in an existing RAM runtime, use a fresh output directory,
`--namespace APKG2 --runtime-dir RAM:APKG2`, and stage the contents of
`payload/APKGTrial` at that explicit target directory. The namespace gives the
attempt separate roots, channel and report (`RAM:APKG2TrialReport.txt`). Retain
each attempt's preparation and exported evidence.

Stage `payload/APKGTrial` as `SYS:APKGTrial` in an isolated RAM runtime using
the target owner's established supervisor and recovery procedure. Check the
payload hashes in `prepared.json` after staging, retain the exact executable
identities and verify that all original system files remain unchanged. Qualify
independent debugger interruption before running the application workload.
Do not replace an installed package manager or invoke its updater.

For keyboard-driven control, verify letters, digits and path punctuation against
the active Shell keymap before sending commands. Open a fresh Shell after
changing the default keymap; an existing console can retain its former layout.
Set the Shell's command stack
to 8 MiB (`Stack 8388608`). Execute `SYS:APKGTrial/Trial` from that Shell. It uses separate `RAM:APKG…`
namespaces and must run once in a fresh runtime. Its 25 steps cover fresh root
initialization, signed source installation, archive tampering refusal, upgrade,
installed-file verification, removal and preservation of edited configuration.
An unused host root seed supplies source configuration; target contexts open
freshly and approve target-held plans. No host approval is reused.

Export `RAM:APKGTrialReport.txt` through the target's established transfer
service, retaining its independent transfer receipt. Validate it on the host:

```sh
python3 tests/aros/verify_rehearsal.py --prepared /tmp/apkg-trial/prepared.json \
  --report /path/to/exported/APKGTrialReport.txt
```

The verifier rejects stale report identities, incomplete or reordered steps,
incorrect return codes, accepted tampering, wrong installed versions/signatures,
missing files and lost configuration. A final PASS marker alone is insufficient.
Its rejection tests run with the common host suite.

`StartGUI` launches PkgManager on the separate prepared GUI root and records
`RAM:APKGManager.log`. Launch success alone does not qualify the GUI: retain
independent interaction, operation and shutdown evidence. These basic fixtures
omit Amiga metadata; metadata, system placement, installed migration, power-loss
durability and bare-metal acceptance require their own qualification.

`fs_probe.c` is a separate native observation program. Run it with a fresh
explicit `RAM:APKGProbe…` drawer; it refuses existing drawers and other volumes.
Its report records POSIX modes, DOS object keys/enumeration cursors/protection
words, creation, write/read flushes and permission changes. A zero exit means
observations completed: inspect every recorded result and retain the report.
It is test tooling, not the proposed product diagnostics command.
