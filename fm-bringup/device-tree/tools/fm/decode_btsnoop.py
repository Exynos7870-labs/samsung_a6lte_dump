#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The LineageOS Project
"""Extract Broadcom FM/PCM transactions from an Android H4 btsnoop capture.

Offline only: never connects to a phone or transmits HCI commands. Output is
JSON Lines and omits unrelated Bluetooth traffic and absolute timestamps.
Register names are protocol hints, not proof of a particular firmware's ABI.
"""

import argparse
import json
from pathlib import Path
import struct
import sys

MAGIC = b"btsnoop\0"
H4_DATALINK = 1002
MAX_RECORD = 65540  # H4 ACL packet: type + 4-byte header + uint16 payload
OPCODES = {0xFC15: "broadcom_fm_register", 0xFC61: "broadcom_pcm_pin_routing"}
REGISTERS = {
    0x00: "RDS_SYS", 0x01: "FM_CTRL", 0x02: "RDS_CTL0",
    0x04: "AUD_PAUS", 0x05: "AUD_CTL0", 0x06: "AUD_CTL1",
    0x07: "SCH_CTL0", 0x08: "SEARCH_SNR", 0x09: "SCH_TUNE",
    0x0A: "FM_FREQ", 0x0B: "FM_FREQ1", 0x0F: "RSSI",
    0x10: "FM_RDS_MSK", 0x11: "FM_RDS_MSK1",
    0x12: "FM_RDS_FLAG", 0x13: "FM_RDS_FLAG1", 0x14: "RDS_WLINE",
    0x28: "RCV_ID", 0x29: "CFG", 0x4D: "PCM_ROUTE", 0x80: "RDS_DATA",
    0xDE: "PRESCAN_QUALITY", 0xDF: "SNR", 0xF8: "VOLUME_CTRL",
    0xF9: "BLEND_MUTE", 0xFB: "SEARCH_BOUNDARY", 0xFC: "SEARCH_METHOD",
    0xFD: "SCH_STEP", 0xFE: "PRESET_MAX", 0xFF: "PRESET_STA",
}


class CaptureError(ValueError):
    """A capture is incomplete, unsupported, or malformed."""


def read_exact(stream, size, what):
    data = stream.read(size)
    if len(data) != size:
        raise CaptureError("truncated " + what)
    return data


def decode_packet(packet):
    """Return a selected event, or None. Preserve raw FM bytes without guessing."""
    if not packet:
        raise CaptureError("empty H4 packet")
    packet_type = packet[0]
    if packet_type == 0x01:
        if len(packet) < 4 or len(packet) != 4 + packet[3]:
            raise CaptureError("invalid H4 command length")
        opcode = int.from_bytes(packet[1:3], "little")
        if opcode not in OPCODES:
            return None
        payload = packet[4:]
        result = {"kind": "command", "opcode": f"0x{opcode:04x}",
                  "operation": OPCODES[opcode], "parameters": payload.hex()}
        if opcode == 0xFC15:
            if len(payload) < 3:
                raise CaptureError("short FM register command")
            reg, mode = payload[:2]
            result["register"] = f"0x{reg:02x}"
            result["register_hint"] = REGISTERS.get(reg, "unknown")
            result["access"] = {0: "write", 1: "read"}.get(mode, "unknown")
            # Retain unknown access modes rather than interpreting another ABI.
            if mode == 1:
                result["requested_bytes"] = payload[2]
            elif mode == 0:
                result["value_bytes"] = payload[2:].hex()
        return result
    if packet_type != 0x04:
        return None  # ACL/SCO/ISO data never appears in the output.
    if len(packet) < 3 or len(packet) != 3 + packet[2]:
        raise CaptureError("invalid H4 event length")
    event, params = packet[1], packet[3:]
    if event == 0x0E:
        if len(params) < 3:
            raise CaptureError("short Command Complete event")
        opcode = int.from_bytes(params[1:3], "little")
        if opcode not in OPCODES:
            return None
        if len(params) < 4:
            raise CaptureError("missing vendor command status")
        return {"kind": "command_complete", "opcode": f"0x{opcode:04x}",
                "status": params[3], "return_parameters": params[4:].hex()}
    if event == 0x0F:
        if len(params) != 4:
            raise CaptureError("invalid Command Status event")
        opcode = int.from_bytes(params[2:4], "little")
        if opcode in OPCODES:
            return {"kind": "command_status", "opcode": f"0x{opcode:04x}",
                    "status": params[0]}
    if event == 0xFF and params[:1] == b"\x08":
        return {"kind": "fm_interrupt", "parameters": params.hex()}
    return None


def iter_events(stream):
    header = read_exact(stream, 16, "btsnoop header")
    if header[:8] != MAGIC:
        raise CaptureError("not btsnoop (btsnooz/bugreport containers must be extracted first)")
    version, datalink = struct.unpack(">II", header[8:])
    if version != 1 or datalink != H4_DATALINK:
        raise CaptureError(f"expected btsnoop version 1 / H4 datalink 1002; got {version}/{datalink}")
    record = 0
    first_selected_time = None
    while True:
        raw_header = stream.read(24)
        if not raw_header:
            return
        record += 1
        if len(raw_header) != 24:
            raise CaptureError(f"record {record}: truncated record header")
        original, included, flags, drops, timestamp = struct.unpack(">IIIIQ", raw_header)
        if original != included:
            raise CaptureError(f"record {record}: truncated packet ({included}/{original} bytes)")
        if not 1 <= included <= MAX_RECORD:
            raise CaptureError(f"record {record}: invalid packet size {included}")
        packet = read_exact(stream, included, f"record {record} packet")
        try:
            event = decode_packet(packet)
        except CaptureError as exc:
            raise CaptureError(f"record {record}: {exc}") from exc
        if event is None:
            continue
        if first_selected_time is None:
            first_selected_time = timestamp
        yield {"record": record, "elapsed_us": timestamp - first_selected_time,
               "direction": "controller_to_host" if flags & 1 else "host_to_controller",
               "cumulative_drops": drops, **event}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="uncompressed Android H4 btsnoop file")
    args = parser.parse_args(argv)
    count = 0
    try:
        with args.capture.open("rb") as stream:
            for event in iter_events(stream):
                print(json.dumps(event, sort_keys=True))
                count += 1
    except (OSError, CaptureError) as exc:
        print(f"error: {exc}; discard any partial output", file=sys.stderr)
        return 2
    if count == 0:
        print("No selected FM/PCM transactions found. This does not prove absence of FM: "
              "logging may filter vendor commands, or the transport may differ.", file=sys.stderr)
        return 1
    print(f"Extracted {count} FM/PCM records. PCM pin commands alone are not proof of FM.",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
