// SPDX-License-Identifier: GPL-3.0-or-later
// VIM4 capture owns DMA buffers through GStreamer; HTTP and memsink stay upstream.
#ifdef WITH_VIM4
#include <gst/app/gstappsink.h>
#include <gst/video/video-event.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "stream.h"
#include "../libs/logging.h"
#include "../libs/process.h"
#include "../libs/vim4codec.h"

static uint _parse_input_rate(const char *text) {
	uint rate = 0, capture_rate = 0;
	bool has_capture_rate = false;
	char **lines = g_strsplit(text, "\n", -1);
	for (char **line = lines; *line != NULL; ++line) {
		if (sscanf(*line, " Capture Frame Rate :%u", &capture_rate) == 1) {
			has_capture_rate = true;
		}
		sscanf(*line, " Frame Rate :%u", &rate);
	}
	g_strfreev(lines);
	if (has_capture_rate) rate = capture_rate;
	return rate >= 100 && rate <= 12000 ? rate : 0;
}

typedef struct {
	uint width, height, rate;
	char color[32], depth[16], eotf[48];
} input_mode_s;

static input_mode_s _parse_input_mode(const char *text) {
	input_mode_s mode = {.rate = _parse_input_rate(text)};
	if (!mode.rate) return mode;
	char **lines = g_strsplit(text, "\n", -1);
	for (char **line = lines; *line; ++line) {
		sscanf(*line, " Hactive :%u", &mode.width);
		sscanf(*line, " Vactive :%u", &mode.height);
		sscanf(*line, " Color Space : %31[^\n]", mode.color);
		sscanf(*line, " Color Depth : %15[^\n]", mode.depth);
		sscanf(*line, " HDR EOTF : %47[^\n]", mode.eotf);
	}
	g_strfreev(lines);
	return mode;
}

static bool _same_input(input_mode_s a, input_mode_s b) {
	// Preserve the vendor measurement tolerance. Stable VRR base rates are
	// supplied by Capture Frame Rate; cadence never forms a separate key.
	return a.width == b.width && a.height == b.height &&
		((!a.rate && !b.rate) || (a.rate && b.rate && abs((int)a.rate - (int)b.rate) <= 100)) &&
		!strcmp(a.color, b.color) && !strcmp(a.depth, b.depth) && !strcmp(a.eotf, b.eotf);
}

static bool _input_changed(input_mode_s initial, input_mode_s next, input_mode_s *pending, uint *count) {
	if (_same_input(initial, next)) {
		*count = 0;
		return false;
	}
	if (!_same_input(*pending, next)) *count = 0;
	*pending = next;
	return ++*count >= 2;
}

// Cached receiver state, not register access: bit 16 is FSM_SIG_READY in
// hdmi_rx_repeater.h. Timing fields alone remain stale while the link is lost.
static bool _signal_locked(const char *text) {
	return (g_ascii_strtoull(text, NULL, 16) & (1u << 16)) != 0;
}

static bool _vendor_signal_ready(void) {
	char *text = NULL;
	GError *error = NULL;
	bool ready = g_file_get_contents("/sys/class/hdmirx/hdmirx0/hw_info", &text, NULL, &error)
		? _signal_locked(text) : g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
	g_free(text);
	g_clear_error(&error);
	return ready;
}

static input_mode_s _input_mode(void) {
	if (g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5") == 0) {
		GstElement *source = gst_element_factory_make(g_getenv("VIM4_BROKER_SOCKET") ? "vim4brokersrc" : "vim4v4l2src", NULL);
		gint rate = 0;
		if (source) {
			g_object_get(source, "input-rate", &rate, NULL);
			gst_object_unref(source);
		}
		return (input_mode_s){.rate = rate};
	}
	char *text = NULL;
	input_mode_s mode = {0};
	if (!_vendor_signal_ready()) return mode;
	if (g_file_get_contents("/sys/class/hdmirx/hdmirx0/info", &text, NULL, NULL)) {
		mode = _parse_input_mode(text);
		g_free(text);
	}
	return _vendor_signal_ready() ? mode : (input_mode_s){0};
}

static void _offline(us_stream_s *stream) {
	us_fpsi_meta_s offline = {.online = false};
	us_fpsi_update(stream->run->http->h264_fpsi, false, &offline);
	us_stream_vim4_publish(stream, stream->run->blank->jpeg);
}

static bool _hdr_requested(void) {
	// Ask the common policy before opening either capture path. In SDR mode
	// JPEG and HEVC must share one VDIN1 source; two opens cannot own video71.
	GstElement *source = gst_element_factory_make("vim4h265src", NULL);
	if (!source) return false;
	gint requested = 0;
	g_object_get(source, "hdr-requested", &requested, NULL);
	gst_object_unref(source);
	return requested != 0;
}

static bool _publish_sample(us_stream_s *stream, GstElement *sink, bool h264) {
	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0);
	if (sample == NULL) {
		return false;
	}
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstMapInfo map;
	GstStructure *caps = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
	int width = 0, height = 0;
	gst_structure_get_int(caps, "width", &width);
	gst_structure_get_int(caps, "height", &height);
	if (width > 0 && height > 0 && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
		const ldf now = us_get_now_monotonic();
		us_frame_s frame = {
			.data = map.data, .used = map.size,
			.width = width, .height = height,
			.format = h264 ? (us_vim4_h265() ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264) : V4L2_PIX_FMT_JPEG,
			.online = true, .key = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT),
			.gop = stream->h264_gop,
			.grab_begin_ts = now, .grab_end_ts = now,
			.encode_begin_ts = now, .encode_end_ts = now,
		};
		us_stream_vim4_publish(stream, &frame);
		gst_buffer_unmap(buffer, &map);
	}
	gst_sample_unref(sample);
	return true;
}

void us_stream_vim4_loop(us_stream_s *stream) {
	gst_init(NULL, NULL);
	pthread_mutex_lock(&stream->enc->run->mutex);
	stream->enc->run->quality = stream->cap->jpeg_quality;
	stream->enc->run->type = US_ENCODER_TYPE_HW;
	pthread_mutex_unlock(&stream->enc->run->mutex);
	bool offline = false;
	while (!atomic_load(&stream->run->stop)) {
		const input_mode_s input = _input_mode();
		const uint input_rate = input.rate;
		if (!input_rate) {
			if (!offline) _offline(stream);
			offline = true;
			g_usleep(100000);
			continue;
		}
		offline = false;
		const uint output_rate = stream->desired_fps ? MIN(input_rate, stream->desired_fps * 100) : input_rate;
		const bool wave5 = g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5") == 0;
		const bool secondary = g_strcmp0(g_getenv("VIM4_SECONDARY_HDMI_CAPTURE"), "1") == 0;
		const bool native_hevc = us_vim4_h265() && us_vim4_main10();
		const bool shared_hevc = native_hevc && _hdr_requested();
		const char *height_setting = g_getenv("VIM4_OUTPUT_HEIGHT");
		const uint height = height_setting ? (uint)g_ascii_strtoull(height_setting, NULL, 10) : 0;
		const char *codec = us_vim4_h265() ? "h265" : "h264";
		char *scale = height && !wave5 ? g_strdup_printf("vim4scale height=%u width-alignment=%u ! "
			"queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! ",
			height, us_vim4_h265() ? 16 : 2) : g_strdup("");
		char *encoded = !stream->h264_sink ? g_strdup("") : shared_hevc ? g_strdup_printf(
			"vim4h265src name=encoder bitrate=%u gop=%u framerate=%u height=%u ! "
			"appsink name=h264 sync=false max-buffers=2",
			stream->h264_bitrate, stream->h264_gop, (output_rate + 50) / 100, height) : g_strdup_printf(
			"capture. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
			"%s%s name=encoder framerate=0 min-force-key-unit-interval=250000000 ! "
			"video/x-%s ! %sparse config-interval=-1 ! "
			"video/x-%s,stream-format=byte-stream,alignment=au ! "
			"appsink name=h264 sync=false max-buffers=2",
			scale, wave5 ? "vim4wave5enc" : "amlvenc", codec, codec, codec);
		g_free(scale);
		// vfm_cap reports variable-rate caps and no buffer duration. videorate
		// needs explicit output caps to produce valid durations and encoder budgets.
		char *capture = wave5 ? g_strdup_printf("%s height=%u",
			g_getenv("VIM4_BROKER_SOCKET") ? "vim4brokersrc" : "vim4v4l2src", height) :
			g_strdup(secondary ? "vim4vdin1src" : "vfmcapsrc");
		char *description = g_strdup_printf(
			"%s ! videorate drop-only=true ! video/x-raw,framerate=%u/100 ! tee name=capture "
			"capture. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
			"vim4jpegenc quality=%u ! appsink name=jpeg sync=false max-buffers=1 drop=true %s",
			capture,
			output_rate, stream->cap->jpeg_quality, encoded);
		g_free(capture);
		g_free(encoded);
		GError *error = NULL;
		GstElement *pipeline = gst_parse_launch(description, &error);
		g_free(description);
		if (error != NULL) {
			US_LOG_ERROR("VIM4 pipeline: %s", error->message);
			g_clear_error(&error);
			if (pipeline != NULL) gst_object_unref(pipeline);
			break;
		}
		GstElement *jpeg = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg");
		GstElement *h264 = gst_bin_get_by_name(GST_BIN(pipeline), "h264");
		GstElement *encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
		if (encoder != NULL) {
			g_object_set(encoder, "bitrate", stream->h264_bitrate, "gop", stream->h264_gop, NULL);
		}
		GstBus *bus = gst_element_get_bus(pipeline);
		// Reserve workspace before JPEG/VDIN1 pools split CMA. The SDK owns a
		// reference after the first encoded frame; final teardown must reset it.
		int bootstrap = !wave5 && h264 != NULL ? open("/dev/amvenc_multi", O_RDWR | O_CLOEXEC) : -1;
		bool reserved = wave5 || h264 == NULL || bootstrap >= 0;
		if (!reserved) US_LOG_ERROR("VIM4 capture: cannot reserve encoder workspace");
		bool playing = reserved && gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
		ldf last_frame = us_get_now_monotonic();
		ldf last_rate_check = last_frame;
		input_mode_s pending = input;
		uint change_count = 0;
		bool reconfigure = false;
		while (playing && !atomic_load(&stream->run->stop)) {
			bool received = _publish_sample(stream, jpeg, false);
			if (h264 != NULL && _publish_sample(stream, h264, true)) {
				received = true;
				if (bootstrap >= 0) {
					close(bootstrap);
					bootstrap = -1;
				}
			}
			if (received) last_frame = us_get_now_monotonic();
			if (encoder != NULL && stream->run->h264_key_requested) {
				GstPad *pad = gst_element_get_static_pad(encoder, "src");
				gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, true, 0));
				gst_object_unref(pad);
				stream->run->h264_key_requested = false;
			}
			GstMessage *message = gst_bus_timed_pop_filtered(bus, 5 * GST_MSECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
			if (message != NULL) {
				if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
					gst_message_parse_error(message, &error, NULL);
					US_LOG_ERROR("VIM4 capture: %s", error->message);
					g_clear_error(&error);
				}
				gst_message_unref(message);
				break;
			}
			// Frame arrival, never pixel changes, determines capture liveness.
			if (us_get_now_monotonic() - last_frame > 3) {
				US_LOG_ERROR("VIM4 capture: no frames for 3 seconds; restarting");
				break;
			}
			if (us_get_now_monotonic() - last_rate_check > 0.1) {
				last_rate_check = us_get_now_monotonic();
				if (native_hevc && _hdr_requested() != shared_hevc) {
					US_LOG_INFO("VIM4 HDR policy changed; rebuilding shared capture");
					reconfigure = true;
					break;
				}
				if (_input_changed(input, _input_mode(), &pending, &change_count)) {
					US_LOG_INFO("VIM4 input changed; reopening capture");
					reconfigure = true;
					break;
				}
			}
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
		if (bootstrap >= 0) close(bootstrap);
		gst_object_unref(bus);
		gst_object_unref(jpeg);
		if (h264 != NULL) gst_object_unref(h264);
		if (encoder != NULL) gst_object_unref(encoder);
		gst_object_unref(pipeline);
		// Keep the last image and the transport during a planned rebuild. A
		// source transition may have raised a bus error before our next poll.
		if (reconfigure || !_same_input(input, _input_mode()) ||
			(native_hevc && _hdr_requested() != shared_hevc)) continue;
		_offline(stream);
		offline = true;
		// Back off genuine failures, but wake early when the source changes.
		for (uint i = 0; i < 10 && !atomic_load(&stream->run->stop); ++i) {
			if (!_same_input(input, _input_mode())) break;
			g_usleep(100000);
		}
	}
}
#endif
