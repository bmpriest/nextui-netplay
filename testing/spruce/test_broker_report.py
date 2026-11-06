"""Verify telemetry arithmetic against known intervals and counter boundaries."""
import importlib.util
from pathlib import Path
import unittest
import json
import tempfile

spec=importlib.util.spec_from_file_location('report',Path(__file__).with_name('broker-report.py'))
r=importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)

class ReportTests(unittest.TestCase):
    def test_transport_report_accepts_incomplete_or_failed_suites(self):
        spec=importlib.util.spec_from_file_location('transport_report',Path(__file__).with_name('transport-report.py'))
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'adhoc-tcp').mkdir()
            (root/'adhoc-tcp/results.json').write_text(json.dumps([{'cell':'linkplay-fc','verdict':'INFRA_FAIL','reason':'navigation failed'}]))
            module.main(root)
            self.assertIn('navigation failed',(root/'report.md').read_text())
            self.assertTrue((root/'comparison.csv').exists())

    def test_failed_postplay_recovers_window_with_numeric_error_line(self):
        with tempfile.TemporaryDirectory() as directory:
            attempt=Path(directory); (attempt/'gameplay').mkdir()
            events=[{'event':'command','line':'host:TRM sh gameplay.sh --bg 60','time':'2026-11-06T00:00:00+00:00'},
                    {'event':'command','line':'guest:TRM sh gameplay.sh --bg 60','time':'2026-11-06T00:00:01+00:00'},
                    {'event':'error','line':56,'time':'2026-11-06T00:01:05+00:00'}]
            (attempt/'gameplay/events.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            window=r.measurement_window(attempt)
            self.assertEqual(window['end']-window['start'],59)

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

    def test_midgame_rebootstrap_is_product_failure_and_freeze_unknown(self):
        spec=importlib.util.spec_from_file_location('judge',Path(__file__).with_name('judge.py'))
        judge=importlib.util.module_from_spec(spec);spec.loader.exec_module(judge)
        end=r.datetime(2026,11,6,0,0,20).timestamp()
        text='[00:00:10.000] [netplay-shim] connection 3 requires a fresh instanced bootstrap\n[00:00:11.000] [netplay-shim] adopted authoritative paired checkpoint'
        self.assertEqual(judge.inspect(text,end-20,end)['verdict'],'PRODUCT_FAIL')
        metrics=r.device_metrics(text,'linkplay',end-20,end)
        self.assertIsNone(metrics['longest_freeze_ms'])
        self.assertEqual(len(metrics['interruptions']),1)
        self.assertEqual(metrics['state_resends'],1)

    def test_udp_hash_duplicates_are_one_logical_check(self):
        end=r.datetime(2026,11,6,0,0,10).timestamp()
        line='[00:00:10.000] [netplay-shim] in sync at frame 600'
        value=r.device_metrics('\n'.join([line]*3),'linkplay',end-10,end)
        self.assertEqual(value['checks_total'],1)
        self.assertEqual(value['raw_check_messages'],3)

    def test_paired_desync_counts_as_failed_check(self):
        end=r.datetime(2026,11,6,0,0,10).timestamp()
        value=r.device_metrics('[00:00:10.000] [netplay-shim] PAIRED DESYNC at frame 600', 'linkplay',end-10,end)
        self.assertEqual(value['checks_failed'],1)
        self.assertEqual(value['checks_total'],1)

    def test_link_pacing_without_stall_or_crc_is_unknown(self):
        end=r.datetime(2026,11,6,0,0,10).timestamp()
        text='[00:00:10.000] [netplay-shim] link pacing: 600 frames in 10000ms, 1000 packets, 0 dropped'
        value=r.device_metrics(text,'linkplay',end-10,end)
        self.assertEqual(value['avg_fps'],60)
        self.assertIsNone(value['longest_freeze_ms'])
        self.assertIsNone(value['checks_failed'])
        self.assertIsNone(value['state_resends'])

if __name__=='__main__':unittest.main()
