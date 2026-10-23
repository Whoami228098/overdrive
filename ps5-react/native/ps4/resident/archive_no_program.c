// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// OpenOrbis has no supported child-process decoder interface. libarchive may
// offer an external-program fallback for missing optional codecs; fail that
// route explicitly. ZIP, RAR, 7z, gzip, bzip2 and xz use linked native decoders.
#include <errno.h>
#include <sys/types.h>
int __archive_create_child(const char* command,int* input,int* output,pid_t* child) {
  (void)command;if(input)*input=-1;if(output)*output=-1;if(child)*child=-1;
  errno=ENOSYS;return -1;
}
void __archive_check_child(int input,int output){(void)input;(void)output;}
