#!/usr/bin/env python3
"""Decode the structurally verified PS5 EMC log and label known fields cautiously.

The 2,020-byte response layout is verified by the native reader. Human-readable
error and sequence labels are community-derived and are identified as such in
the generated report; unknown values are never guessed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from collections import Counter
from pathlib import Path

LOG_SIZE = 2020
SLOT_SIZE = 32
SLOT_COUNT = 63

CODE_LABELS = {
    0x80000009: "AC-in detection / unexpected power-loss or shutdown event",
    0x80800000: "OS-requested fatal shutdown / kernel-panic shutdown family",
    0x808E0005: "Abnormal host command: wait-SIG1 error",
    0x808E0006: "Abnormal host command: host reset while command error active",
    0x808E0109: "TPM-related (public detail unavailable)",
    0xB0082108: "Wi-Fi module or Sonics-bus fault",
    0xB0088108: "Rebuild DBI or storage-controller initialization failure",
    0xC0020303: "Main SoC/APU SB-TSI I2C access failure",
}

SEQUENCE_LABELS = {
    0x2013: "Stop subsystem PG2 bus-error detection (DDR4 buffer overflow)",
    0x217B: "WLAN/BT reset sequence",
    0x219F: "Drive fan control stop",
}

UPCAUSE_BITS = {
    26: "DEV UART",
    19: "Bluetooth",
    18: "HDMI-CEC",
    17: "EAP order",
    16: "Main SoC",
    10: "Eject button",
    9: "Disc loaded",
    8: "Power button",
    0: "Power-on",
}

DEVPM_BITS = {
    4: "HDMI 5V",
    3: "BD drive",
    2: "HDMI-CEC",
    1: "USB",
    0: "WLAN",
}

SOURCES = [
    "https://github.com/apewalkers/PS5_NOR_Decode",
    "https://github.com/Liams-Electronics-Lab/PS5-Error-Codes-",
]


def flags(value: int, known: dict[int, str]) -> tuple[list[str], int]:
    labels = [label for bit, label in known.items() if value & (1 << bit)]
    mask = sum(1 << bit for bit in known)
    return labels, value & ~mask


def decode(data: bytes) -> dict:
    if len(data) != LOG_SIZE:
        raise ValueError(f"expected {LOG_SIZE} bytes, got {len(data)}")
    header = struct.unpack_from("<I", data)[0]
    records = []
    for index in range(SLOT_COUNT):
        words = struct.unpack_from("<8I", data, 4 + index * SLOT_SIZE)
        code, rtc, power, upcause, sequence_word, telemetry, pad0, pad1 = words
        sequence = sequence_word & 0xFFFF
        sequence_prefix = sequence_word >> 16
        devpm = telemetry >> 16
        soc_temperature_raw = telemetry & 0xFFFF
        upcause_labels, upcause_unknown = flags(upcause, UPCAUSE_BITS)
        devpm_labels, devpm_unknown = flags(devpm, DEVPM_BITS)
        records.append({
            "slot": index,
            "code": f"{code:08X}",
            "codeLabel": CODE_LABELS.get(code),
            "rtcRaw": f"{rtc:08X}",
            "powerStateRaw": f"{power:08X}",
            "upCauseRaw": f"{upcause:08X}",
            "upCauseKnown": upcause_labels,
            "upCauseUnknownMask": f"{upcause_unknown:08X}",
            "sequencePrefixRaw": f"{sequence_prefix:04X}",
            "sequence": f"{sequence:04X}",
            "sequenceLabel": SEQUENCE_LABELS.get(sequence),
            "devicePowerRaw": f"{devpm:04X}",
            "devicePowerKnown": devpm_labels,
            "devicePowerUnknownMask": f"{devpm_unknown:04X}",
            "socTemperatureRaw": f"{soc_temperature_raw:04X}",
            "socTemperatureC": round(soc_temperature_raw / 256.0, 2),
            "padding": [f"{pad0:08X}", f"{pad1:08X}"],
        })
    counts = Counter(record["code"] for record in records)
    return {
        "schema": 1,
        "decoderPolicy": {
            "layout": "verified from the 13.60 ICC response and reader implementation",
            "labels": "community-derived; null means unknown; no inferred labels",
            "sources": SOURCES,
        },
        "input": {
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "headerLe": f"0x{header:08X}",
            "slotSize": SLOT_SIZE,
            "slotCount": SLOT_COUNT,
        },
        "codeCounts": [
            {"code": code, "count": count,
             "label": CODE_LABELS.get(int(code, 16))}
            for code, count in counts.most_common()
        ],
        "records": records,
    }


def text_report(report: dict) -> str:
    lines = [
        "PS5 EMC decoded report",
        "",
        "WARNING: Record structure is verified; labels are community-derived.",
        "Unknown values are intentionally left unknown.",
        "",
        f"Input SHA-256: {report['input']['sha256']}",
        f"Header: {report['input']['headerLe']}",
        "",
        "Code summary:",
    ]
    for row in report["codeCounts"]:
        lines.append(f"  {row['count']:2d} x {row['code']}  {row['label'] or 'UNKNOWN'}")
    lines.extend(["", "Records:"])
    for row in report["records"]:
        up = ", ".join(row["upCauseKnown"]) or "none identified"
        dev = ", ".join(row["devicePowerKnown"]) or "none identified"
        lines.extend([
            f"[{row['slot']:02d}] {row['code']} - {row['codeLabel'] or 'UNKNOWN'}",
            f"     rtc={row['rtcRaw']} power={row['powerStateRaw']}",
            f"     upcause={row['upCauseRaw']} ({up}; unknown-mask={row['upCauseUnknownMask']})",
            f"     sequence={row['sequence']} ({row['sequenceLabel'] or 'UNKNOWN'}), "
            f"prefix={row['sequencePrefixRaw']}",
            f"     devpm={row['devicePowerRaw']} ({dev}; unknown-mask={row['devicePowerUnknownMask']}), "
            f"SoC={row['socTemperatureC']:.2f} C (raw {row['socTemperatureRaw']})",
        ])
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--text", type=Path, required=True)
    args = parser.parse_args()
    report = decode(args.input.read_bytes())
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    args.text.write_text(text_report(report), encoding="utf-8")


if __name__ == "__main__":
    main()
