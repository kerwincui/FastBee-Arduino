"""Calendar/formatting and real DS1302 driver GPIO-mock regression."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

class Ds1302DateTimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler: raise unittest.SkipTest('C++17 compiler required')
        cls.folder=tempfile.TemporaryDirectory(prefix='fastbee-rtc-datetime-')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.binary=Path(cls.folder.name)/('rtc.exe' if os.name=='nt' else 'rtc')
        command=[compiler,'-std=c++17','-Wall','-Wextra','-Werror','-DFASTBEE_ENABLE_DS1302=1',
                 '-I'+str(ROOT/'test/unit/cpp/rtc_datetime_mocks'),
                 '-I'+str(ROOT/'test/unit/cpp/mocks'),'-I'+str(ROOT/'include'),
                 str(ROOT/'test/unit/cpp/rtc_datetime_harness.cpp'),
                 str(ROOT/'src/peripherals/drivers/DS1302Driver.cpp'),'-o',str(cls.binary)]
        result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)

    def run_case(self,name):
        result=subprocess.run([str(self.binary),name],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_calendar_ranges_and_leap_days(self): self.run_case('calendar')
    def test_default_minutes_and_explicit_legacy_format(self): self.run_case('format')
    def test_unix_epoch_and_invalid_calendar_sentinel(self): self.run_case('timestamp')
    def test_twelve_hour_midnight_noon_and_boundaries(self): self.run_case('read-hours')
    def test_invalid_read_is_not_formatted_as_a_valid_time(self): self.run_case('read-invalid')
    def test_invalid_write_has_no_gpio_side_effect(self): self.run_case('write-guard')
    def test_mode_conversion_preserves_all_twenty_four_hours(self): self.run_case('mode')
    def test_passive_instance_observation_never_touches_gpio(self): self.run_case('observation')

if __name__=='__main__': unittest.main()
