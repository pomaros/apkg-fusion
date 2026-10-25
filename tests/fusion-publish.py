#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Offline authoring -> signed channel -> native-engine install contract.

The ELF fixture proves CPU selection; it is not an executable runtime test.
The key, payload, channel and roots live in one private temporary directory.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parents[1]
ENGINE = Path(os.environ.get("PKG", str(HERE / "build/pkg"))).resolve()
BRIDGE = HERE / "tools/apkg-publish"
checks = 0


def check(value, message):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message)


def elf(cpu=62):
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4:6] = bytes([2, 1])
    header[18:20] = cpu.to_bytes(2, "little")
    return bytes(header) + b"fixture, not a runnable program\n"


def inventory(directory):
    return {str(p.relative_to(directory)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in directory.rglob("*") if p.is_file()}


def main():
    with tempfile.TemporaryDirectory(prefix="fusion-publish-test-") as temp:
        temp = Path(temp)
        env = {k: v for k, v in os.environ.items() if not k.startswith("PKG_")}
        env["XDG_CONFIG_HOME"] = str(temp / "config")

        def invoke(command, expect=0):
            result = subprocess.run([str(x) for x in command], env=env, text=True, capture_output=True)
            output = result.stdout + result.stderr
            check((result.returncode == 0) if expect == 0 else (result.returncode != 0),
                  f"exit {result.returncode} for {command}:\n{output}")
            return output

        key = temp / "author.key"
        invoke([ENGINE, "KEYGEN", "FILE", key, "MACHINE"])
        sources = temp / "authors"
        sources.mkdir()
        drawer = sources / "Demo"
        drawer.mkdir()
        (drawer / "Demo").write_bytes(elf())
        (drawer / "ReadMe").write_text("fixture data\n")
        author = sources / "Demo.arospkg.toml"
        original = '''# The author's original spacing and comments survive signing.
id = "demo"
version = "1.0"
revision = 1
summary = "Fusion fixture"
category = "game/demo"
license = "MIT"
kind = "app"
arch = "x86_64"
abi = "v11"
subdir = "Demo"
depends = []
depends_checked = true
requires_system = []

[launch]
executable = "Demo"
mode = "shell"
'''
        author.write_bytes(original.encode())
        before = inventory(sources)
        channel = temp / "channel"

        def bridge(*extra, expect=0, selected=channel):
            return invoke([sys.executable, BRIDGE, drawer, "--channel", selected,
                           "--sign", key, "--runtime", "native", "--engine", ENGINE, *extra], expect)

        output = bridge()
        check("publication-mode: open" in output and "review: not-queried" in output
              and "runtime-tests: not-queried" in output,
              "open publication without curator approval")
        check("target: x86_64-aros-v11-native" in output, "full target is explicit")
        check(before == inventory(sources), "author files untouched, no generated source files")
        name, version, target, digest = (channel / "index").read_text().strip().split()
        check((name, version, target) == ("demo", "1.0", "x86_64-aros-v11-native"), "index identity")
        signed = (channel / "objects" / (digest + ".manifest")).read_bytes()
        check(hashlib.sha256(signed).hexdigest() == digest, "manifest address verified independently")
        check(b"Demo/Package.arospkg.toml" in signed and b"Demo/application.json" in signed,
              "author metadata and bundle descriptor covered by signed inventory")
        check((channel / "objects" / (digest + ".sig")).is_file(), "author signature exists")
        check(b"Reviewed:" not in signed and b"approved" not in signed, "no invented review attestation")

        root = temp / "root"
        invoke([ENGINE, "INSTALL", "demo", "ROOT", root, "CHANNEL", channel,
                "ARCH", target, "MACHINE"])
        invoke([ENGINE, "VERIFY", "demo", "ROOT", root, "MACHINE"])
        check((root / "Demo/Demo").read_bytes() == elf(), "installed executable unchanged")
        check((root / "Demo/Package.arospkg.toml").read_bytes() == original.encode(), "author TOML preserved byte-for-byte")
        desc = json.loads((root / "Demo/application.json").read_bytes())
        check(desc["id"] == "demo" and desc["launch"]["executable"] == "Demo"
              and desc["targets"][0]["abi"] == "v11", "generated bundle descriptor retains launch and ABI")
        installed_author = root / "Demo/Package.arospkg.toml"
        installed_author.write_bytes(original.encode() + b"# unauthorized change\n")
        invoke([ENGINE, "VERIFY", "demo", "ROOT", root, "MACHINE"], expect=1)
        installed_author.write_bytes(original.encode())
        invoke([ENGINE, "VERIFY", "demo", "ROOT", root, "MACHINE"])
        wrong_root = temp / "wrong-abi"
        invoke([ENGINE, "INSTALL", "demo", "ROOT", wrong_root, "CHANNEL", channel,
                "ARCH", "x86_64-aros-v1-native", "MACHINE"], expect=1)
        check(not (wrong_root / "Demo/Demo").exists(), "ABI mismatch never installs package bytes")
        wrong_runtime = temp / "wrong-runtime"
        invoke([ENGINE, "INSTALL", "demo", "ROOT", wrong_runtime, "CHANNEL", channel,
                "ARCH", "x86_64-aros-v11-hosted", "MACHINE"], expect=1)
        check(not (wrong_runtime / "Demo/Demo").exists(), "runtime mismatch never installs package bytes")
        bridge()
        check(before == inventory(sources), "repeat publication leaves inputs unchanged")

        # The same author identity cannot silently replace published bytes.
        (drawer / "ReadMe").write_text("different payload\n")
        bridge(expect=1)
        check((channel / "index").read_text().strip().split()[-1] == digest, "immutable published version")
        (drawer / "ReadMe").write_text("fixture data\n")

        def refused_author(text, phrase):
            author.write_text(text)
            rejected = temp / "must-not-publish"
            answer = bridge(expect=1, selected=rejected)
            check(phrase in answer, f"refusal names {phrase}: {answer}")
            check(not rejected.exists(), "rejected metadata never creates a channel")
            author.write_bytes(original.encode())

        refused_author(original.replace('revision = 1', 'revision = 2'), "revision")
        refused_author(original.replace('version = "1.0"', 'version = "1.0-rc1"'), "version")
        refused_author('reviewed = true\n' + original, "reviewed")
        refused_author('status = "approved"\n' + original, "status")
        refused_author(original.replace('depends = []', 'depends = ["../escape"]'), "depends")
        refused_author(original.replace('requires_system = []', 'requires_system = [{type="library",id="SDL2.library"}]'), "requires_system")
        refused_author('conflicts = ["other"]\n' + original, "conflicts")
        refused_author('replaces = ["other"]\n' + original, "replaces")
        refused_author('recommends = [{id="other",reason="optional"}]\n' + original, "recommends")
        refused_author('runs_on = ["AROS One"]\n' + original, "runs_on")
        refused_author(original.replace('executable = "Demo"', 'executable = "../outside"'), "launch")
        refused_author(original.replace('arch = "x86_64"', 'arch = "i386"'), "arch")
        refused_author(original.replace('kind = "app"', 'kind = "library"'), "kind")

        (drawer / "leak").symlink_to(key)
        check("regular files" in bridge(expect=1), "source symlink rejected")
        (drawer / "leak").unlink()
        (drawer / ".config").write_text("needed application data")
        check("hidden member" in bridge(expect=1), "hidden app files cannot silently disappear")
        (drawer / ".config").unlink()
        (drawer / "empty").mkdir()
        check("empty directory" in bridge(expect=1), "empty app directories cannot silently disappear")
        (drawer / "empty").rmdir()
        (drawer / "Package.arospkg.toml").write_text("collision")
        check("reserved" in bridge(expect=1), "reserved author metadata path cannot overwrite application data")
        (drawer / "Package.arospkg.toml").unlink()
        check(before == inventory(sources), "all refusals leave original author data intact")

        # Dependencies arrive through the same signed engine, without review.
        helper = sources / "Helper"
        helper.mkdir()
        (helper / "data").write_text("dependency data\n")
        helper_author = sources / "Helper.arospkg.toml"
        helper_author.write_text(original.split("[launch]")[0].replace('"demo"', '"helper"')
                                 .replace('"Demo"', '"Helper"').replace('kind = "app"', 'kind = "data"'))
        invoke([sys.executable, BRIDGE, helper, "--channel", channel, "--sign", key,
                "--runtime", "native", "--engine", ENGINE])
        author.write_text(original.replace('version = "1.0"', 'version = "1.1"')
                          .replace('depends = []', 'depends = ["helper >= 1.0"]'))
        bridge()
        dependent_root = temp / "with-dependency"
        invoke([ENGINE, "INSTALL", "demo", "ROOT", dependent_root, "CHANNEL", channel,
                "ARCH", target, "MACHINE"])
        check((dependent_root / "Helper/data").read_text() == "dependency data\n", "declared dependency installed")
        invoke([ENGINE, "VERIFY", "ALL", "ROOT", dependent_root, "MACHINE"])
    print(f"fusion-publish: {checks} checks passed")


if __name__ == "__main__":
    main()
