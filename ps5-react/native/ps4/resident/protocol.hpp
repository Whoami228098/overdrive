// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include "http_range.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
namespace resident_protocol {
inline std::string hex(const std::string& value) {
  std::string out; out.reserve(value.size() * 2);
  for (unsigned char c : value) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
  return out;
}
inline std::string unhex(const std::string& value, std::size_t max) {
  if (value.size() % 2 || value.size() > max * 2) throw std::runtime_error("Invalid bounded hex string");
  auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  std::string out; out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int a = digit(value[i]), b = digit(value[i + 1]);
    if (a < 0 || b < 0 || (a == 0 && b == 0)) throw std::runtime_error("Invalid hex text byte");
    out += static_cast<char>((a << 4) | b);
  }
  return out;
}
inline std::string json(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 32) { out += "\\u00"; out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    else out += c;
  }
  return out + '"';
}
inline std::uint64_t number(const std::string& value, std::uint64_t max = UINT64_MAX) {
  std::uint64_t out;
  if (!overdrive_decimal(value.data(), value.size(), &out) || out > max) throw std::runtime_error("Invalid bounded decimal number");
  return out;
}
inline std::vector<std::string> lines(const std::string& value) {
  std::vector<std::string> out;
  for (std::size_t start = 0; start < value.size();) {
    const auto end = value.find('\n', start);
    if (end == value.npos) throw std::runtime_error("Unterminated protocol record");
    out.push_back(value.substr(start, end - start)); start = end + 1;
    if (out.size() > 32) throw std::runtime_error("Protocol record has too many fields");
  }
  return out;
}
inline std::string lower(std::string value) {
  for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
}
