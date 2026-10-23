// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
// musl fputc has an inline FS-thread-ID fast path before __lockfile. Reuse
// fwrite's already wrapped lock path, including for libarchive diagnostics.
int __wrap_fputc(int value,FILE* stream) {
  const unsigned char byte=(unsigned char)value;
  return fwrite(&byte,1,1,stream)==1?byte:EOF;
}
