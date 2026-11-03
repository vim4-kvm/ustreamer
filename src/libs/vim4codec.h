// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef WITH_VIM4
#include <gst/gst.h>
#endif

// The shared source probes formats, controls and plugins without streaming.
// Main10 is reserved even for SDR so a global mapping change needs no SDP swap.
static inline bool us_vim4_main10(void) {
#ifdef WITH_VIM4
    const char *backend = getenv("VIM4_VIDEO_BACKEND");
    if (!gst_init_check(NULL, NULL, NULL)) return false;
    GstElement *source = gst_element_factory_make("vim4h265src", NULL);
    if (!source) return false;
    gint capable = 0;
    g_object_get(source, "hdr-capable", &capable, NULL);
    gst_object_unref(source);
    // Wave5 has one VDIN owner. JPEG must share its raw pad and tone mapper.
    GstElementFactory *mapper = gst_element_factory_find("vim4pq2sdr");
    bool available = !backend || strcmp(backend, "wave5") || mapper;
    if (mapper) gst_object_unref(mapper);
    return capable == 1 && available;
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
