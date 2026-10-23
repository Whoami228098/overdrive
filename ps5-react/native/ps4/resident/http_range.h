// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#ifndef OVERDRIVE_HTTP_RANGE_H
#define OVERDRIVE_HTTP_RANGE_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint64_t first, last; int partial; } OverdriveRange;
static int overdrive_decimal(const char* text, size_t length, uint64_t* out) {
  uint64_t value = 0;
  if (!length) return 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return 0;
    unsigned digit = (unsigned)(text[i] - '0');
    if (value > (UINT64_MAX - digit) / 10) return 0;
    value = value * 10 + digit;
  }
  *out = value; return 1;
}
// Returns the HTTP status. Multiple ranges, overflow and unsatisfiable ranges
// are rejected; a closed range ending beyond EOF is clamped per HTTP semantics.
static int overdrive_range(const char* value, size_t length, uint64_t total, OverdriveRange* out) {
  if (!total) return 416;
  out->first = 0; out->last = total - 1; out->partial = 0;
  if (!length) return 200;
  if (length < 7 || value[0] != 'b' || value[1] != 'y' || value[2] != 't' || value[3] != 'e' || value[4] != 's' || value[5] != '=') return 416;
  size_t dash = 6;
  while (dash < length && value[dash] != '-') ++dash;
  if (dash == length) return 416;
  uint64_t first, last;
  if (dash == 6) {
    uint64_t suffix;
    if (!overdrive_decimal(value + 7, length - 7, &suffix) || !suffix) return 416;
    first = suffix >= total ? 0 : total - suffix; last = total - 1;
  } else {
    if (!overdrive_decimal(value + 6, dash - 6, &first) || first >= total) return 416;
    if (dash + 1 == length) last = total - 1;
    else if (!overdrive_decimal(value + dash + 1, length - dash - 1, &last) || last < first) return 416;
    if (last >= total) last = total - 1;
  }
  out->first = first; out->last = last; out->partial = 1; return 206;
}
#endif
