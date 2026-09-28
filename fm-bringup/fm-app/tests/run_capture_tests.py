#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run real capture/encoder helpers against mock Android audio/codec boundaries."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--java-home', type=Path, help='JDK with javac/java; otherwise use PATH')
args = p.parse_args()
tests = Path(__file__).resolve().parent
sources = [str(path) for path in (tests / 'capture').rglob('*.java')]
sources += [str(path) for path in (tests / 'encoder').rglob('*.java')]
sources += [str(tests.parent / 'src/com/android/fmradio' / name)
            for name in ('BcmFmCapture.java', 'AudioRecorder.java')]
with tempfile.TemporaryDirectory(prefix='fm-capture-test-') as temp:
    tool = lambda name: str(args.java_home / 'bin' / name) if args.java_home else name
    subprocess.run([tool('javac'), '-source', '8', '-target', '8', '-d', temp, *sources], check=True)
    for test in ('CaptureTest', 'EncoderTest'):
        subprocess.run([tool('java'), '-ea', '-Djava.io.tmpdir=' + temp, '-cp', temp,
                        'com.android.fmradio.' + test], check=True, timeout=30)
