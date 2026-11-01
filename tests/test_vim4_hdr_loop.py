"""Run the production capture loop with simulated frame arrival, not hardware.

Real GStreamer bins/buses exercise teardown. Only capture, policy and clock are
stubbed so repeated policy polling/backoff needs no sleeping or HDMI device.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class HdrLoopTest(unittest.TestCase):
    def test_wave5_sdr_stays_running_roundtrips_rebuild_once_and_failures_back_off(self):
        source=(Path(__file__).parents[1]/'src/ustreamer/vim4.c').read_text()
        loop=source[source.index('void us_stream_vim4_loop('):source.rindex('\n#endif')]
        fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/video/video.h>
typedef long double ldf;
typedef struct { pthread_mutex_t mutex; unsigned quality,type; } encoder_run;
typedef struct { encoder_run *run; } encoder;
typedef struct { unsigned jpeg_quality; } capture;
typedef struct { atomic_bool stop; bool h264_key_requested; } stream_run;
typedef struct { encoder *enc; capture *cap; stream_run *run; unsigned desired_fps;
 void *h264_sink; unsigned h264_bitrate,h264_gop; } us_stream_s;
typedef struct { unsigned rate; } input_mode_s;
#define US_ENCODER_TYPE_HW 1
#define US_LOG_ERROR(...) ((void)0)
#define US_LOG_INFO(...) ((void)0)
static unsigned iterations,builds,offline,sleeps,scenario;
static bool hdr;
static ldf now;
static input_mode_s _input_mode(void) { return (input_mode_s){6000}; }
static bool _same_input(input_mode_s a,input_mode_s b) { return a.rate==b.rate; }
static bool _input_changed(input_mode_s a,input_mode_s b,input_mode_s *p,unsigned *n) { return false; }
static bool us_vim4_h265(void) { return true; }
static bool us_vim4_main10(void) { return true; }
static bool _hdr_requested(void) { return hdr; }
static void _offline(us_stream_s *s) { offline++; }
static ldf us_get_now_monotonic(void) { return now; }
static bool _publish_sample(us_stream_s *s,GstElement *sink,bool encoded) {
 if (!encoded) {
  iterations++;now+=0.11;
  if (scenario==1 && iterations==5) hdr=false;
  if (scenario==1 && iterations==10) hdr=true;
  if (iterations==30) atomic_store(&s->run->stop,true);
 }
 return true;
}
static GstElement *launch(const char *description,GError **error) {
 builds++;
 assert(strstr(description,"vim4h265src name=encoder"));
 return gst_parse_launch("appsink name=jpeg appsink name=h264",error);
}
static GstStateChangeReturn state(GstElement *pipeline,GstState state) {
 return scenario==2 && state==GST_STATE_PLAYING ? GST_STATE_CHANGE_FAILURE : GST_STATE_CHANGE_SUCCESS;
}
static stream_run *running;
static void pause_us(gulong delay) {
 assert(delay==100000);sleeps++;
 if (sleeps==3) atomic_store(&running->stop,true);
}
#define gst_parse_launch launch
#define gst_element_set_state state
#define g_usleep pause_us
'''
        main=r'''
int main(int argc,char **argv) {
 gst_init(&argc,&argv);g_setenv("VIM4_VIDEO_BACKEND","wave5",true);
 for(scenario=0;scenario<3;scenario++) {
  iterations=builds=offline=sleeps=0;now=0;hdr=scenario==1;
  encoder_run er={0};pthread_mutex_init(&er.mutex,NULL);
  encoder e={&er};capture c={80};stream_run r={0};running=&r;
  us_stream_s s={.enc=&e,.cap=&c,.run=&r,.h264_sink=&r,.h264_gop=60};
  us_stream_vim4_loop(&s);
  if(scenario==0) { assert(iterations==30);assert(builds==1);assert(offline==1&&!sleeps); }
  if(scenario==1) { assert(iterations==30);assert(builds==3);assert(offline==1&&!sleeps); }
  if(scenario==2) { assert(builds==1);assert(offline==1);assert(sleeps==3); }
  pthread_mutex_destroy(&er.mutex);
 }
 return 0;
}
'''
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gstreamer-app-1.0','gstreamer-video-1.0'],text=True))
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'loop.c';path.write_text(fixture+loop+main)
            program=Path(folder)/'loop'
            subprocess.run(['cc','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(path),'-o',str(program),*flags,'-pthread'],check=True)
            subprocess.run([str(program)],check=True,timeout=10)
