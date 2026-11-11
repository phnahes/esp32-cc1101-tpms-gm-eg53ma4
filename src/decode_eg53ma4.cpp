#include "decode_eg53ma4.h"

#include <stdio.h>
#include <string.h>

namespace eg53ma4 {
namespace {

constexpr int kMinSyncZeros = 24;
constexpr int kFrameBytes = 10;
constexpr int kPayloadBits = 80;
constexpr int kMaxBits = 160;
constexpr uint32_t kHeaders[] = {0x4D920052u, 0x4D930052u, 0x4C960052u};

bool headerAllowed(uint32_t header) {
  for (uint32_t value : kHeaders) {
    if (header == value) return true;
  }
  return false;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int rowToBits(const char* row, int bitCount, uint8_t* bits,
              char* error, size_t errorSize) {
  const char* p = row;
  if (strncmp(p, "row=", 4) == 0) p += 4;

  int parsed = 0;
  while (*p != '\0' && parsed < kMaxBits) {
    const char c = *p++;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') continue;
    const int nibble = hexNibble(c);
    if (nibble < 0) {
      snprintf(error, errorSize, "invalid hex character: %c", c);
      return -1;
    }
    for (int shift = 3; shift >= 0; --shift) {
      if (parsed < kMaxBits) {
        bits[parsed++] = static_cast<uint8_t>((nibble >> shift) & 1);
      }
    }
  }

  if (parsed == 0) {
    snprintf(error, errorSize, "empty row");
    return -1;
  }
  if (bitCount <= 0) return parsed;
  if (bitCount > parsed) {
    snprintf(error, errorSize, "bit count %d exceeds row length %d",
             bitCount, parsed);
    return -1;
  }
  return bitCount;
}

void longestZeroRun(const uint8_t* bits, int len, int& bestEnd, int& bestLen) {
  bestEnd = 0;
  bestLen = 0;
  int i = 0;
  while (i < len) {
    if (bits[i] != 0) {
      ++i;
      continue;
    }
    int j = i;
    while (j < len && bits[j] == 0) ++j;
    if (j - i > bestLen) {
      bestLen = j - i;
      bestEnd = j;
    }
    i = j;
  }
}

uint8_t extractByte(const uint8_t* bits, int pos) {
  uint8_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = static_cast<uint8_t>((value << 1) | (bits[pos + i] & 1));
  }
  return value;
}

bool decodeAt(const uint8_t* bits, int len, int start, bool allow119,
              Frame& out, char* error, size_t errorSize) {
  int available = len - start;
  bool recovered = false;
  uint8_t temp[kMaxBits];
  const uint8_t* data = bits;
  int dataLen = len;

  if (available < kPayloadBits) {
    if (allow119 && available == kPayloadBits - 1) {
      memcpy(temp, bits, static_cast<size_t>(len));
      temp[len] = 0;
      data = temp;
      dataLen = len + 1;
      available = kPayloadBits;
      recovered = true;
    } else {
      snprintf(error, errorSize,
               "payload incomplete: not enough bits after the sync (need 80)");
      return false;
    }
  }
  (void)dataLen;

  uint8_t b[kFrameBytes];
  for (int i = 0; i < kFrameBytes; ++i) {
    b[i] = extractByte(data, start + i * 8);
  }

  uint8_t sum = 0;
  for (int i = 0; i < 9; ++i) sum = static_cast<uint8_t>(sum + b[i]);
  if (sum != b[9]) {
    snprintf(error, errorSize, "checksum mismatch: got 0x%02X, expected 0x%02X",
             b[9], sum);
    return false;
  }

  const uint32_t header = (static_cast<uint32_t>(b[0]) << 24) |
                          (static_cast<uint32_t>(b[1]) << 16) |
                          (static_cast<uint32_t>(b[2]) << 8) | b[3];
  if (!headerAllowed(header)) {
    snprintf(error, errorSize, "header 0x%08lX not in whitelist",
             static_cast<unsigned long>(header));
    return false;
  }

  out.header = header;
  out.sensor_id = (static_cast<uint32_t>(b[4]) << 16) |
                  (static_cast<uint32_t>(b[5]) << 8) | b[6];
  out.pressure_kpa = static_cast<float>(b[7]) * 2.5f;
  out.temperature_f = b[8];
  out.recovered_119 = recovered;
  return true;
}

bool decodeBitsInternal(const uint8_t* bits, int len, bool allow119,
                        Frame& out, char* error, size_t errorSize) {
  int end = 0;
  int length = 0;
  longestZeroRun(bits, len, end, length);
  if (length < kMinSyncZeros) {
    snprintf(error, errorSize,
             "no sync: longest zero run is %d bits; need at least 24", length);
    return false;
  }

  int candidates[2];
  int count = 0;
  candidates[count++] = end;
  if (end > kMinSyncZeros) candidates[count++] = end - 1;

  char firstError[128] = {0};
  for (int i = 0; i < count; ++i) {
    if (decodeAt(bits, len, candidates[i], allow119, out, error, errorSize)) {
      return true;
    }
    if (firstError[0] == '\0') {
      strncpy(firstError, error, sizeof(firstError) - 1);
    }
  }
  strncpy(error, firstError, errorSize - 1);
  error[errorSize - 1] = '\0';
  return false;
}

bool normalizeInverted(const uint8_t* bits, int len, uint8_t* out, int& outLen) {
  if (len < 104 || bits[0] != 0) return false;
  int ones = 0;
  for (int i = 1; i < len && bits[i] != 0; ++i) ++ones;
  if (ones < 24) return false;

  memcpy(out, bits, static_cast<size_t>(len));
  for (int i = 1; i < 40 && i < len; ++i) out[i] = 0;
  if (len > 40) out[40] = 0;
  outLen = len;
  return true;
}

}  // namespace

bool decodeRow(const char* row, int bitCount, Frame& out,
               char* error, size_t errorSize) {
  uint8_t bits[kMaxBits];
  const int len = rowToBits(row, bitCount, bits, error, errorSize);
  if (len < 0) return false;

  if (decodeBitsInternal(bits, len, true, out, error, errorSize)) return true;

  uint8_t normalized[kMaxBits];
  int normalizedLen = 0;
  if (normalizeInverted(bits, len, normalized, normalizedLen) &&
      decodeBitsInternal(normalized, normalizedLen, true, out, error, errorSize)) {
    return true;
  }
  return false;
}

bool decodeBits(const uint8_t* bits, int len, Frame& out,
                char* error, size_t errorSize) {
  return decodeBitsInternal(bits, len, true, out, error, errorSize);
}

}  // namespace eg53ma4
