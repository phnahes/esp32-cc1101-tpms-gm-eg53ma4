// Minimal E07-M1101D (CC1101-based) receiver for the Schrader EG53MA4
// 433.92 MHz TPMS frame. Serial output only. This is a reference example
// for the article, not the product firmware.
//
// Wiring on the ESP32-S3:
//   E07 VCC   -> 3V3
//   E07 GND   -> GND
//   E07 SCK   -> GPIO40
//   E07 MISO  -> GPIO41
//   E07 MOSI  -> GPIO42
//   E07 CSN   -> GPIO47
//   E07 GDO0  -> GPIO8
//   E07 GDO2  -> GPIO15 (wired; not used here)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <Arduino.h>
#include <SPI.h>
#include <stdlib.h>
#include <string.h>

#include "decode_eg53ma4.h"

namespace {

constexpr uint8_t PIN_SCK = 40;
constexpr uint8_t PIN_MISO = 41;
constexpr uint8_t PIN_MOSI = 42;
constexpr uint8_t PIN_CSN = 47;
constexpr uint8_t PIN_GDO0 = 8;
// PIN_GDO2 = 15 is wired on the board but not used by this example.

struct RegValue {
  uint8_t addr;
  uint8_t value;
};

const RegValue kRadioConfig[] = {
    {0x00, 0x0E},  // IOCFG2: carrier sense (not used)
    {0x02, 0x0D},  // IOCFG0: GDO0 = serial data
    {0x03, 0x0F},  // FIFOTHR
    {0x06, 0x00},  // PKTLEN
    {0x07, 0x04},  // PKTCTRL1
    {0x08, 0x32},  // PKTCTRL0: async serial
    {0x09, 0x00},  // ADDR
    {0x0B, 0x06},  // FSCTRL1
    {0x0C, 0x00},  // FSCTRL0
    {0x0D, 0x10},  // FREQ2
    {0x0E, 0xB0},  // FREQ1
    {0x0F, 0x71},  // FREQ0 -> 433.9198 MHz
    {0x10, 0x07},  // MDMCFG4: RX filter 812.5 kHz, DRATE_E=7
    {0x11, 0x93},  // MDMCFG3: ~5 kbps setting
    {0x12, 0x32},  // MDMCFG2: ASK/OOK
    {0x13, 0x02},  // MDMCFG1
    {0x14, 0xF8},  // MDMCFG0
    {0x15, 0x47},  // DEVIATN
    {0x16, 0x07},  // MCSM2
    {0x17, 0x3C},  // MCSM1: stay in RX
    {0x18, 0x18},  // MCSM0
    {0x19, 0x16},  // FOCCFG
    {0x1A, 0x1C},  // BSCFG
    {0x1B, 0xC7},  // AGCCTRL2
    {0x1C, 0x00},  // AGCCTRL1
    {0x1D, 0xB2},  // AGCCTRL0
    {0x21, 0x56},  // FREND1
    {0x22, 0x11},  // FREND0: ASK
    {0x23, 0xE9},  // FSCAL3
    {0x24, 0x2A},  // FSCAL2
    {0x25, 0x00},  // FSCAL1
    {0x26, 0x1F},  // FSCAL0
    {0x29, 0x59},  // FSTEST
    {0x2A, 0x7F},  // PTEST
    {0x2B, 0x3F},  // AGCTEST
    {0x2C, 0x81},  // TEST2
    {0x2D, 0x35},  // TEST1
    {0x2E, 0x09},  // TEST0
};

struct Edge {
  uint32_t us;
  uint8_t level;
};

constexpr int kRingSize = 2048;
volatile Edge g_ring[kRingSize];
volatile uint16_t g_head = 0;
volatile uint16_t g_tail = 0;
volatile uint32_t g_overflow = 0;
portMUX_TYPE g_ringMux = portMUX_INITIALIZER_UNLOCKED;

struct Slicer {
  uint8_t out[160];
  int outCount = 0;
  uint32_t timeSinceLast = 0;
};

Slicer g_slicer;

bool g_havePrev = false;
uint32_t g_prevUs = 0;
uint8_t g_prevLvl = 0;
bool g_haveSeg = false;
uint8_t g_segLvl = 0;
uint32_t g_segDur = 0;
bool g_havePulse = false;
uint32_t g_pulseDur = 0;

void printFrame(const eg53ma4::Frame& frame) {
  const float psi = frame.pressure_kpa / 6.89476f;
  const float bar = frame.pressure_kpa / 100.0f;
  const float celsius = (frame.temperature_f - 32.0f) * 5.0f / 9.0f;
  Serial.printf(
      "[frame] header=0x%08lX id=%06lX pressure=%.1f kPa "
      "(%.1f PSI / %.2f bar) temp=%d F (%.1f C) checksum=OK%s\n",
      static_cast<unsigned long>(frame.header),
      static_cast<unsigned long>(frame.sensor_id),
      frame.pressure_kpa, psi, bar, frame.temperature_f, celsius,
      frame.recovered_119 ? " (119-bit, trailing zero)" : "");
}

void onBitRow(const uint8_t* bits, int len) {
  eg53ma4::Frame frame;
  char error[128];
  if (!eg53ma4::decodeBits(bits, len, frame, error, sizeof(error))) {
    Serial.printf("[reject] bits=%d %s\n", len, error);
    return;
  }
  printFrame(frame);
}

void addBit(uint8_t bit) {
  if (g_slicer.outCount < static_cast<int>(sizeof(g_slicer.out))) {
    g_slicer.out[g_slicer.outCount++] = bit;
  }
}

void resetRow() {
  g_slicer.outCount = 0;
  addBit(0);
  g_slicer.timeSinceLast = 0;
}

void deliverRow() {
  if (g_slicer.outCount >= 64) {
    onBitRow(g_slicer.out, g_slicer.outCount);
  }
}

void processPair(uint32_t pulse, uint32_t gap) {
  constexpr uint32_t kShortUs = 123;
  constexpr uint32_t kToleranceUs = 35;
  constexpr uint32_t kMinUs = kShortUs - kToleranceUs;
  constexpr uint32_t kMaxUs = kShortUs * 2 + kToleranceUs;
  constexpr uint32_t kDecisionUs = static_cast<uint32_t>(kShortUs * 1.5f);

  if (pulse < kMinUs || pulse > kMaxUs) {
    deliverRow();
    resetRow();
    return;
  }
  if (pulse + g_slicer.timeSinceLast > kDecisionUs) {
    addBit(1);
    g_slicer.timeSinceLast = 0;
  } else {
    g_slicer.timeSinceLast += pulse;
  }

  if (gap < kMinUs) {
    deliverRow();
    resetRow();
    return;
  }
  if (gap > kMaxUs) {
    addBit(0);
    deliverRow();
    resetRow();
    return;
  }
  if (gap + g_slicer.timeSinceLast > kDecisionUs) {
    addBit(0);
    g_slicer.timeSinceLast = 0;
  } else {
    g_slicer.timeSinceLast += gap;
  }
}

void emitSegment(uint8_t level, uint32_t duration) {
  if (level != 0) {
    g_pulseDur = duration;
    g_havePulse = true;
  } else if (g_havePulse) {
    processPair(g_pulseDur, duration);
    g_havePulse = false;
  }
}

void addSegment(uint8_t level, uint32_t duration) {
  if (duration == 0) return;
  if (!g_haveSeg) {
    g_segLvl = level;
    g_segDur = duration;
    g_haveSeg = true;
    return;
  }
  if (level == g_segLvl) {
    g_segDur += duration;
    return;
  }
  if (duration < 20) {
    g_segDur += duration;  // merge glitches into the current segment
    return;
  }
  if (g_segDur < 20) {
    g_segLvl = level;
    g_segDur += duration;
    return;
  }
  emitSegment(g_segLvl, g_segDur);
  g_segLvl = level;
  g_segDur = duration;
}

void processEdge(uint32_t us, uint8_t level) {
  if (!g_havePrev) {
    g_prevUs = us;
    g_prevLvl = level;
    g_havePrev = true;
    return;
  }
  const uint32_t duration = us - g_prevUs;
  addSegment(g_prevLvl, duration);
  g_prevUs = us;
  g_prevLvl = level;
}

void streamIdle(uint32_t now) {
  if (!g_havePrev) return;
  const uint32_t duration = now - g_prevUs;
  addSegment(g_prevLvl, duration);
  g_prevUs = now;
  if (g_haveSeg) {
    emitSegment(g_segLvl, g_segDur);
    g_haveSeg = false;
  }
  if (g_havePulse) {
    processPair(g_pulseDur, 300);
    g_havePulse = false;
  }
  deliverRow();
  resetRow();
  g_havePrev = false;
}

void IRAM_ATTR onEdge() {
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  const uint8_t level = static_cast<uint8_t>(gpio_get_level(
      static_cast<gpio_num_t>(PIN_GDO0)));
  portENTER_CRITICAL_ISR(&g_ringMux);
  const uint16_t next = static_cast<uint16_t>((g_head + 1) & (kRingSize - 1));
  if (next != g_tail) {
    g_ring[g_head].us = now;
    g_ring[g_head].level = level;
    g_head = next;
  } else {
    ++g_overflow;
  }
  portEXIT_CRITICAL_ISR(&g_ringMux);
}

// ------------------------- CC1101 SPI -------------------------

void ccWrite(uint8_t addr, uint8_t value) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(addr);
  SPI.transfer(value);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
}

uint8_t ccReadStatus(uint8_t addr) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(static_cast<uint8_t>(addr | 0xC0));
  const uint8_t value = SPI.transfer(0x00);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
  return value;
}

void ccStrobe(uint8_t command) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(command);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
}

void ccReset() {
  pinMode(PIN_CSN, OUTPUT);
  digitalWrite(PIN_CSN, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_CSN, LOW);
  delayMicroseconds(10);
  digitalWrite(PIN_CSN, HIGH);
  delayMicroseconds(41);
  digitalWrite(PIN_CSN, LOW);
  delayMicroseconds(10);
  unsigned long start = micros();
  while (digitalRead(PIN_MISO) && (micros() - start < 2000)) {
  }
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  SPI.transfer(0x30);  // SRES
  start = micros();
  while (digitalRead(PIN_MISO) && (micros() - start < 2000)) {
  }
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
  delay(2);
}

bool ccBegin() {
  ccReset();
  for (const RegValue& reg : kRadioConfig) {
    ccWrite(reg.addr, reg.value);
  }
  ccStrobe(0x36);  // SIDLE
  ccStrobe(0x3A);  // SFRX
  ccStrobe(0x3B);  // SFTX
  ccStrobe(0x34);  // SRX
  delay(2);

  const uint8_t partnum = ccReadStatus(0x30);
  const uint8_t version = ccReadStatus(0x31);
  const uint8_t marc = static_cast<uint8_t>(ccReadStatus(0x35) & 0x1F);
  Serial.printf("[CC1101] PARTNUM=0x%02X VERSION=0x%02X MARC=0x%02X\n",
                partnum, version, marc);
  if (partnum != 0x00 || version != 0x14 || marc != 0x0D) {
    Serial.println(
        "[CC1101] unexpected ID/state; expected PARTNUM=0x00 VERSION=0x14 "
        "MARC=0x0D");
  }
  return version != 0x00 && version != 0xFF;
}

// ------------------------- Serial console -------------------------

void printResult(const char* row, int bits) {
  eg53ma4::Frame frame;
  char error[128];
  Serial.printf("[row] row=%s bits=%d\n", row, bits);
  if (!eg53ma4::decodeRow(row, bits, frame, error, sizeof(error))) {
    Serial.printf("[reject] %s\n", error);
    return;
  }
  printFrame(frame);
}

void handleSerialLine(char* line) {
  char* space = strrchr(line, ' ');
  if (space == nullptr) {
    Serial.println("[console] expected: <row> <bits>");
    return;
  }
  *space = '\0';
  const int bits = atoi(space + 1);
  if (bits <= 0) {
    Serial.println("[console] invalid bit count");
    return;
  }
  printResult(line, bits);
}

void selfTest() {
  struct Case {
    const char* row;
    int bits;
    bool expected;
  };
  const Case cases[] = {
      {"00000000004d9200520066e91a5af4", 120, true},
      {"7FFFFFFFFFCD930052006AC655550C0", 121, true},
      {"00000000004d9200520066e91a5af4", 119, true},
      {"00000000009B2400A400", 79, false},
      {"0000009B2600A400CD4CC2ABE8", 104, false},
  };
  int passed = 0;
  int failed = 0;
  for (const Case& c : cases) {
    eg53ma4::Frame frame;
    char error[128];
    const bool ok = eg53ma4::decodeRow(c.row, c.bits, frame, error, sizeof(error));
    if (ok == c.expected) {
      ++passed;
    } else {
      ++failed;
      Serial.printf("[selftest] FAIL row=%s bits=%d got=%d expected=%d\n",
                    c.row, c.bits, ok ? 1 : 0, c.expected ? 1 : 0);
    }
  }
  Serial.printf("[selftest] pass=%d fail=%d\n", passed, failed);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nE07-M1101D / EG53MA4 reference receiver");
  Serial.println("Serial output only; not the product firmware.");

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  SPI.setFrequency(1000000);
  SPI.setDataMode(SPI_MODE0);
  pinMode(PIN_CSN, OUTPUT);
  digitalWrite(PIN_CSN, HIGH);

  if (!ccBegin()) {
    Serial.println("[CC1101] init failed; check wiring and 3.3 V");
  }

  pinMode(PIN_GDO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_GDO0), onEdge, CHANGE);
  selfTest();
  Serial.println("[console] send: <row> <bits>");
}

void loop() {
  static Edge batch[128];
  int count = 0;

  portENTER_CRITICAL(&g_ringMux);
  while (g_tail != g_head && count < 128) {
    batch[count].us = g_ring[g_tail].us;
    batch[count].level = g_ring[g_tail].level;
    ++count;
    g_tail = static_cast<uint16_t>((g_tail + 1) & (kRingSize - 1));
  }
  portEXIT_CRITICAL(&g_ringMux);

  if (count > 0) {
    for (int i = 0; i < count; ++i) {
      processEdge(batch[i].us, batch[i].level);
    }
  } else if (g_havePrev) {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    if (static_cast<uint32_t>(now - g_prevUs) > 3000) {
      streamIdle(now);
    }
  }

  static char line[192];
  static size_t lineLen = 0;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        line[lineLen] = '\0';
        handleSerialLine(line);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }

  delay(1);
}
