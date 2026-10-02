# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The LineageOS Project
import contextlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import decode_btsnoop as decoder

HEADER = b"btsnoop\0" + struct.pack(">II", 1, 1002)


def command(opcode, params):
    return b"\x01" + struct.pack("<H", opcode) + bytes([len(params)]) + params


def event(code, params):
    return bytes([4, code, len(params)]) + params


def record(packet, time=100, flags=2, drops=0):
    return struct.pack(">IIIIQ", len(packet), len(packet), flags, drops, time) + packet


def decode(*packets):
    return list(decoder.iter_events(io.BytesIO(HEADER + b"".join(record(p) for p in packets))))


class DecoderTests(unittest.TestCase):
    def test_fm_write(self):
        row, = decode(command(0xFC15, b"\x0a\x00\xcc\x5b"))
        self.assertEqual(row["parameters"], "0a00cc5b")
        self.assertEqual(row["register_hint"], "FM_FREQ")
        self.assertEqual(row["access"], "write")
        self.assertEqual(row["value_bytes"], "cc5b")
        self.assertNotIn("frequency", row)  # Firmware semantics aren't assumed.

    def test_fm_read(self):
        row, = decode(command(0xFC15, b"\x12\x01\x02"))
        self.assertEqual(row["requested_bytes"], 2)
        self.assertEqual(row["access"], "read")

    def test_unknown_register_and_mode_preserved(self):
        row, = decode(command(0xFC15, b"\xaa\x03\x77"))
        self.assertEqual(row["register_hint"], "unknown")
        self.assertEqual(row["access"], "unknown")
        self.assertEqual(row["parameters"], "aa0377")

    def test_pin_routing_is_not_interpreted_as_register_access(self):
        row, = decode(command(0xFC61, b"\x07\x19\x18\x19\x19"))
        self.assertEqual(row["operation"], "broadcom_pcm_pin_routing")
        self.assertNotIn("register", row)

    def test_command_complete_retains_echo(self):
        row, = decode(event(0x0E, b"\x01\x15\xfc\x00\x12\x01\x03\x00"))
        self.assertEqual(row["status"], 0)
        self.assertEqual(row["return_parameters"], "12010300")

    def test_error_status(self):
        row, = decode(event(0x0E, b"\x01\x15\xfc\x01"))
        self.assertEqual(row["status"], 1)
        self.assertEqual(row["return_parameters"], "")

    def test_command_status(self):
        row, = decode(event(0x0F, b"\x0c\x01\x61\xfc"))
        self.assertEqual(row["kind"], "command_status")
        self.assertEqual(row["status"], 12)

    def test_fm_interrupt(self):
        row, = decode(event(0xFF, b"\x08\x00"))
        self.assertEqual(row["kind"], "fm_interrupt")

    def test_unrelated_traffic_omitted(self):
        self.assertEqual(decode(
            command(0x0C03, b""),
            command(0xFC01, b"private_vendor_data"),
            event(0x0E, b"\x01\x03\x0c\x00"),
            event(0x0F, b"\x00\x01\x03\x0c"),
            event(0xFF, b"\x09private_vendor_data"),
            event(0x03, b"private_connection_data"),
            b"\x02private_acl_data", b"\x03private_sco_data"), [])

    def test_relative_time_direction_and_drops(self):
        capture = HEADER + record(command(0x0C03, b""), time=10)
        capture += record(command(0xFC15, b"\x00\x00\x01"), time=500)
        capture += record(event(0x0E, b"\x01\x15\xfc\x00"), time=1500, flags=3, drops=4)
        a, b = decoder.iter_events(io.BytesIO(capture))
        self.assertEqual(a["elapsed_us"], 0)
        self.assertEqual(a["record"], 2)
        self.assertEqual(b["elapsed_us"], 1000)
        self.assertEqual(b["direction"], "controller_to_host")
        self.assertEqual(b["cumulative_drops"], 4)
        self.assertNotIn("timestamp", a)

    def test_backward_clock_preserved(self):
        p = command(0xFC15, b"\x00\x00\x01")
        rows = list(decoder.iter_events(io.BytesIO(HEADER + record(p, 20) + record(p, 10))))
        self.assertEqual(rows[1]["elapsed_us"], -10)

    def test_empty_valid_capture(self):
        self.assertEqual(decode(), [])

    def test_invalid_headers(self):
        for header in (b"", b"btsnoop", b"wrong!!!" + HEADER[8:],
                       b"btsnoop\0" + struct.pack(">II", 2, 1002),
                       b"btsnoop\0" + struct.pack(">II", 1, 1001)):
            with self.subTest(header=header), self.assertRaises(decoder.CaptureError):
                list(decoder.iter_events(io.BytesIO(header)))

    def test_truncated_record_header(self):
        for n in range(1, 24):
            with self.subTest(length=n), self.assertRaises(decoder.CaptureError):
                list(decoder.iter_events(io.BytesIO(HEADER + bytes(n))))

    def test_truncated_payload(self):
        with self.assertRaises(decoder.CaptureError):
            list(decoder.iter_events(io.BytesIO(HEADER + struct.pack(">IIIIQ", 4, 4, 0, 0, 0) + b"\x01")))

    def test_rejects_capture_truncation_and_oversized_records(self):
        for orig, incl in ((3, 2), (2, 3), (0, 0), (65541, 65541), (0xFFFFFFFF, 0xFFFFFFFF)):
            with self.subTest(orig=orig, incl=incl), self.assertRaises(decoder.CaptureError):
                list(decoder.iter_events(io.BytesIO(HEADER + struct.pack(">IIIIQ", orig, incl, 0, 0, 0))))

    def test_malformed_hci_lengths(self):
        for packet in (b"", b"\x01", b"\x01\x15\xfc\x04\x00",
                       b"\x04", b"\x04\x0e\x04\x00",
                       command(0xFC15, b"\x00\x00"),
                       event(0x0E, b"\x01\x15"), event(0x0E, b"\x01\x15\xfc"),
                       event(0x0F, b"\x00\x01\x15")):
            with self.subTest(packet=packet), self.assertRaises(decoder.CaptureError):
                decoder.decode_packet(packet)

    def run_cli(self, content):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.log"
            path.write_bytes(content)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = decoder.main([str(path)])
            return status, out.getvalue(), err.getvalue()

    def test_cli_success(self):
        status, out, err = self.run_cli(HEADER + record(command(0xFC15, b"\x00\x00\x01")))
        self.assertEqual(status, 0)
        self.assertIn('"opcode": "0xfc15"', out)
        self.assertIn("Extracted 1", err)

    def test_cli_no_data_is_not_success(self):
        status, out, err = self.run_cli(HEADER)
        self.assertEqual(status, 1)
        self.assertEqual(out, "")
        self.assertIn("does not prove absence", err)

    def test_cli_invalid_input_is_not_success(self):
        status, out, err = self.run_cli(b"wrong")
        self.assertEqual(status, 2)
        self.assertIn("discard any partial output", err)

    def test_cli_missing_file(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(decoder.main([str(Path(directory) / "missing")]), 2)

    def test_cli_partial_output_failure_is_explicit(self):
        status, out, err = self.run_cli(HEADER + record(command(0xFC15, b"\x00\x00\x01")) + b"x")
        self.assertEqual(status, 2)
        self.assertIn('"opcode"', out)
        self.assertIn("discard any partial output", err)


if __name__ == "__main__":
    unittest.main()
