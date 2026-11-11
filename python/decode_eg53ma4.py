#!/usr/bin/env python3
"""Reference decoder for the Schrader EG53MA4 433.92 MHz TPMS frame.

This is a host-side companion to the article, not the product firmware.
It takes a `row=` hex string and the bit count from the serial log,
finds the zero-run boundary, extracts the 10 bytes, checks the checksum,
and prints the fields.

Protocol model (from the public rtl_433 decoder, contributed by
Leonardo Hamada):

    120 bits = 40 zero bits + 10 bytes
    b[0..3] header, b[4..6] ID, b[7] pressure (2.5 kPa/count),
    b[8] temperature in F, b[9] checksum = sum(b[0..8]) & 0xFF.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

HEADER_WHITELIST = {0x4D920052, 0x4D930052, 0x4C960052}
MIN_SYNC_ZEROS = 24
FRAME_BYTES = 10
PAYLOAD_BITS = 80


@dataclass(frozen=True)
class Frame:
    header: int
    sensor_id: int
    pressure_kpa: float
    temperature_f: int
    recovered_119: bool = False


def row_to_bits(row: str, bit_count: int | None = None) -> list[int]:
    """Convert a row= hex string to a bit list, truncating to bit_count."""
    text = row.strip().lower().replace("row=", "").replace(" ", "")
    bits: list[int] = []
    for ch in text:
        value = int(ch, 16)
        bits.extend((value >> shift) & 1 for shift in (3, 2, 1, 0))
    if bit_count is not None:
        bits = bits[:bit_count]
    return bits


def longest_zero_run(bits: list[int]) -> tuple[int, int, int]:
    """Return (start, end, length) of the longest run of zeros."""
    best_start = best_end = best_len = 0
    i = 0
    while i < len(bits):
        if bits[i] != 0:
            i += 1
            continue
        j = i
        while j < len(bits) and bits[j] == 0:
            j += 1
        if j - i > best_len:
            best_start, best_end, best_len = i, j, j - i
        i = j
    return best_start, best_end, best_len


def extract_byte(bits: list[int], pos: int) -> int:
    value = 0
    for bit in bits[pos:pos + 8]:
        value = (value << 1) | bit
    return value


def normalize_inverted(bits: list[int]) -> list[int] | None:
    """Handle the 7FFF...CD row: leading 0, >=24 ones, MSB of byte 0 set."""
    if len(bits) < 104 or bits[0] != 0:
        return None
    ones = 0
    for bit in bits[1:]:
        if bit == 0:
            break
        ones += 1
    if ones < 24:
        return None
    out = bits[:]
    for i in range(1, min(40, len(out))):
        out[i] = 0
    if len(out) > 40:
        out[40] = 0
    return out


def decode_at(bits: list[int], start: int, allow_119: bool,
              header_check: bool) -> tuple[Frame | None, str]:
    available = len(bits) - start
    recovered = False
    if available < PAYLOAD_BITS:
        if allow_119 and available == PAYLOAD_BITS - 1:
            bits = bits + [0]
            available = PAYLOAD_BITS
            recovered = True
        else:
            return None, "payload incomplete: not enough bits after the sync (need 80)"

    b = [extract_byte(bits, start + i * 8) for i in range(FRAME_BYTES)]
    checksum = sum(b[:9]) & 0xFF
    if checksum != b[9]:
        return None, f"checksum mismatch: got 0x{b[9]:02X}, expected 0x{checksum:02X}"

    header = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
    if header_check and header not in HEADER_WHITELIST:
        return None, f"header 0x{header:08X} not in whitelist"

    sensor_id = (b[4] << 16) | (b[5] << 8) | b[6]
    frame = Frame(header, sensor_id, b[7] * 2.5, b[8], recovered)
    return frame, "ok"


def decode_bits(bits: list[int], allow_119: bool = True,
                header_check: bool = True) -> tuple[Frame | None, str]:
    """Decode a bit row.

    The payload starts at the end of the longest zero run. Because the
    header can start with 0, the first 1 may be one bit inside the
    payload; try both the end of the run and the bit before it.
    """
    _, end, length = longest_zero_run(bits)
    if length < MIN_SYNC_ZEROS:
        return None, f"no sync: longest zero run is {length} bits; need at least {MIN_SYNC_ZEROS}"

    candidates = [end]
    if end > MIN_SYNC_ZEROS:
        candidates.append(end - 1)

    last_error = "no candidate"
    for start in dict.fromkeys(candidates):
        frame, note = decode_at(bits, start, allow_119, header_check)
        if frame is not None:
            return frame, note
        if last_error == "no candidate":
            last_error = note
    return None, last_error


def decode_row(row: str, bit_count: int | None = None,
               allow_119: bool = True,
               header_check: bool = True) -> tuple[Frame | None, str]:
    bits = row_to_bits(row, bit_count)
    frame, note = decode_bits(bits, allow_119, header_check)
    if frame is not None:
        return frame, note

    normalized = normalize_inverted(bits)
    if normalized is not None:
        frame, _ = decode_bits(normalized, allow_119, header_check)
        if frame is not None:
            return frame, "normalized inverted sync"

    return None, note


def format_frame(frame: Frame) -> str:
    psi = frame.pressure_kpa / 6.89476
    bar = frame.pressure_kpa / 100.0
    celsius = (frame.temperature_f - 32.0) * 5.0 / 9.0
    suffix = " (119-bit, trailing zero)" if frame.recovered_119 else ""
    return (
        f"[frame] header=0x{frame.header:08X} id={frame.sensor_id:06X} "
        f"pressure={frame.pressure_kpa:.1f} kPa "
        f"({psi:.1f} PSI / {bar:.2f} bar) "
        f"temp={frame.temperature_f} F ({celsius:.1f} C) checksum=OK{suffix}"
    )


def decode_line(row: str, bit_count: int | None, header_check: bool = True) -> bool:
    frame, note = decode_row(row, bit_count, header_check=header_check)
    print(f"[row] row={row} bits={bit_count if bit_count is not None else '?'}")
    if frame is None:
        print(f"[reject] {note}")
        return False
    print(format_frame(frame))
    if note != "ok":
        print(f"[note] {note}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode an EG53MA4 row= hex string.")
    parser.add_argument("--row", help="row= hex string")
    parser.add_argument("--bits", type=int, help="bit count from the log")
    parser.add_argument(
        "--file",
        default=str(Path(__file__).with_name("data") / "rows.txt"),
        help="file with 'row bits' per line (default: data/rows.txt)",
    )
    parser.add_argument(
        "--no-119-repair",
        action="store_true",
        help="do not append the missing zero to 119-bit rows",
    )
    args = parser.parse_args()
    allow_119 = not args.no_119_repair

    if args.row:
        bits = args.bits
        if bits is None:
            bits = len(args.row.replace("row=", "").strip()) * 4
        return 0 if decode_line(args.row.strip(), bits) else 1

    path = Path(args.file)
    if not path.exists():
        parser.error(f"file not found: {path}")

    accepted = rejected = 0
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        row = parts[0]
        bits = int(parts[1]) if len(parts) > 1 else None
        if decode_line(row, bits):
            accepted += 1
        else:
            rejected += 1
    print(f"[summary] decoded={accepted} rejected={rejected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
