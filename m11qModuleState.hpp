// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>
namespace m11q {
struct ModuleState {
  std::string id, propHash;
  bool disabled;
};
inline bool SafeId(const std::string &s) {
  if (s.empty() || s.size() > 100 || s == "." || s == "..")
    return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
  });
}
inline bool ParseModuleState(const std::string &input,
                             std::vector<ModuleState> &output) {
  output.clear();
  if (input.size() > 1024 * 1024)
    return false;
  std::istringstream lines(input);
  std::string marker, row;
  std::getline(lines, marker);
  if (marker != "quokka-modules-v1")
    return false;
  std::vector<ModuleState> parsed;
  std::set<std::string> ids;
  while (std::getline(lines, row)) {
    std::istringstream fields(row);
    std::string id, hash, extra;
    int disabled;
    if (!(fields >> id >> disabled >> hash) || (fields >> extra) ||
        (disabled != 0 && disabled != 1) || !SafeId(id) ||
        !ids.insert(id).second || hash.size() != 64 ||
        !std::all_of(hash.begin(), hash.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
      return false;
    parsed.push_back({id, hash, bool(disabled)});
    if (parsed.size() > 2048)
      return false;
  }
  if (parsed.empty())
    return false;
  output = std::move(parsed);
  return true;
}
} // namespace m11q
