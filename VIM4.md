# VIM4 backend

This page covers PiKVM capture/Janus ownership, recovery and qualification limits.
Use the workspace service and matched platform plugins; only one application may own capture.
Read [build](#build-and-interfaces), [capture](#capture-and-recovery), or [HDR](#hdr-and-transport).
Options, defaults and pool budgets belong to the linked source.

## Build and interfaces

[Makefile](Makefile) owns flags/dependencies; clean when changing flags.
Non-VIM4 builds retain upstream capture. Use the workspace ARM64 PiKVM builder.
[vim4.c](src/ustreamer/vim4.c) retains DMA raw frames and upstream HTTP/memsink interfaces.
Consumer keyframe requests become force-key-unit events. The compressed memsink keeps its historical name;
frame fourcc identifies the actual codec.
Install matched runtime/capture/encoder plugins before applications and use the common launcher,
which selects backend/plugin paths and coordinates switching.

## Capture and recovery

[vim4.c](src/ustreamer/vim4.c) owns lock checks, rate parsing and restart timing.
Timing fields can remain stale after signal loss; check RX lock.
Measured rate jitter is not necessarily a mode change. Use stable capture base rate where available,
with measured fallback for older receivers. This neither advertises nor qualifies VRR.
FPS limits drop without duplication; budget is not capture pacing.

Planned rebuilds retain transport and skip hardware-failure backoff; missing frames retain watchdog recovery.
Static pixels are not a stall. Software cannot remove HDMI resynchronization waits.
Release buffers before restart. Reserve vendor encoder workspace before JPEG/capture allocations,
then transfer ownership after first output. Excess capture buffers can exhaust contiguous memory on reallocation.
Do not change EDID/HPD or the independent HDMI service.
Live JPEG publication completes pending snapshots; offline frames retain timeout behavior.

## HDR and transport

Pipeline selection lives in [vim4.c](src/ustreamer/vim4.c).
Wave5 JPEG borrows the shared HEVC raw pad and tone maps PQ before conversion:
never open a second capture owner. Bound/leak raw JPEG queues while retaining encoded prediction chains.
Vendor native HDR keeps independent SDR JPEG; vendor SDR shares capture with HEVC to avoid competing opens.
Input/policy changes release and rebuild without terminating uStreamer.

Deploy matched GPU launcher and codec ABI first.
[SDP](janus/src/sdp.c) and [RTP](janus/src/rtpv.c) preserve actual HEVC capability and NAL layer/temporal IDs.
Report Main10 incompatibility clearly; H.264 remains SDR.
Direct uses SPS profile/level and native video where available.
Safari remote HDR and HLG are unqualified. Both mainline GPU stacks and the actual PiKVM viewer remain board gates.

[Janus playout](janus/src/plugin.c) requests prompt VIM4 presentation regardless of GOP;
unsupported receivers retain their policy. It promises no zero latency and does not change audio/Direct.
Offline regressions: [rate parser](tests/test_vim4_rate.py), [packet boundaries](janus/test-rtpv.c).
Hardware evidence: workspace [bringup index](https://github.com/vim4-kvm/vim4-kvm/blob/main/docs/bringup/README.md).
