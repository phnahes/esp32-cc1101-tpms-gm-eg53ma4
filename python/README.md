# EG53MA4 reference decoder

Host-side companion to the ESP32 receiver in [`../README.md`](../README.md).

Decoder for the Schrader **EG53MA4** 433.92 MHz TPMS frame. It is not the product firmware. It takes a `row=` hex string and the bit count from the serial log, finds the payload boundary, verifies the checksum, and prints the sensor ID, pressure, and temperature.

## Protocol model

```text
120 bits = 40 zero bits + 10 bytes
b[0..3] header, b[4..6] ID, b[7] pressure (2.5 kPa/count),
b[8] temperature in F, b[9] checksum = sum(b[0..8]) & 0xFF
```

The payload boundary is the end of the longest zero run. Because the header can start with `0`, the decoder also tries one bit earlier; the checksum decides. The 121-bit `7FFF...` row is normalized by zeroing bits 1..39 and bit 40. A 119-bit row is recovered only when the missing trailing bit is `0`.

## Usage

Decode the rows in `data/rows.txt`:

```bash
python3 decode_eg53ma4.py
```

Decode one row:

```bash
python3 decode_eg53ma4.py --row 00000000004d9200520066e91a5af4 --bits 120
```

Run the tests:

```bash
python3 -m unittest -v test_decode.py
```

No dependencies beyond the Python standard library.

## Example output

```text
[row] row=00000000004d9200520066e91a5af4 bits=120
[frame] header=0x4D920052 id=0066E9 pressure=65.0 kPa (9.4 PSI / 0.65 bar) temp=90 F (32.2 C) checksum=OK

[row] row=7FFFFFFFFFCD930052006AC655550C0 bits=121
[frame] header=0x4D930052 id=006AC6 pressure=212.5 kPa (30.8 PSI / 2.12 bar) temp=85 F (29.4 C) checksum=OK
[note] normalized inverted sync
```

The truncated rows are rejected:

```text
[row] row=00000000009B2400A400 bits=79
[reject] payload incomplete: not enough bits after the sync (need 80)

[row] row=0000009B2600A400CD4CC2ABE8 bits=104
[reject] checksum mismatch: got 0xE8, expected 0xEB
```

The 119-bit synthetic row (the clean frame with the last bit removed) is recovered because the missing bit is zero:

```text
[row] row=00000000004d9200520066e91a5af4 bits=119
[frame] header=0x4D920052 id=0066E9 pressure=65.0 kPa (9.4 PSI / 0.65 bar) temp=90 F (32.2 C) checksum=OK (119-bit, trailing zero)
```

## Test rows

| `row=` | Bits | Result |
|---|---:|---|
| `00000000004d9200520066e91a5af4` | 120 | clean frame, ID `0066E9` |
| `7FFFFFFFFFCD930052006AC655550C0` | 121 | normalized, ID `006AC6` |
| `00000000009B2400A400` | 79 | rejected: payload incomplete |
| `00000000004D9300520066E948` | 101 | rejected: payload incomplete |
| `00000000009B2600A400` | 78 | rejected: payload incomplete |
| `0000009B2600A400CD4CC2ABE8` | 104 | rejected: checksum mismatch |
| `7FFF` | 16 | rejected: no sync |
| `7FFFFFFFFF` | 40 | rejected: no sync |
| `00000000004d9200520066e91a5af4` | 119 | synthetic; recovered with trailing zero |

## Notes and limits

- The decoder is fitted to the headers used in the article (`0x4D920052`, `0x4D930052`, `0x4C960052`). It decodes any row from that family, not only the examples in `data/rows.txt`; use `--row` and `--bits` for a new row.
- A different header family needs a different boundary rule. The current rule assumes the known headers start with `01`, so the zero run is 41 bits and the payload starts one bit before its end.
- The decoder is intentionally small. It does not do ID-to-wheel mapping, noise filtering, or the product's anti-ghost logic.
- The 121-bit row is an alignment artifact, not a different protocol. The normalization is empirical.
- The 119-bit case is conditional: if the missing bit is `1`, the appended `0` fails the checksum and the row is rejected.
- The code is a reference implementation. It does not contain the product firmware or its configuration.

## License and credits

The frame model comes from the EG53MA4 decoder in [`rtl_433`](https://github.com/merbanan/rtl_433), contributed by **Leonardo Hamada** (`hkazu`). That project is GPL-2.0; this reference decoder is distributed under **GPL-2.0-or-later**. See `../LICENSE`.

Article: [From Noise to Tire Pressure: Reverse Engineering a 433 MHz TPMS Protocol](https://medium.com/@phnahes/from-noise-to-tire-pressure-reverse-engineering-a-433-mhz-tpms-protocol-1bd8e7c28920)
