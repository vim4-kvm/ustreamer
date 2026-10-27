"""Exercise the real vendor-rate parser without opening capture or encoders."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class Vim4RateTest(unittest.TestCase):
    def test_vrr_keeps_nominal_budget_and_legacy_rates_still_work(self):
        text = (Path(__file__).parents[1] / 'src/ustreamer/vim4.c').read_text()
        start = text.index('static uint _parse_input_rate(')
        end = text.index('\n}\n', start) + 3
        modes_start = text.index('typedef struct {')
        modes_end = text.index('static bool _vendor_signal_ready(void)')
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
'''
        main = r'''
int main(void) {
 assert(_signal_locked("13b11\n"));
 assert(!_signal_locked("3b11"));
 assert(!_signal_locked("0"));
 assert(!_signal_locked("invalid"));
 assert(_parse_input_rate("Frame Rate: 5994\n")==5994);
 assert(_parse_input_rate("Frame Rate: 0\n")==0);
 assert(_parse_input_rate("Frame Rate: invalid\n")==0);
 for (unsigned rate=4800;rate<=12000;rate+=13) {
  char text[128];
  snprintf(text,sizeof(text),"Frame Rate: %u\nCapture Frame Rate: 12000\n",rate);
  assert(_parse_input_rate(text)==12000);
  snprintf(text,sizeof(text),"Capture Frame Rate: 12000\nFrame Rate: %u\n",rate);
  assert(_parse_input_rate(text)==12000);
 }
 assert(_parse_input_rate("Frame Rate: 4800\nCapture Frame Rate: 0\n")==0);
 assert(_parse_input_rate("Frame Rate: 4800\nCapture Frame Rate: 14400\n")==0);
 input_mode_s initial = _parse_input_mode("Hactive: 3840\nVactive: 2160\nFrame Rate: 5999\nHDR EOTF: SDR");
 const char *changes[] = {
  "Hactive: 3440\nVactive: 1440\nFrame Rate: 6000\nHDR EOTF: SDR",
  "Hactive: 3840\nVactive: 2160\nFrame Rate: 6000\nHDR EOTF: SMPTE_ST_2048",
  "",
 };
 for (unsigned i=0;i<3;i++) {
  input_mode_s pending={0}, next=_parse_input_mode(changes[i]);
  unsigned count=0;
  assert(!_input_changed(initial,next,&pending,&count));
  assert(_input_changed(initial,next,&pending,&count));
 }
 input_mode_s jitter=_parse_input_mode("Hactive: 3840\nVactive: 2160\nFrame Rate: 5987\nHDR EOTF: SDR");
 assert(_same_input(initial,jitter));
 input_mode_s a=_parse_input_mode("Hactive: 1920\nVactive: 1080\nFrame Rate: 4800\nCapture Frame Rate: 12000");
 input_mode_s b=_parse_input_mode("Hactive: 1920\nVactive: 1080\nFrame Rate: 11000\nCapture Frame Rate: 12000");
 assert(_same_input(a,b));
 unsigned count=0; input_mode_s pending={0};
 assert(!_input_changed(initial,a,&pending,&count));
 assert(!_input_changed(initial,jitter,&pending,&count));
 assert(!_input_changed(initial,a,&pending,&count));
 return 0;
}
'''
        flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0'],
                               capture_output=True, text=True, check=True).stdout
        with tempfile.TemporaryDirectory() as temporary:
            source, binary = Path(temporary) / 'rate.c', Path(temporary) / 'rate'
            source.write_text(fixture + text[start:end] + text[modes_start:modes_end] + main)
            subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Werror', str(source),
                            '-o', str(binary), *shlex.split(flags)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
