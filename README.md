# esp32-cc1101-tpms-gm-eg53ma4

Reference receiver and decoder for the Schrader **EG53MA4** 433.92 MHz TPMS sensor, using an **Ebyte E07-M1101D** module (CC1101-based) and an **ESP32-S3**.

This is a standalone reference implementation for the article. It is not the product firmware. It has no NVS mapping, WiFi portal, OTA, cache, or ID-to-wheel bookkeeping. It configures the radio, captures GDO0, slices the Manchester waveform, decodes the frame, and prints the fields on the serial console.

## Hardware

- ESP32-S3 board (the example uses `esp32-s3-devkitc-1`; any ESP32 with the same pins works).
- Ebyte **E07-M1101D** module, 433 MHz, 8-pin header, 3.3 V only.
- 433 MHz antenna, approximately 16.5 cm.
- The module is built around the TI CC1101, but its header is not the CC1101 chip pinout. GDO0 is pin 3 and GDO2 is pin 8; MISO/GDO1 is shared.

### Wiring

| E07-M1101D | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCK | GPIO40 |
| MISO | GPIO41 |
| MOSI | GPIO42 |
| CSN | GPIO47 |
| GDO0 | GPIO8 |
| GDO2 | GPIO15 (wired; not used by this example) |
| Antenna | 433 MHz, ~16.5 cm |

The module has an 8-pin 2×4 header. Do not wire from a generic CC1101 breakout pinout; the module order is its own. GDO2 is wired on the board but the firmware ignores it, capturing every edge on GDO0 and filtering noise in software.

## Protocol summary

The clean frame is **120 bits**: **40 zeros of preamble** plus **10 bytes**.

| Bytes | Field | Encoding |
|---|---|---|
| `b[0..3]` | Header / status word | Observed values: `0x4D920052`, `0x4D930052`, `0x4C960052`; semantics unknown |
| `b[4..6]` | Sensor ID | 24 bits |
| `b[7]` | Pressure | 2.5 kPa per count (25 mbar / 0.025 bar / ~0.36 PSI) |
| `b[8]` | Temperature | Degrees Fahrenheit |
| `b[9]` | Checksum | `sum(b[0..8]) & 0xFF` |

The decoder also handles two transmission artifacts:

- **121-bit row** (`7FFFFFFFFFCD93...`): a one-symbol alignment artifact. The decoder zeroes bits 1..39 and bit 40, then decodes the 120-bit frame.
- **119-bit row**: the last bit is lost. The decoder appends a trailing `0` only when the checksum then closes; if the missing bit was `1`, the row is rejected.

The frame model comes from the public `rtl_433` decoder, contributed by Leonardo Hamada. The receiver-side logic is this project's reference implementation.

## Repository layout

```text
.
├── platformio.ini          ESP32-S3 build
├── src/
│   ├── main.cpp            CC1101 setup, GDO0 capture, slicer, serial console, self-test
│   ├── decode_eg53ma4.cpp  Bit-level frame decoder
│   └── decode_eg53ma4.h    Decoder API
├── test/
│   └── test_decode.cpp     Host-side C++ decoder tests
├── python/
│   ├── decode_eg53ma4.py   Host-side reference decoder
│   ├── test_decode.py      Python tests
│   ├── data/rows.txt       Test rows
│   └── README.md           Python usage and row table
├── scripts/
│   └── test_host.sh        Runs the C++ and Python tests without a board
├── CHANGELOG.md
└── LICENSE                 GPL-2.0
```

## Build and upload

```bash
pio run -e esp32-s3
pio run -e esp32-s3 -t upload
pio device monitor -b 115200
```

The sketch boots with a self-test of the decoder and then listens for serial lines. Send a row with its bit count:

```text
00000000004d9200520066e91a5af4 120
```

The receiver also prints frames as they arrive from the radio:

```text
[frame] header=0x4D930052 id=006AC6 pressure=212.5 kPa (30.8 PSI / 2.12 bar) temp=85 F (29.4 C) checksum=OK
```

## CC1101 configuration

| Register | Value | Meaning |
|---|---|---|
| `FREQ2/1/0` | `0x10/0xB0/0x71` | 433.9198 MHz center |
| `MDMCFG4` | `0x07` | RX filter ~812.5 kHz; data-rate exponent 7 |
| `MDMCFG3` | `0x93` | Data rate ~5.0 kbps (setting) |
| `MDMCFG2` | `0x32` | ASK/OOK; sync setting unused in async mode |
| `PKTCTRL0` | `0x32` | Async serial, no CRC, no whitening |
| `IOCFG0` | `0x0D` | GDO0 = serial data output |
| `IOCFG2` | `0x0E` | GDO2 = carrier sense |
| `MCSM1` | `0x3C` | Stay in RX after packet |

The code checks `PARTNUM=0x00`, `VERSION=0x14`, and `MARCSTATE=0x0D` at boot. If the module is not connected, the decoder self-test and the serial row decoder still run; only the RF path is unavailable.

## Python decoder

The Python script is the host-side reference and test harness. It is not limited to the rows in `data/rows.txt`; it accepts any `row=` and bit count from the same header family:

```bash
cd python
python3 decode_eg53ma4.py --row 00000000004D930052006AC655550C0 --bits 120
```

Output:

```text
[row] row=00000000004D930052006AC655550C0 bits=120
[frame] header=0x4D930052 id=006AC6 pressure=212.5 kPa (30.8 PSI / 2.12 bar) temp=85 F (29.4 C) checksum=OK
```

Run all rows:

```bash
python3 decode_eg53ma4.py
```

See [`python/README.md`](python/README.md) for the row table and the notes.

## Tests without the board

```bash
./scripts/test_host.sh
```

This runs:

- a host C++ build of `src/decode_eg53ma4.cpp` against `test/test_decode.cpp`;
- the Python unit tests in `python/test_decode.py`.

Covered without hardware:

- clean 120-bit frame, ID, pressure, temperature, checksum;
- 121-bit alignment artifact and normalization;
- 119-bit conditional trailing-zero repair;
- rejection of truncated rows and a checksum mismatch;
- a row that is not in `data/rows.txt`, proving the decoder is not hardcoded to the examples.

Not covered without hardware:

- CC1101 SPI bring-up, register state, and RSSI;
- GDO0 interrupt timing and the slicer under real noise;
- RF range, antenna, and in-car behavior.

The firmware compiles for the ESP32-S3, but the RF path still needs a board and a sensor for full validation.

## Limits

- The header whitelist (`0x4D920052`, `0x4D930052`, `0x4C960052`) is fitted to the sensors used in the article. Edit `kHeaders` in `src/decode_eg53ma4.cpp` for another batch.
- The payload boundary assumes the known header family starts with `01`: the zero run is 41 bits and the payload starts one bit before its end. A different header family needs a different boundary rule.
- The 119-bit repair is conditional: it recovers only rows whose missing bit is `0`.
- The example does not include the product's mapping, cache, or alert logic.
- The radio configuration is the one from the article. It is not a general-purpose CC1101 driver.

## License and credits

The frame model comes from the EG53MA4 decoder in [`rtl_433`](https://github.com/merbanan/rtl_433), contributed by **Leonardo Hamada** (`hkazu`). That project is GPL-2.0; this reference implementation is distributed under **GPL-2.0-or-later**. See `LICENSE`.

Article: link to be added when the Medium post is published.
