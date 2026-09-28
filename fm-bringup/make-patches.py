#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate and git-am-test the three-project FM integration series.

Inputs are pristine unpacked snapshots at the revisions in source-revisions.json.
No checkout, reset, commit or push is performed in any input checkout. The first
two device patches from the investigation are preserved; a third updates them
with the implementation. All scratch history is on the session branch name.
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
]


def git(tree, *args):
    return subprocess.check_output(
        ["git", "-C", str(tree), "-c", "commit.gpgsign=false",
         "-c", "core.hooksPath=/dev/null", *args], env=ENV, text=True).strip()


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
    parser.add_argument("--bt-tree", type=Path, required=True, help="pristine LineageOS system/bt snapshot")
    parser.add_argument("--fm-tree", type=Path, required=True, help="pristine LineageOS FMRadio snapshot")
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
    device_patches = [patches / name for name in DEVICE_PATCHES]
    bt_patch = patches / "system_bt/0001-bt-add-device-gated-Broadcom-FM-endpoint.patch"
    fm_patch = patches / "FMRadio/0001-FMRadio-add-Broadcom-HCI-and-SEC-audio-support.patch"
    with tempfile.TemporaryDirectory(prefix="a6lte-fm-") as tmp:
        tmp = Path(tmp)
        device, bt, app = tmp / "device", tmp / "bt", tmp / "app"
        initialize(base, device)
        git(device, "am", "--whitespace=error", *map(str, device_patches[:2]))
        overlay(HERE / "device-tree", device)
        export_patch(device, device_patches[2],
                     "a6lte: integrate experimental Broadcom HCI FM\n\n"
                     "Package the separate JNI receiver backend and FMRadio, select the\n"
                     "Bluetooth-owned endpoint and grant only system_app socket connect.\n"
                     "Implement EU-band power/tune/readback/seek/scan/cancel/mute/volume\n"
                     "with bounded failures and rollback, plus mock-controller tests.\n\n"
                     "Use current vendor firmware without adding blobs or kernel drivers.\n"
                     "Requires companion patches in system/bt and packages/apps/FMRadio.\n"
                     "Hardware-unverified: protocol/audio assumptions are documented;\n"
                     "RDS/AF, recording and FM-to-Bluetooth audio remain disabled.", 3)
        initialize(args.bt_tree.resolve(), bt)
        overlay(HERE / "bluetooth-stack", bt)
        export_patch(bt, bt_patch,
                     "bt: add device-gated Broadcom FM endpoint\n\n"
                     "Use Fluoride's existing HCI queue/credits rather than sharing the\n"
                     "UART out of band. Authenticate UID 1000 and allow only the needed\n"
                     "FM registers, never arbitrary VSCs or pin configuration.\n\n"
                     "Bound transactions, handle late callbacks without request-owned\n"
                     "pointers, and stop the session before HCI teardown. Attempt FM OFF\n"
                     "and SEC audio route cleanup on client death. Other devices leave\n"
                     "BRCM_FM_HCI_INCLUDED undefined and do not create the endpoint.\n\n"
                     "Experimental A6 receive integration; not hardware validated.\n"
                     "Base: bcca9a572f1cc539ba21ebe940c3642ce8707d1a")
        initialize(args.fm_tree.resolve(), app)
        overlay(HERE / "fm-app", app)
        export_patch(app, fm_patch,
                     "FMRadio: add Broadcom HCI and SEC audio support\n\n"
                     "Select a separate JNI soname and opt-in device resource. Require\n"
                     "Bluetooth enabled and a wired antenna; follow adapter/SCO/focus\n"
                     "lifecycle, bound cancellation and monitor link health. Route through\n"
                     "the Samsung SEC HAL rather than software-looping direct FM audio.\n\n"
                     "Use per-track output selection and receiver digital volume. Keep\n"
                     "RDS/AF unsupported and recording UI disabled for this candidate.\n"
                     "Legacy devices keep the existing libfmjni and audio path.\n\n"
                     "Experimental: exact firmware response and audio route need phone\n"
                     "validation; a stock log is not required to build this candidate.\n"
                     "Base: c8664126026de37a442dba996db8bc4d3606dc41")
        (patches / "series").write_text("".join(name + "\n" for name in DEVICE_PATCHES))
        for path in (bt_patch, fm_patch):
            (path.parent / "series").write_text(path.name + "\n")
        applied_device, applied_bt, applied_app = tmp / "verify-device", tmp / "verify-bt", tmp / "verify-app"
        verify(base, device, applied_device, device_patches)
        verify(args.bt_tree.resolve(), bt, applied_bt, [bt_patch])
        verify(args.fm_tree.resolve(), app, applied_app, [fm_patch])
        subprocess.run(["python3", "-m", "unittest", "discover", "-s", "tools/fm/tests", "-v"],
                       cwd=applied_device, check=True)
        command = ["python3", str(applied_device / "fm/tests/run_host_tests.py"),
                   "--bt-tree", str(applied_bt)]
        if args.sanitize:
            command.append("--sanitize")
        subprocess.run(command, check=True)
        print("PASS: all five patches git-am applied; all three resulting trees match; host tests passed.")


if __name__ == "__main__":
    main()
