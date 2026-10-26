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
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <glib.h>
'''
        main = r'''
int main(void) {
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
 return 0;
}
'''
        flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0'],
                               capture_output=True, text=True, check=True).stdout
        with tempfile.TemporaryDirectory() as temporary:
            source, binary = Path(temporary) / 'rate.c', Path(temporary) / 'rate'
            source.write_text(fixture + text[start:end] + main)
            subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Werror', str(source),
                            '-o', str(binary), *shlex.split(flags)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
