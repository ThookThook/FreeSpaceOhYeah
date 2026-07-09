#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
class Sha256 {
 public:
  Sha256();
  void update(const uint8_t* data, size_t len);
  void update(const std::vector<uint8_t>& v) { update(v.data(), v.size()); }
  std::array<uint8_t,32> final();
  static std::string hex(const std::array<uint8_t,32>& digest);
 private:
  void transform(const uint8_t block[64]);
  uint64_t bit_len_ = 0;
  std::array<uint8_t,64> buffer_{};
  size_t buffer_len_ = 0;
  std::array<uint32_t,8> state_{};
};
