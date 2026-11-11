#include <cmath>
#include <cstdio>

#include "decode_eg53ma4.h"

namespace {

struct Case {
  const char* row;
  int bits;
  bool expected;
  uint32_t id;
  float kpa;
  int temp_f;
};

}  // namespace

int main() {
  const Case cases[] = {
      {"00000000004d9200520066e91a5af4", 120, true, 0x0066E9, 65.0f, 90},
      {"7FFFFFFFFFCD930052006AC655550C0", 121, true, 0x006AC6, 212.5f, 85},
      {"00000000004d9200520066e91a5af4", 119, true, 0x0066E9, 65.0f, 90},
      {"00000000009B2400A400", 79, false, 0, 0.0f, 0},
      {"00000000004D9300520066E948", 101, false, 0, 0.0f, 0},
      {"00000000009B2600A400", 78, false, 0, 0.0f, 0},
      {"0000009B2600A400CD4CC2ABE8", 104, false, 0, 0.0f, 0},
      {"7FFF", 16, false, 0, 0.0f, 0},
      {"7FFFFFFFFF", 40, false, 0, 0.0f, 0},
  };

  int passed = 0;
  int failed = 0;
  for (const Case& c : cases) {
    eg53ma4::Frame frame;
    char error[128] = {0};
    const bool ok = eg53ma4::decodeRow(c.row, c.bits, frame, error, sizeof(error));
    bool good = (ok == c.expected);
    if (good && c.expected) {
      good = frame.sensor_id == c.id &&
             std::fabs(frame.pressure_kpa - c.kpa) < 0.01f &&
             frame.temperature_f == c.temp_f;
    }
    if (good) {
      ++passed;
      std::printf("PASS row=%s bits=%d\n", c.row, c.bits);
    } else {
      ++failed;
      std::printf("FAIL row=%s bits=%d expected=%d got=%d error=%s\n",
                  c.row, c.bits, c.expected ? 1 : 0, ok ? 1 : 0, error);
    }
  }
  std::printf("pass=%d fail=%d\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
