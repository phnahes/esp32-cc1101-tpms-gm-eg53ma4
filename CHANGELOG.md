# Changelog

## v0.1.0 - 2026-11-11

- Initial public release.
- ESP32-S3 receiver for the Ebyte E07-M1101D (CC1101-based) and the Schrader EG53MA4 433.92 MHz TPMS frame.
- GDO0 capture, Manchester slicer, frame decoder, serial console, and boot self-test.
- Python reference decoder and test harness.
- Host C++ decoder tests.
- Test rows for the clean 120-bit frame, the 121-bit alignment artifact, the 119-bit conditional repair, and truncated rows.
