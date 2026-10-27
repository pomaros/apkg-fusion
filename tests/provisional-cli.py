#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Exercise the provisional adapter regression baseline with offline, disposable fixtures.

The ELF header establishes CPU selection, not an executable AROS program.
Bundle validation does not prove launch or runtime dependencies work.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


HERE = Path(__file__).resolve().parents[1]
APKG = Path(os.environ.get("APKG_PROVISIONAL", str(HERE / "build/apkg-provisional"))).resolve()
TARGET = "x86_64-aros-v11-native"
checks = 0
failures = []


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def elf():
    head = bytearray(64)
    head[:4] = b"\x7fELF"
    head[4:7] = bytes([2, 1, 1])
    head[18:20] = (62).to_bytes(2, "little")
    return bytes(head) + b"fixture only, not executable\n"


def descriptor():
    return {
        "schema": 1, "id": "demo", "version": "1.0", "revision": 1,
        "targets": [{"os": "aros", "arch": "x86_64", "abi": "v11"}],
        "requires_system": [],
        "launch": {"executable": "Demo", "mode": "shell"},
    }


def records(output):
    return dict(line.split(": ", 1) for line in output.splitlines() if ": " in line)


def scenario(name, test):
    with tempfile.TemporaryDirectory(prefix="fusion-cli-") as temp:
        temp = Path(temp)
        env = {k: v for k, v in os.environ.items() if not k.startswith("PKG_")}
        env["XDG_CONFIG_HOME"] = str(temp / "config")

        def run(*args, expect=0):
            command = [str(APKG), *map(str, args)]
            result = subprocess.run(command, cwd=temp, env=env, capture_output=True, text=True, timeout=30)
            output = result.stdout + result.stderr
            check(result.returncode == expect,
                  f"{args}: expected exit {expect}, got {result.returncode}\n{output}")
            return output

        try:
            test(temp, run)
            print(f"PASS {name}")
        except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
            failures.append(f"{name}: {error}")
            print(f"FAIL {name}: {error}")


def signed_fixture(temp, run, short="--literal description"):
    key = temp / "--author.key"
    run("keygen", "--file", "--author.key", "--machine")
    source = temp / "source with spaces"
    (source / "Demo").mkdir(parents=True)
    (source / "Demo/Demo").write_bytes(elf())
    (source / "Demo/ReadMe").write_text("signed fixture data\n")
    channel = temp / "local channel"
    out = run("publish", source, "--channel", channel, "--name", "demo", "--version", "1.0",
              "--target", TARGET, "--kind", "application", "--sign", "--author.key",
              "--short", short, "--machine")
    answer = records(out)
    check(answer.get("result") == "published", "publish reports its actual result")
    line = (channel / "index").read_text().strip().split()
    check(line[:3] == ["demo", "1.0", TARGET], "signed channel carries the complete target")
    digest = line[3]
    manifest = (channel / "objects" / (digest + ".manifest")).read_bytes()
    check(hashlib.sha256(manifest).hexdigest() == digest, "independent manifest address check")
    check(b"Short: " + short.encode() + b"\n" in manifest, "option value preserved in signed metadata")
    check((channel / "objects" / (digest + ".sig")).is_file(), "author signature published")
    check(answer.get("signer") == records(run("keyinfo", "--file", key, "--machine")).get("public"),
          "published signer is the chosen author key")
    return source, channel, key


def signed_round_trip(temp, run):
    source, channel, _ = signed_fixture(temp, run)
    root = temp / "root with spaces"
    dry = run("install", "demo", "--root", root, "--channel", channel,
              "--target", TARGET, "--dry-run", "--machine")
    check("result:" in dry and not (root / "Demo/Demo").exists(), "dry run reports a result without placing files")
    answer = run("install", "demo", "--root", root, "--channel", channel, "--target", TARGET, "--machine")
    check(records(answer).get("result") == "installed", "native install dispatches to the engine")
    check((root / "Demo/Demo").read_bytes() == elf(), "installed bytes match the signed source")
    check(records(run("verify", "demo", "--root", root, "--machine")).get("result") == "intact",
          "verify checks the installed package")
    check(records(run("verify", "--all", "--root", root, "--machine")).get("result") == "intact",
          "long switch followed by a value option is parsed correctly")
    (root / "Demo/ReadMe").write_text("local edit\n")
    out = run("verify", "demo", "--root", root, "--machine", expect=12)
    check("ReadMe" in out, "verification identifies changed content")
    check((source / "Demo/ReadMe").read_text() == "signed fixture data\n", "source files remain untouched")


def abi_isolation(temp, run):
    _, channel, _ = signed_fixture(temp, run)
    for target in ("x86_64-aros-v1-native", "x86_64-aros-v11-hosted", "aarch64-aros-v11-native"):
        root = temp / target
        out = run("install", "demo", "--root", root, "--channel", channel, "--target", target, "--machine", expect=11)
        check("result: refused" in out, "incompatible target is refused by selection")
        check(not (root / "Demo/Demo").exists(), "incompatible ABI/runtime/CPU never places package files")


def option_values(temp, run):
    # Values that resemble options and keyword names are literal values.
    out = run("keygen", "--file", "--machine", "--machine")
    check((temp / "--machine").is_file(), "an option-looking key path is used verbatim")
    check(records(out).get("file") == "--machine", "machine output identifies that exact file")
    out = run("keygen", "--file", "MACHINE", "--machine")
    check((temp / "MACHINE").is_file(), "uppercase keyword-looking value is used verbatim")
    check(records(out).get("file") == "MACHINE", "uppercase filename is not consumed as a switch")
    out = run("keygen", "--file", "ROOT=value", "--machine")
    check((temp / "ROOT=value").is_file() and records(out).get("file") == "ROOT=value",
          "equals signs inside option values remain literal")
    signed_fixture(temp, run, short="MACHINE")


def target_refusals(temp, run):
    for target in ("x86_64", "generic", "x86_64-aros-v99-native", "x86_64-aros-v11-raspi", "aarch64-linux-v1-hosted"):
        out = run("install", "demo", "--root", temp / "root", "--channel", temp / "channel",
                  "--target", target, expect=20)
        check("target" in out.lower(), "invalid target refusal is explicit")
    run("install", "demo", "--root", temp / "root", "--channel", temp / "channel", expect=20)
    run("install", "demo", "--target", expect=20)
    run("install", "demo", "--arch", "x86_64", expect=20)
    run("keygen", "--file", expect=20)
    run("keygen", "--file", "one", "--file", "two", expect=20)
    run("keygen", "--file", temp / "must-not-exist", "--bogus", "value", expect=20)
    check(not (temp / "must-not-exist").exists(), "unknown options are rejected before key creation")
    check(not (temp / "root").exists(), "invalid request never creates the installation root")


def target_case_bypass(temp, run):
    # Dispatch accepts AmigaDOS-style casing; it must not bypass target policy.
    run("keygen", "--file", temp / "key", "--machine")
    source = temp / "data"
    source.mkdir()
    (source / "file").write_text("data\n")
    for command in ("PUBLISH", "Publish", "publish"):
        channel = temp / (command + "-channel")
        run(command, source, "CHANNEL", channel, "NAME", "demo", "VERSION", "1.0",
            "ARCH", "generic", "KIND", "data", "SIGN", temp / "key", "MACHINE", expect=20)
        check(not channel.exists(), "command spelling cannot bypass explicit full target")
    for command in ("INSTALL", "Upgrade", "ROLLBACK", "Repair"):
        out = run(command, "demo", "--root", temp / "root", expect=20)
        check("target" in out.lower(), "every writing operation enforces its target across command casing")


def command_arity(temp, run):
    check("apkg-fusion development build" in run("VERSION"), "command casing retains fusion version identity")
    check("apkg-fusion development build" in run("-v"), "short version alias retains fusion identity")
    run("--version", "ignored", expect=20)
    run("VERSION", "ignored", expect=20)
    run("--help", "ignored", expect=20)
    run("SELF-UPDATE", expect=18)


def bundles(temp, run):
    drawer = temp / "unregistered drawer"
    drawer.mkdir()
    desc = drawer / "application.json"
    desc.write_text(json.dumps(descriptor()))
    before = desc.read_bytes()
    out = records(run("bundle-check", drawer))
    check(out.get("description") == "valid" and out.get("id") == "demo", "valid unregistered descriptor accepted")
    check(out.get("cpu") == "x86_64" and out.get("abi") == "v11", "descriptor target preserved")
    check(out.get("runtime") == "unchecked", "descriptor validation does not claim launch success")
    check(desc.read_bytes() == before and sorted(p.name for p in drawer.iterdir()) == ["application.json"],
          "bundle check neither requires nor writes a registry or executable")
    bad = descriptor()
    bad["launch"]["executable"] = "../escape"
    desc.write_text(json.dumps(bad))
    check("invalid application description" in run("bundle-check", drawer, expect=12), "unsafe executable path refused")
    desc.write_bytes(b" " * (64 * 1024 + 1))
    check("invalid application description" in run("bundle-check", drawer, expect=12), "oversized descriptor refused")
    desc.write_text('{"schema":1,"schema":1}')
    run("bundle-check", drawer, expect=12)
    run("bundle-check", temp / "absent", expect=17)
    run("bundle-check", expect=20)


def main():
    check(APKG.is_file(), "build/apkg-provisional is required; run make build/apkg-provisional")
    for name, test in (
        ("signed publish/install/verify", signed_round_trip),
        ("CPU ABI runtime isolation", abi_isolation),
        ("literal long-option values", option_values),
        ("invalid target and options", target_refusals),
        ("target casing bypass", target_case_bypass),
        ("command identity and arity", command_arity),
        ("unregistered application descriptions", bundles),
    ):
        scenario(name, test)
    print(f"provisional-cli: {checks} checks; {len(failures)} failed scenarios")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
