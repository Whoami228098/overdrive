// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Native titles use the C locale. Libarchive performs UTF-8 archive decoding itself.
#include <langinfo.h>
#if !defined(PS5_REACT_TORRENT) || !PS5_REACT_TORRENT
// The torrent build links SDK libc's C-locale implementation for libc++.
// Retain the small fallback only for hosts that do not link that SDK archive.
int ___mb_cur_max(void) { return 1; }
#endif
char *nl_langinfo(nl_item item) {
    static char codeset[] = "US-ASCII";
    static char unavailable[] = "";
    return item == CODESET ? codeset : unavailable;
}
