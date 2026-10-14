// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Capture and Janus inherit the same service setting; never negotiate a codec
// different from the one written to the compressed-video memsink.
static inline bool us_vim4_h265(void) {
#ifdef WITH_VIM4
    const char *codec = getenv("VIM4_VIDEO_CODEC");
    return codec != NULL && strcmp(codec, "h265") == 0;
#else
    return false;
#endif
}
