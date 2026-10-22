// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Apply after platform defaults: upstream defines MADVISE unconditionally on BSD/Linux.
#pragma once
#include <libtorrent/config.hpp>
#undef TORRENT_USE_MADVISE
#define TORRENT_USE_MADVISE 0
#undef TORRENT_HAS_FADVISE
#define TORRENT_HAS_FADVISE 0
