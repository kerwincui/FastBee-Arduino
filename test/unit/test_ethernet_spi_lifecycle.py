"""Compile actual W5500 adapter and SPI leases; mock only SDK/network/GPIO."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class EthernetSpiLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler: raise unittest.SkipTest('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-ethernet-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binaries = {}
        for mode, dedicated, supported, compiled in [('shared',0,1,1),('dedicated',1,1,1),
                                                     ('unsupported',0,0,1),('disabled',0,1,0),('unlocked',0,1,1)]:
            binary=Path(cls.directory.name)/(mode+'.exe')
            args=[compiler,'-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                  '-DCHIP_ETHERNET_DEDICATED_SPI='+str(dedicated),
                  '-DETH_SPI_SUPPORTS_CUSTOM='+str(supported),
                  '-DFASTBEE_ENABLE_ETHERNET='+str(compiled),
                  '-DCONFIG_DISABLE_HAL_LOCKS='+str(int(mode=='unlocked'))]
            args += ['-I'+str(ROOT/p) for p in ('test/unit/cpp/ethernet_mocks',
                     'test/unit/cpp/spi_bus_mocks','test/mocks',
                     '.pio/libdeps/native/ArduinoJson/src','include')]
            args += [str(ROOT/p) for p in ('test/unit/cpp/ethernet_lifecycle_harness.cpp',
                     'src/network/EthernetAdapter.cpp','src/core/SpiBusInitializer.cpp')]
            result=subprocess.run(args+['-o',str(binary)],capture_output=True,text=True,
                                  encoding='utf-8',errors='replace',timeout=90)
            if result.returncode: raise AssertionError(mode+': '+result.stdout+result.stderr)
            cls.binaries[mode]=binary

    def run_case(self, case):
        for mode in ('shared','dedicated'):
            with self.subTest(mode=mode):
                result=subprocess.run([str(self.binaries[mode]),case],capture_output=True,text=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_shared_and_dedicated_host_identity(self): self.run_case('normal')
    def test_existing_peripheral_remains_running(self): self.run_case('peer-first')
    def test_network_first_protects_all_control_pins(self): self.run_case('network-first')
    def test_invalid_pins_have_no_io(self): self.run_case('pins')
    def test_conflict_rejected_before_gpio_and_callbacks(self): self.run_case('conflict-first')
    def test_each_partial_begin_failure_cleans_owned_resources(self): self.run_case('failed-begin')
    def test_failed_stop_retains_spi_and_lease_until_confirmed(self): self.run_case('failed-stop')
    def test_destructor_retains_failed_transport_without_dangling_callback(self): self.run_case('destructor')
    def test_busy_start_and_stop_do_not_change_hardware(self): self.run_case('busy')
    def test_changed_pins_ip_and_failed_static_config(self): self.run_case('config')
    def test_unmanaged_hardware_is_not_adopted_or_stopped(self): self.run_case('unmanaged')
    def test_callback_failure_and_late_delivery(self): self.run_case('events')
    def test_spi_begin_failure_and_retry(self): self.run_case('spi-failure')
    def test_thousand_lifecycles_keep_bounded_resources(self): self.run_case('cycles')
    def test_network_lease_identity_control_pins_and_lost_handle(self): self.run_case('lease-contract')
    def test_observation_is_passive_and_outlives_failed_adapter_cleanup(self): self.run_case('observation')
    def test_disabled_and_unsupported_core_have_no_io(self):
        for mode in ('disabled','unsupported','unlocked'):
            result=subprocess.run([str(self.binaries[mode]),'disabled'],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__ == '__main__': unittest.main()
