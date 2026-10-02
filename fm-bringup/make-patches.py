#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate and git-am-test the three-project FM integration series.

Inputs are pristine unpacked snapshots at the revisions in source-revisions.json.
No checkout, reset, commit or push is performed in any input checkout. The first
five integration patches are preserved; three follow-up patches add RDS/AF and
FM-only recording. All scratch history is on the session branch name.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
BRANCH = "arena/01a0e571-samsung-a6lte-dump"
ENV = {**os.environ, "GIT_AUTHOR_NAME": "A6 FM bring-up",
       "GIT_AUTHOR_EMAIL": "a6lte-fm@localhost",
       "GIT_COMMITTER_NAME": "A6 FM bring-up",
       "GIT_COMMITTER_EMAIL": "a6lte-fm@localhost",
       "GIT_AUTHOR_DATE": "2026-09-28T12:00:00+02:00",
       "GIT_COMMITTER_DATE": "2026-09-28T12:00:00+02:00"}
DEVICE_PATCHES = [
    "0001-a6lte-select-the-shipped-Broadcom-firmware.patch",
    "0002-a6lte-document-FM-transport-and-add-stock-trace-decoder.patch",
    "0003-a6lte-integrate-experimental-Broadcom-HCI-FM.patch",
    "0004-a6lte-add-RDS-AF-and-FM-capture.patch",
]


def git(tree, *args):
    try:
        return subprocess.check_output(
            ["git", "-C", str(tree), "-c", "commit.gpgsign=false",
             "-c", "core.hooksPath=/dev/null", *args], env=ENV, text=True).strip()
    except subprocess.CalledProcessError as error:
        print(error.output)
        raise


def initialize(base, dest):
    shutil.copytree(base, dest, symlinks=True,
                    ignore=shutil.ignore_patterns(".git", "__pycache__"))
    git(dest, "init", "-q", "--initial-branch=" + BRANCH)
    git(dest, "add", ".")
    git(dest, "commit", "-qm", "Test fixture: pristine source snapshot")


def overlay(source, target):
    for path in source.rglob("*"):
        if path.is_file() and "__pycache__" not in path.parts:
            dest = target / path.relative_to(source)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, dest)


def export_patch(tree, output, message, number=1):
    git(tree, "add", ".")
    git(tree, "diff", "--cached", "--check")
    git(tree, "commit", "-qm", message)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(git(tree, "format-patch", "--no-signature", "--stdout",
                          "--start-number", str(number), "-1") + "\n")


def verify(base, expected, destination, patches):
    initialize(base, destination)
    git(destination, "am", "--whitespace=error", *map(str, patches))
    assert git(destination, "status", "--porcelain") == ""
    assert git(expected, "rev-parse", "HEAD^{tree}") == git(destination, "rev-parse", "HEAD^{tree}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_tree", type=Path, help="pristine A6 device snapshot")
    parser.add_argument("--bt-tree", type=Path, required=True)
    parser.add_argument("--fm-tree", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, help="also run capture helper tests with this JDK")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    base = args.base_tree.resolve()
    if (base / "bluetooth/bt_vendor.conf").read_text() != (
            "# Firmware patch file name\nFwPatchFileName = bcm43455.hcd\n"):
        parser.error("unexpected base Bluetooth config; use the documented pristine snapshot")
    if (HERE / "device-tree/fm/BcmFmProtocol.h").read_bytes() != (
            HERE / "bluetooth-stack/btif/include/BcmFmProtocol.h").read_bytes():
        parser.error("protocol copies differ")
    patches = HERE / "patches"
    projects = [
        ("device", base, HERE / "device-tree", [patches / n for n in DEVICE_PATCHES],
         "a6lte: add RDS/AF decoding and FM capture policy\n\n"
         "Implement bounded FIFO polling, confirmed PS/2A/2B text and PI identity,\n"
         "G0-to-UTF-8 conversion, Method-A/B AF lists and stronger-same-PI probes.\n"
         "Restore tune/mute after probes and expose the real FMRadio JNI contract.\n"
         "Attach the existing SEC FM capture port in an A6-specific audio policy.\n"
         "Include decoder/failure-injection/AF/socket tests and hardware caveats.\n\n"
         "Requires the v2 Bluetooth broker and FMRadio follow-up patches.\n"
         "Firmware, Android policy/build and physical audio remain unverified."),
        ("bt", args.bt_tree.resolve(), HERE / "bluetooth-stack", [
            patches / "system_bt/0001-bt-add-device-gated-Broadcom-FM-endpoint.patch",
            patches / "system_bt/0002-bt-allow-bounded-FM-RDS-FIFO-access.patch"],
         "bt: allow bounded FM RDS FIFO access\n\n"
         "Version the authenticated socket to v2. Permit FM+RDS power, FIFO flush,\n"
         "a fixed waterline and only 240-byte FIFO read requests. Validate FIFO\n"
         "replies as bounded whole triples while keeping scalar lengths exact.\n"
         "Do not widen access to arbitrary VSCs, memory or pin configuration."),
        ("app", args.fm_tree.resolve(), HERE / "fm-app", [
            patches / "FMRadio/0001-FMRadio-add-Broadcom-HCI-and-SEC-audio-support.patch",
            patches / "FMRadio/0002-FMRadio-wire-RDS-AF-and-FM-only-recording.patch"],
         "FMRadio: wire RDS/AF and FM-only recording\n\n"
         "Serialize BCM metadata polling with tuner operations; update station\n"
         "database, UI and notifications using decoded UTF-8 PS/RadioText. Do not\n"
         "probe AF while recording or override queued user station selections.\n"
         "Enable recording through an explicit, verified FM-tuner AudioRecord,\n"
         "feeding the existing AAC/file workflow without duplicating playback.\n"
         "Reject microphone fallback, route changes and read failures; stop on\n"
         "focus, Bluetooth, headset and service teardown. Add host capture tests.\n\n"
         "HAL routing and encoded recordings require A6 hardware validation."),
    ]
    with tempfile.TemporaryDirectory(prefix="a6lte-fm-") as tmp:
        tmp = Path(tmp)
        applied = {}
        for name, source, files, series, message in projects:
            tree = tmp / name
            preserved = [p.read_bytes() for p in series[:-1]]
            initialize(source, tree)
            git(tree, "am", "--whitespace=error", *map(str, series[:-1]))
            overlay(files, tree)
            export_patch(tree, series[-1], message, len(series))
            assert preserved == [p.read_bytes() for p in series[:-1]]
            (series[-1].parent / "series").write_text("".join(p.name + "\n" for p in series))
            applied[name] = tmp / ("verify-" + name)
            verify(source, tree, applied[name], series)
        subprocess.run(["python3", "-m", "unittest", "discover", "-s", "tools/fm/tests", "-v"],
                       cwd=applied["device"], check=True)
        command = ["python3", str(applied["device"] / "fm/tests/run_host_tests.py"),
                   "--bt-tree", str(applied["bt"])]
        if args.sanitize:
            command.append("--sanitize")
        subprocess.run(command, check=True)
        if args.java_home:
            subprocess.run(["python3", str(applied["app"] / "tests/run_capture_tests.py"),
                            "--java-home", str(args.java_home.resolve())], check=True)
        print("PASS: all eight patches git-am applied; three trees match; selected host tests passed.")


if __name__ == "__main__":
    main()
