#pragma once

#include <stddef.h>
#include <stdint.h>

namespace eg53ma4 {

struct Frame {
  uint32_t header;
  uint32_t sensor_id;
  float pressure_kpa;
  int temperature_f;
  bool recovered_119;
};

// Decode a row= hex string with the bit count from the serial log.
// Returns true on success; on failure, fills `error`.
bool decodeRow(const char* row, int bitCount, Frame& out,
               char* error, size_t errorSize);

// Decode a bit row captured directly from the receiver.
bool decodeBits(const uint8_t* bits, int len, Frame& out,
                char* error, size_t errorSize);

}  // namespace eg53ma4
