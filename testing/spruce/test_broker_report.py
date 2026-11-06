"""Verify telemetry arithmetic against known intervals and counter boundaries."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('report',Path(__file__).with_name('broker-report.py'))
r=importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)

class ReportTests(unittest.TestCase):
    def test_weighted_average_and_rolling_minimum(self):
        value=r.rates([(0,10,600),(10,30,600)],0,30)
        self.assertAlmostEqual(value['avg_fps'],40)
        self.assertAlmostEqual(value['slowest_10s_fps'],30)
        self.assertAlmostEqual(value['coverage_s'],30)

    def test_missing_coverage_does_not_become_zero_or_full_window(self):
        value=r.rates([(0,5,300),(15,20,300)],0,20)
        self.assertEqual(value['avg_fps'],60)
        self.assertEqual(value['coverage_s'],10)
        self.assertIsNone(value['slowest_10s_fps'])

    def test_bin_overlap_and_clipping(self):
        value=r.rates([(0,10,600),(9.999,20,600)],5,15)
        self.assertAlmostEqual(value['coverage_s'],10)
        self.assertAlmostEqual(value['avg_fps'],60,places=2)

    def test_ra_counter_baseline_excludes_startup_load(self):
        end=r.datetime(2026,11,6,0,0,30).timestamp()
        text='\n'.join([
          '[00:00:00.000] [netplay-shim] ra: frame 0, 600 run, 0 stalled (max 0), crc 0 bad of 0, states 1 loaded',
          '[00:00:10.000] [netplay-shim] ra: frame 600, 600 run, 0 stalled (max 2), crc 1 bad of 2, states 2 loaded',
          '[00:00:20.000] [netplay-shim] ra: frame 1200, 600 run, 0 stalled (max 3), crc 2 bad of 4, states 3 loaded',
          '[00:00:30.000] [netplay-shim] ra: frame 1800, 600 run, 0 stalled (max 4), crc 3 bad of 6, states 4 loaded'])
        value=r.device_metrics(text,'retroarch',end-25,end-5)
        self.assertEqual(value['checks_failed'],2)
        self.assertEqual(value['checks_total'],4)
        self.assertEqual(value['state_resends'],2)
        self.assertAlmostEqual(value['avg_fps'],60)
        self.assertAlmostEqual(value['longest_freeze_ms'],50)

    def test_link_pacing_without_stall_or_crc_is_unknown(self):
        end=r.datetime(2026,11,6,0,0,10).timestamp()
        text='[00:00:10.000] [netplay-shim] link pacing: 600 frames in 10000ms, 1000 packets, 0 dropped'
        value=r.device_metrics(text,'linkplay',end-10,end)
        self.assertEqual(value['avg_fps'],60)
        self.assertIsNone(value['longest_freeze_ms'])
        self.assertIsNone(value['checks_failed'])
        self.assertIsNone(value['state_resends'])

if __name__=='__main__':unittest.main()
