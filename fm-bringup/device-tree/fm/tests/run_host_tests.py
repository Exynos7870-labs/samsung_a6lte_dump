#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Linux host tests. Android HCI/audio/peer-credential APIs are mocked in broker tests.

These tests do not build Android, compile SELinux policy, or verify hardware.
Never run the socket-broker test executable on a phone.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bt-tree", type=Path, help="patched system/bt source tree")
    parser.add_argument("--sanitize", action="store_true", help="enable ASan and UBSan")
    args = parser.parse_args()
    if os.environ.get("ANDROID_ROOT"):
        parser.error("host-only test: do not run the mock Bluetooth endpoint on Android")
    tests = Path(__file__).resolve().parent
    source = tests.parent
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="bcm-fm-host-") as directory:
        binary = str(Path(directory) / "radio-test")
        subprocess.run(["g++", *flags, "-I" + str(source), str(source / "BcmFmRadio.cpp"), str(source / "RdsDecoder.cpp"),
                        str(tests / "radio_test.cpp"), "-o", binary], check=True)
        subprocess.run([binary], check=True)
        binary = str(Path(directory) / "rds-test")
        subprocess.run(["g++", *flags, "-I" + str(source), str(source / "RdsDecoder.cpp"),
                        str(tests / "rds_test.cpp"), "-o", binary], check=True)
        subprocess.run([binary], check=True)
        if args.bt_tree:
            bt = args.bt_tree.resolve()
            header = bt / "btif/include/BcmFmProtocol.h"
            if header.read_bytes() != (source / "BcmFmProtocol.h").read_bytes():
                parser.error("device and Bluetooth protocol headers differ")
            implementation = bt / "btif/src/btif_bcm_fm.cc"
            if '"' in str(implementation):
                parser.error("source path cannot contain quotes")
            binary = str(Path(directory) / "broker-test")
            subprocess.run(["g++", *flags, "-I" + str(tests / "stack_stubs"),
                            "-I" + str(bt / "btif/include"),
                            '-DBCM_FM_SERVER_SOURCE="' + str(implementation) + '"',
                            str(tests / "server_test.cpp"), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    main()
