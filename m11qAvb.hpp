// Device tool policy and bounded top-level AVB flag edits.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace m11q {
inline bool BootloaderUnlocked(const std::string& state, const std::string& verified,
                               const std::string& flash_locked) {
  // Samsung may omit device_state/flash.locked on subsequent recovery boots.
  // AOSP defines orange as UNLOCKED; reject any explicit conflicting value.
  return verified == "orange" && (state.empty() || state == "unlocked") &&
         (flash_locked.empty() || flash_locked == "0");
}
inline uint64_t AvbBe(const std::vector<uint8_t>& b, size_t at, size_t n) {
  uint64_t value = 0;
  for (size_t i = 0; i < n; ++i) value = (value << 8) | b[at+i];
  return value;
}
inline bool AvbFlags(const std::vector<uint8_t>& b, uint32_t& flags) {
  if (b.size() < 256 || memcmp(b.data(), "AVB0", 4) ||
      AvbBe(b,4,4) != 1 || AvbBe(b,8,4) > 2 || AvbBe(b,124,4) != 0)
    return false;
  const uint64_t auth = AvbBe(b,12,8), aux = AvbBe(b,20,8);
  if (auth % 64 || aux % 64 || auth > b.size()-256 || aux > b.size()-256-auth)
    return false;
  const uint64_t algorithm = AvbBe(b,28,4);
  if (algorithm > 6 || (algorithm && !auth)) return false;
  auto range = [&](size_t offset, size_t length, uint64_t limit) {
    const uint64_t start = AvbBe(b,offset,8), size = AvbBe(b,length,8);
    return start <= limit && size <= limit-start;
  };
  if (!range(32,40,auth) || !range(48,56,auth) || !range(64,72,aux) ||
      !range(80,88,aux) || !range(96,104,aux)) return false;
  uint64_t pos = AvbBe(b,96,8), end = pos+AvbBe(b,104,8);
  while (pos < end) {
    if (end-pos < 16) return false;
    uint64_t size = AvbBe(b,256+auth+pos+8,8);
    if (size % 8 || size > end-pos-16) return false;
    pos += 16+size;
  }
  flags = static_cast<uint32_t>(AvbBe(b,120,4));
  return !(flags & ~uint32_t(3));
}
inline bool PrepareAvb(const std::vector<uint8_t>& original, std::vector<uint8_t>& changed) {
  uint32_t flags;
  if (!AvbFlags(original,flags)) return false;
  changed = original;
  changed[123] = static_cast<uint8_t>(flags | 3);
  return true;
}
inline bool AvbHashtreeDisabled(uint32_t flags) { return (flags & 3) != 0; }
} // namespace m11q
