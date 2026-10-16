# VIM4 backend

Build with `make WITH_VIM4=1 WITH_JANUS=1 WITH_PYTHON=1 PY=python3.14` in the
workspace's ARM64 Ubuntu PiKVM builder. A build without WITH_VIM4 retains the
upstream V4L2 backend. Clean before changing build flags.

The VIM4 backend in `src/ustreamer/vim4.c` uses the existing vfmcapsrc, amlvenc
and vim4jpegenc GStreamer plugins. Raw video stays in DMA memory. Encoded frames
enter the upstream JPEG HTTP ring and JPEG/H.264 memsinks, so kvmd and the Janus
plugin retain their established interfaces. Keyframe requests from memsink
consumers become GStreamer force-key-unit events.

Input dimensions follow capture caps. `--desired-fps` limits output without
duplicating frames; the encoder derives its rate-control budget from negotiated
caps. The H.264 bitrate range is 1000–60000 kbps. JPEG quality uses the existing
`--quality` option. The backend does not change EDID, HPD or the board HDMI service.
Capture errors or missing frames restart the pipeline after releasing its buffers;
unchanging pixels do not imply a stalled capture.
HDMI's centi-Hz measurement can briefly range from 5987 to 5999 on the same
60 Hz input. A rate change must exceed 1 Hz and persist for two polls before
rebuilding the pipeline; isolated zero readings do not interrupt live frames.

Only one KVM application may own capture and encoding. Run through the workspace
PiKVM service, which provides plugin paths and coordinates application switching.
Actual hardware verification is recorded in the workspace bringup log.

The VIM4 build accepts `VIM4_VIDEO_CODEC=h265` in the shared service environment.
Capture uses amlvenc HEVC and h265parse; Janus advertises HEVC Main and sends
RFC 7798 single NAL units or fragmentation units, preserving layer and temporal
IDs. The existing compressed-video memsink retains its `::h264` name for API
compatibility; each frame carries its actual fourcc. H.264 remains the default.
`janus/test-rtpv.c` checks packet-size boundaries and reassembly for both codecs.

VIM4 WebRTC requests immediate video playout through the negotiated RTP
playout-delay extension (min=0/max=0) on the first packet of each frame, for
both H.264 and H.265 regardless of GOP length. Janus handles the negotiated
extension ID; receivers without support retain their own playout policy.
This does not change encoding quality, GOP, audio, Direct playback or non-VIM4
builds, and does not imply zero capture-to-display latency.

Publishing a live JPEG frame completes a pending HTTP snapshot request, matching
the normal JPEG worker. This avoids waiting for the HTTP fallback timeout despite
fresh frames being available; offline frames still follow the existing timeout.
