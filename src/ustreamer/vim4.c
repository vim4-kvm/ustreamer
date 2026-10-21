// SPDX-License-Identifier: GPL-3.0-or-later
// VIM4 capture owns DMA buffers through GStreamer; HTTP and memsink stay upstream.
#ifdef WITH_VIM4
#include <gst/app/gstappsink.h>
#include <gst/video/video-event.h>
#include <linux/videodev2.h>

#include "stream.h"
#include "../libs/logging.h"
#include "../libs/process.h"
#include "../libs/vim4codec.h"

static uint _input_rate(void) {
	if (g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5") == 0) {
		GstElement *source = gst_element_factory_make("vim4v4l2src", NULL);
		gint rate = 0;
		if (source) {
			g_object_get(source, "input-rate", &rate, NULL);
			gst_object_unref(source);
		}
		return rate;
	}
	char *text = NULL;
	uint rate = 0;
	if (g_file_get_contents("/sys/class/hdmirx/hdmirx0/info", &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (char **line = lines; *line != NULL; ++line) {
			if (sscanf(*line, " Frame Rate :%u", &rate) == 1) break;
		}
		g_strfreev(lines);
		g_free(text);
	}
	return rate >= 100 && rate <= 12000 ? rate : 0;
}

static void _offline(us_stream_s *stream) {
	us_fpsi_meta_s offline = {.online = false};
	us_fpsi_update(stream->run->http->h264_fpsi, false, &offline);
	for (unsigned i = 0; i < 10 && !atomic_load(&stream->run->stop); ++i) {
		if (i == 0) us_stream_vim4_publish(stream, stream->run->blank->jpeg);
		g_usleep(100000);
	}
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
	while (!atomic_load(&stream->run->stop)) {
		const uint input_rate = _input_rate();
		if (!input_rate) {
			_offline(stream);
			continue;
		}
		const uint output_rate = stream->desired_fps ? MIN(input_rate, stream->desired_fps * 100) : input_rate;
		const bool wave5 = g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5") == 0;
		const char *codec = us_vim4_h265() ? "h265" : "h264";
		char *encoded = stream->h264_sink ? g_strdup_printf(
			"capture. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
			"%s name=encoder framerate=0 min-force-key-unit-interval=250000000 ! "
			"video/x-%s ! %sparse config-interval=-1 ! "
			"video/x-%s,stream-format=byte-stream,alignment=au ! "
			"appsink name=h264 sync=false max-buffers=2",
			wave5 ? "vim4wave5enc" : "amlvenc", codec, codec, codec) : g_strdup("");
		// vfm_cap reports variable-rate caps and no buffer duration. videorate
		// needs explicit output caps to produce valid durations and encoder budgets.
		char *description = g_strdup_printf(
			"%s ! videorate drop-only=true ! video/x-raw,framerate=%u/100 ! tee name=capture "
			"capture. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
			"vim4jpegenc quality=%u ! appsink name=jpeg sync=false max-buffers=1 drop=true %s",
			wave5 ? "vim4v4l2src" : "vfmcapsrc", output_rate, stream->cap->jpeg_quality, encoded);
		g_free(encoded);
		GError *error = NULL;
		GstElement *pipeline = gst_parse_launch(description, &error);
		g_free(description);
		if (error != NULL) {
			US_LOG_ERROR("VIM4 pipeline: %s", error->message);
			g_clear_error(&error);
			if (pipeline != NULL) gst_object_unref(pipeline);
			us_process_suicide();
			break;
		}
		GstElement *jpeg = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg");
		GstElement *h264 = gst_bin_get_by_name(GST_BIN(pipeline), "h264");
		GstElement *encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
		if (encoder != NULL) {
			g_object_set(encoder, "bitrate", stream->h264_bitrate, "gop", stream->h264_gop, NULL);
		}
		GstBus *bus = gst_element_get_bus(pipeline);
		bool playing = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
		ldf last_frame = us_get_now_monotonic();
		ldf last_rate_check = last_frame;
		uint pending_rate = input_rate;
		uint rate_change_count = 0;
		while (playing && !atomic_load(&stream->run->stop)) {
			bool received = _publish_sample(stream, jpeg, false);
			if (h264 != NULL) received |= _publish_sample(stream, h264, true);
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
			if (us_get_now_monotonic() - last_rate_check > 1) {
				last_rate_check = us_get_now_monotonic();
				// HDMI's measured centi-Hz value can briefly jump even while
				// frames arrive normally. Require two stable, materially different
				// readings before disrupting capture; the watchdog handles stalls.
				const uint next_rate = _input_rate();
				if (!next_rate || abs((int)next_rate - (int)input_rate) > 100) {
					if (abs((int)next_rate - (int)pending_rate) <= 10) ++rate_change_count;
					else rate_change_count = 1;
					pending_rate = next_rate;
					if (rate_change_count >= 2) {
						US_LOG_INFO("VIM4 input rate changed: %u -> %u centi-Hz; restarting", input_rate, next_rate);
						break;
					}
				} else rate_change_count = 0;
			}
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(bus);
		gst_object_unref(jpeg);
		if (h264 != NULL) gst_object_unref(h264);
		if (encoder != NULL) gst_object_unref(encoder);
		gst_object_unref(pipeline);
		_offline(stream);
	}
}
#endif
