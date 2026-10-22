// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include "packages.hpp"
namespace packages {
// Read-only query, including a BGFT job id saved by the JS queue during an earlier launch.
bool status(std::uint32_t job_id, Snapshot& out, std::string& error);
void detach(std::uint32_t bgft_id);
}
