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

Only one KVM application may own capture and encoding. Run through the workspace
PiKVM service, which provides plugin paths and coordinates application switching.
Actual hardware verification is recorded in the workspace bringup log.
