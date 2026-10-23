// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <cstddef>
#include <cstdint>

// PS4 LP64 ABI verified against Itemzflow/include/installpkg.h and flatz's bgft.h.
// OpenOrbis's older _types/bgft.h incorrectly narrows the four progress counters to uint32_t.
namespace ps4_bgft {
struct Init { void* heap; std::size_t heap_size; };
struct Download {
  std::int32_t user_id, entitlement_type;
  const char *id, *url, *ex_url, *name, *icon, *sku;
  std::uint32_t options;
  const char *scenario, *release_date, *type, *subtype;
  std::uint64_t size;
};
struct DownloadEx { Download param; std::uint32_t slot; };
struct Progress {
  std::uint32_t bits;
  std::int32_t error;
  std::uint64_t length, transferred, length_total, transferred_total;
  std::uint32_t index, count, seconds, seconds_total;
  std::int32_t preparing_percent, copy_percent;
};
static_assert(sizeof(Download) == 0x68 && offsetof(Download, size) == 0x60);
static_assert(sizeof(DownloadEx) == 0x70);
static_assert(sizeof(Progress) == 0x40 && offsetof(Progress, transferred_total) == 0x20);
inline constexpr std::uint32_t disable_cdn_query = 0x10000;
}
extern "C" {
int sceBgftServiceInit(ps4_bgft::Init*);
int sceBgftServiceTerm();
int sceBgftServiceIntDebugDownloadRegisterPkg(ps4_bgft::DownloadEx*, int*);
int sceBgftServiceIntDownloadRegisterTaskByStorageEx(ps4_bgft::DownloadEx*,int*);
int sceBgftServiceDownloadStartTask(int);
int sceBgftServiceDownloadGetProgress(int, ps4_bgft::Progress*);
int sceBgftServiceIntDownloadUnregisterTask(int);
}
