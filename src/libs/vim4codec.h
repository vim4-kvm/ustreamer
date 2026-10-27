// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Match the shared source's runtime gate without opening capture or encoding.
// Main10 is reserved even for SDR so a global mapping change needs no SDP swap.
static inline bool us_vim4_main10(void) {
#ifdef WITH_VIM4
    const char *backend = getenv("VIM4_VIDEO_BACKEND");
    const char *secondary = getenv("VIM4_SECONDARY_HDMI_CAPTURE");
    if ((backend && strcmp(backend, "wave5") == 0) || !secondary || strcmp(secondary, "1")) return false;
    FILE *file = fopen("/sys/module/aml_media/parameters/vdin0_native_hdr", "r");
    if (!file) return false;
    int enabled = fgetc(file);
    fclose(file);
    file = fopen("/sys/module/vfm_cap/parameters/hdr_metadata_version", "r");
    if (!file) return false;
    int version = fgetc(file);
    fclose(file);
    return enabled == 'Y' && version == '1';
#else
    return false;
#endif
}

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
