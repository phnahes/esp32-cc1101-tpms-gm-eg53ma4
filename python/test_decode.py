#!/usr/bin/env python3
"""Tests for the EG53MA4 reference decoder."""

from __future__ import annotations

import unittest

from decode_eg53ma4 import decode_row


class TestEG53MA4(unittest.TestCase):
    def test_clean_120_bits(self) -> None:
        frame, note = decode_row("00000000004d9200520066e91a5af4", 120)
        self.assertIsNotNone(frame)
        assert frame is not None
        self.assertEqual(frame.header, 0x4D920052)
        self.assertEqual(frame.sensor_id, 0x0066E9)
        self.assertAlmostEqual(frame.pressure_kpa, 65.0)
        self.assertEqual(frame.temperature_f, 90)
        self.assertFalse(frame.recovered_119)
        self.assertEqual(note, "ok")

    def test_121_bit_inverted_sync(self) -> None:
        frame, note = decode_row("7FFFFFFFFFCD930052006AC655550C0", 121)
        self.assertIsNotNone(frame)
        assert frame is not None
        self.assertEqual(frame.header, 0x4D930052)
        self.assertEqual(frame.sensor_id, 0x006AC6)
        self.assertAlmostEqual(frame.pressure_kpa, 212.5)
        self.assertEqual(frame.temperature_f, 85)
        self.assertEqual(note, "normalized inverted sync")

    def test_119_bit_trailing_zero_repair(self) -> None:
        frame, note = decode_row("00000000004d9200520066e91a5af4", 119)
        self.assertIsNotNone(frame)
        assert frame is not None
        self.assertEqual(frame.sensor_id, 0x0066E9)
        self.assertTrue(frame.recovered_119)
        self.assertEqual(note, "ok")

    def test_119_bit_repair_can_be_disabled(self) -> None:
        frame, note = decode_row("00000000004d9200520066e91a5af4", 119, allow_119=False)
        self.assertIsNone(frame)
        self.assertIn("payload incomplete", note)

    def test_other_row_same_family(self) -> None:
        # A row that is not in data/rows.txt; it must decode with the same
        # header family, not only the examples.
        frame, note = decode_row("00000000004D930052006AC655550C0", 120)
        self.assertIsNotNone(frame)
        assert frame is not None
        self.assertEqual(frame.header, 0x4D930052)
        self.assertEqual(frame.sensor_id, 0x006AC6)
        self.assertAlmostEqual(frame.pressure_kpa, 212.5)
        self.assertEqual(frame.temperature_f, 85)
        self.assertEqual(note, "ok")

    def test_truncated_rows_are_rejected(self) -> None:
        rows = [
            ("00000000009B2400A400", 79),
            ("00000000004D9300520066E948", 101),
            ("00000000009B2600A400", 78),
            ("0000009B2600A400CD4CC2ABE8", 104),
            ("7FFF", 16),
            ("7FFFFFFFFF", 40),
        ]
        for row, bits in rows:
            with self.subTest(row=row, bits=bits):
                frame, _ = decode_row(row, bits)
                self.assertIsNone(frame)


if __name__ == "__main__":
    unittest.main()
