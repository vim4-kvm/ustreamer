# VIM4 backend

Build with `make WITH_VIM4=1 WITH_JANUS=1 WITH_PYTHON=1 PY=python3.14` in the
workspace's ARM64 Ubuntu PiKVM builder. A build without WITH_VIM4 retains the
upstream V4L2 backend. Clean before changing build flags.

The VIM4 backend in `src/ustreamer/vim4.c` uses the existing vfmcapsrc, amlvenc
and vim4jpegenc GStreamer plugins. Raw video stays in DMA memory. Encoded frames
enter the upstream JPEG HTTP ring and JPEG/H.264 memsinks, so kvmd and the Janus
plugin retain their established interfaces. Keyframe requests from memsink
consumers become GStreamer force-key-unit events.

With `VIM4_VIDEO_BACKEND=wave5`, the common launcher selects `vim4v4l2src` and
`vim4wave5enc` from the mainline-only plugin directory. The JPEG/memsink/Janus
interfaces stay the same. Install the matched board runtime and patched V4L2
plugin together; these factories are distinct from the 5.15 vendor factories.

Input dimensions follow capture caps. `--desired-fps` limits output without
duplicating frames; the encoder derives its rate-control budget from negotiated
caps. The H.264 bitrate range is 1000–60000 kbps. JPEG quality uses the existing
`--quality` option. The backend does not change EDID, HPD or the board HDMI service.
Capture errors or missing frames restart the pipeline after releasing its buffers;
unchanging pixels do not imply a stalled capture.
On vendor 5.15, cached `hw_info` bit 16 establishes RX lock: timing fields in
`info` can remain stale during loss. Input geometry, color, depth, EOTF and rate
are checked every 100 ms; two matching changed observations reopen capture.
HDMI's centi-Hz measurement can briefly range from 5987 to 5999 on the same
60 Hz input. A rate change must exceed 1 Hz; isolated loss readings do not
interrupt live frames. Planned rebuilds retain the transport and skip the
one-second hardware-failure backoff. Actual missing frames retain the three-second
watchdog. This reduces application waits but cannot eliminate HDMI resynchronization.
Before starting vendor encoding, reserve its workspace ahead of JPEG/VDIN1
allocation and drop the bootstrap after the first encoded frame. Use the matched
capture plugin's six-buffer physical pool to leave CMA for input reallocation.
Receivers with `Capture Frame Rate` use that stable base rate for VRR budgets
and mode-change detection; older kernels keep the measured-rate fallback.
This does not advertise or qualify VRR. The parser's offline regression runs
with `python3 tests/test_vim4_rate.py` in a GLib development environment.

Only one KVM application may own capture and encoding. Run through the workspace
PiKVM service, which provides plugin paths and coordinates application switching.
Actual hardware verification is recorded in the workspace bringup log.

The VIM4 build accepts `VIM4_VIDEO_CODEC=h265` in the shared service environment.
Capture uses amlvenc HEVC and h265parse; Janus advertises HEVC Main and sends
RFC 7798 single NAL units or fragmentation units, preserving layer and temporal
IDs. The existing compressed-video memsink retains its `::h264` name for API
compatibility; each frame carries its actual fourcc. H.264 remains the default.
`janus/test-rtpv.c` checks packet-size boundaries and reassembly for both codecs.

On qualified native-HDR 5.15, H.265 instead uses the shared `vim4h265src` and
Janus advertises Main10. It follows the manager's global HDR-to-SDR preference:
PQ can retain Main10/BT.2020, while SDR or converted input uses eight-bit HEVC.
Only native HDR uses an independent VDIN1 JPEG branch. SDR shares one VDIN1
capture between JPEG and HEVC because two opens would contend for that device.
An input/preference change
releases and rebuilds the pipeline without terminating uStreamer. Deploy the
encoder package providing `vim4-hevc-hdr-api-1` and the common GPU launcher first.
PiKVM Direct derives profile/level from SPS and presents HDR via a native video
track where available. Browsers lacking Main10 negotiation receive a clear
WebRTC compatibility message; H.264 remains available for SDR. Safari remote
HDR and HLG remain unqualified.

VIM4 WebRTC requests immediate video playout through the negotiated RTP
playout-delay extension (min=0/max=0) on the first packet of each frame, for
both H.264 and H.265 regardless of GOP length. Janus handles the negotiated
extension ID; receivers without support retain their own playout policy.
This does not change encoding quality, GOP, audio, Direct playback or non-VIM4
builds, and does not imply zero capture-to-display latency.

Publishing a live JPEG frame completes a pending HTTP snapshot request, matching
the normal JPEG worker. This avoids waiting for the HTTP fallback timeout despite
fresh frames being available; offline frames still follow the existing timeout.
