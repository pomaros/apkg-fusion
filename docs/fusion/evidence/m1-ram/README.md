<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Isolated M1 RAM component evidence

These are application outputs from synthetic fixtures on a physical M1 under
a hypervisor. The runtime uses a retained RAM OS and an SDK with local C-runtime
changes. Platform, extended metadata, native network transport, interrupted
recovery, power-loss and live-distribution qualification have separate gates.

`prepared.json` records preparation inputs; its execution field describes that
preparation stage. `verdict.json` records the measured execution and source and
binary identities. Replay the command report validation from the repository:

```sh
python3 tests/aros/verify_rehearsal.py \
  --prepared docs/fusion/evidence/m1-ram/prepared.json \
  --report docs/fusion/evidence/m1-ram/cli.txt
```

`gui-verify.txt` records successful window-driven verification and the first
confirmed removal. `gui-final.txt` records the corrected operation name and
clean worker/client shutdown. `gui-final-state.json` is the independently
exported common database after the final removal. Independent transfer hashes,
target identity, debugger interruption and process-exit observations are retained
with the hardware run. The installed library and updater were unchanged.

Signing keys and package data are synthetic fixtures. These records confer no review or runtime qualification
on published third-party packages.
