"""Keep missing data, historical minima and paired DHT readings honest."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('review_mdns',Path(__file__).resolve().parents[2]/'scripts/review-mdns-disable.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


class MdnsDisableReviewTests(unittest.TestCase):
    def test_empty_log_is_unobserved(self):
        result=module.serial_summary('')
        self.assertEqual(result['metricCount'],0)
        self.assertIsNone(result['minimumCurrentDram8'])
        self.assertEqual(result['completeNaturalDhtCycles'],0)

    def test_historical_low_water_does_not_replace_current_free(self):
        result=module.serial_summary('[METRICS] heap=45000 min=14000 dram8=42000 blk8=24000 frag=43% mqtt_q=0 sse=0 poll=0ms up=61s')
        self.assertEqual(result['minimumCurrentDram8'],42000)
        self.assertEqual(result['historicalMinimumSinceBoot'],14000)

    def test_disabled_load_and_no_start_are_separate_observations(self):
        result=module.serial_summary('NetworkManager: Config loaded - enableMDNS=false, customDomain=synthetic\nDNSManager: Starting mDNS with hostname synthetic')
        self.assertEqual(result['configDisabledLogCount'],1)
        self.assertEqual(result['mdnsStartLogCount'],1)

    def test_uptime_reset_and_panic_retained(self):
        text='[METRICS] heap=40000 min=30000 dram8=40000 blk8=25000 frag=37% mqtt_q=0 sse=0 poll=0ms up={}s\n'
        result=module.serial_summary(text.format(90)+text.format(30)+'Guru Meditation Error')
        self.assertEqual(result['metricUptimeRegressions'],1)
        self.assertEqual(result['panicLogCount'],1)

    def test_temperature_and_humidity_count_one_cycle(self):
        text=''
        for field in ['temperature','humidity']:
            text+='[PeriphExec] Executing action: type=19 on dht11, {"dataField":"'+field+'"}\n'
            text+='Sensor read: periph=dht11 cat=dht11 raw=23.0 unit=test label='+field+'\n'
            text+='[PeriphExec] Action completed: result=SUCCESS\n'
        result=module.serial_summary(text)
        self.assertEqual(result['completeNaturalDhtCycles'],1)
        self.assertEqual(result['successfulNaturalDhtCycles'],1)

    def test_partial_pair_not_success(self):
        result=module.serial_summary('[PeriphExec] Executing action: type=19 on dht11, {"dataField":"temperature"}\n[PeriphExec] Action completed: result=SUCCESS\n')
        self.assertEqual(result['successfulNaturalDhtCycles'],0)
        self.assertEqual(result['incompleteNaturalDhtCycles'],1)


if __name__=='__main__':unittest.main(verbosity=2)
