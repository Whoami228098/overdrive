// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace torrent::stream {
std::string key(const std::string& uri, std::string& error);
// A false result with an empty error means file priorities are still pending.
bool select(int id, const std::string& path, std::string& error);
bool read(int id, std::uint64_t offset, std::size_t limit, std::vector<char>& bytes, std::string& error);
// Nonblocking verified prefix read for the controller; empty error means pending.
bool peek(int id, std::uint64_t offset, std::size_t limit, std::vector<char>& bytes, std::string& error);
void suspend(int id);
}
