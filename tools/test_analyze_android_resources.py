#!/usr/bin/env python3

import copy
import unittest
from analyze_android_resources import summarize


class ResourceSummaryTest(unittest.TestCase):
    def fixture(self):
        rows = []
        for cpu, interval in ((100., 5000), (50., 10000)):
            rows.append({'run_id': 'current', 'threads': [
                {'tid': 7, 'start_time_ticks': 90, 'name': 'ring',
                 'cpu_percent_one_core': cpu, 'cpu_interval_ms': interval, 'last_cpu': 6}],
                'renderer': {'gpu_window_id': '12345', 'gpu_window_swaps': 200}})
        return {'game_session': {'run_id': 'current', 'performance_capture': {'samples': rows}}}

    def test_weighted_cpu_and_deduplicated_gpu(self):
        result = summarize(self.fixture())
        self.assertAlmostEqual(result['top_threads'][0]['mean_cpu_percent_one_core'], 200/3)
        self.assertEqual(result['top_threads'][0]['peak_cpu_percent_one_core'], 100)
        self.assertEqual(result['top_threads'][0]['observed_cpu_ids'], [6])
        self.assertEqual(len(result['gpu_windows']), 1)

    def test_other_session_and_unavailable_metrics(self):
        fixture = self.fixture()
        extra = copy.deepcopy(fixture['game_session']['performance_capture']['samples'][0])
        extra.update(run_id='old', rss_kib=999999)
        fixture['game_session']['performance_capture']['samples'].append(extra)
        result = summarize(fixture)
        self.assertEqual(result['samples'], 2)
        self.assertIsNone(result['rss_kib']['maximum'])

    def test_tid_reuse_is_a_distinct_lifetime(self):
        fixture = self.fixture()
        fixture['game_session']['performance_capture']['samples'][1]['threads'][0]['start_time_ticks'] = 900
        self.assertEqual(len(summarize(fixture)['top_threads']), 2)

    def test_empty_report_has_no_measurements(self):
        result = summarize({})
        self.assertEqual(result['samples'], 0)
        self.assertEqual(result['top_threads'], [])
        self.assertEqual(result['gpu_windows'], [])
        self.assertIsNone(result['process_cpu_percent_one_core']['maximum'])


if __name__ == '__main__':
    unittest.main()
