// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace m11q {
inline uint32_t Le32(const std::vector<uint8_t> &b, size_t o) {
  return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 |
         uint32_t(b[o + 3]) << 24;
}
inline bool ValidBoot(const std::vector<uint8_t> &b, std::string &error) {
  if (b.size() < 2048 || memcmp(b.data(), "ANDROID!", 8)) {
    error = "Not an Android boot image";
    return false;
  }
  uint32_t version = Le32(b, 40), page = Le32(b, 36);
  if (version > 2 || page < 2048 || page > 65536 || (page & (page - 1))) {
    error = "Only legacy m11q boot headers v0-v2 are supported";
    return false;
  }
  auto aligned = [page](uint64_t n) { return ((n + page - 1) / page) * page; };
  uint64_t kernel = Le32(b, 8), ramdisk = Le32(b, 16), second = Le32(b, 24);
  if (!kernel ||
      uint64_t(page) + aligned(kernel) + aligned(ramdisk) + aligned(second) >
          b.size()) {
    error = "Boot components exceed image size";
    return false;
  }
  if (version >= 1 && Le32(b, 1644) != (version == 1 ? 1648u : 1660u)) {
    error = "Unexpected boot header size";
    return false;
  }
  if (!memchr(b.data() + 64, 0, 512) || !memchr(b.data() + 608, 0, 1024)) {
    error = "Unterminated boot command line";
    return false;
  }
  return true;
}
inline std::string Cmdline(const std::vector<uint8_t> &b) {
  return std::string(reinterpret_cast<const char *>(b.data() + 64)) +
         std::string(reinterpret_cast<const char *>(b.data() + 608));
}
inline std::string Trim(const std::string &s) {
  size_t first = s.find_first_not_of(" \t\r\n"),
         last = s.find_last_not_of(" \t\r\n");
  return first == std::string::npos ? "" : s.substr(first, last - first + 1);
}
// Keep non-SELinux bytes, including quoted values and whitespace, intact.
inline std::string WithoutMode(const std::string &s,
                               std::string *modes = nullptr) {
  std::string result;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == ' ' || s[i] == '\t') {
      result += s[i++];
      continue;
    }
    size_t start = i;
    char quote = 0;
    for (; i < s.size(); ++i) {
      char c = s[i];
      if ((c == '\'' || c == '"') && (!quote || quote == c))
        quote = quote ? 0 : c;
      if (!quote && (c == ' ' || c == '\t'))
        break;
    }
    std::string token = s.substr(start, i - start);
    if (token.compare(0, 20, "androidboot.selinux=") == 0) {
      if (modes) {
        if (!modes->empty())
          *modes += ' ';
        *modes += token;
      }
    } else
      result += token;
  }
  return Trim(result);
}
inline bool SetCmdline(std::vector<uint8_t> &b, const std::string &s,
                       std::string &error) {
  // Each field is NUL-terminated. Keep the first field at most 511 bytes.
  if (s.size() > 1534 || s.find('\0') != std::string::npos ||
      s.find('\n') != std::string::npos) {
    error = "Command line too long or invalid";
    return false;
  }
  memset(b.data() + 64, 0, 512);
  memset(b.data() + 608, 0, 1024);
  size_t first = std::min<size_t>(511, s.size());
  memcpy(b.data() + 64, s.data(), first);
  if (s.size() > first)
    memcpy(b.data() + 608, s.data() + first, s.size() - first);
  return true;
}
inline std::string RequestedMode(const std::string &current,
                                 const std::string &mode) {
  std::string s = WithoutMode(current);
  if (!s.empty())
    s += ' ';
  return s + "androidboot.selinux=" + mode;
}
inline std::string RestoreMode(const std::string &current,
                               const std::string &original) {
  if (WithoutMode(current) == WithoutMode(original))
    return original;
  std::string modes, result = WithoutMode(current);
  WithoutMode(original, &modes);
  if (!modes.empty()) {
    if (!result.empty())
      result += ' ';
    result += modes;
  }
  return result;
}
} // namespace m11q
