#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import copy
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from analyze_android_pixel_probe import analyze, load


def fixture():
    blocks = []
    for phase in range(1, 5):
        clip = phase in (2, 3)
        frames = 1250 if clip else 500
        blocks.append({'phase': phase, 'mode': 'scissor_1x1' if clip else 'normal',
            'frames': frames, 'frame_wall_ns': 25_000_000_000,
            'process_cpu_ns': 50_000_000_000, 'ring_cpu_ns': 7_500_000_000,
            'gpu_jobs': frames, 'gpu_ns': frames * (15_000_000 if clip else 45_000_000),
            'gpu_pending': 0, 'gpu_failed': 0, 'gpu_jobs_excluded': 0, 'failed_presents': 0,
            'draws': frames * 1000, 'scene_draws': frames * 900,
            'triangles_approx': frames * 100000, 'scene_triangles_approx': frames * 90000,
            'clipped_draws': frames * 900 if clip else 0})
    return {'trial': 1, 'state': 'complete', 'updated_monotonic_ns': '999',
            'process_cpu_clock_available': True, 'ring_cpu_clock_available': True, 'blocks': blocks}


class AnalysisTest(unittest.TestCase):
    def test_known_math(self):
        result = analyze({'run_id': 'current'}, [], fixture())
        self.assertEqual(result['measurement_errors'], [])
        self.assertEqual(result['comparison_warnings'], [])
        self.assertEqual(result['cadence_ratio_clipped_over_normal'], 2.5)
        self.assertAlmostEqual(result['gpu_interval_fraction_reduced'], 2 / 3)
        self.assertEqual(result['normal']['process_cpu_percent_one_core'], 200)

    def test_cancelled_empty_missing_gpu_and_wrong_mode(self):
        for change in ['cancelled', 'empty', 'gpu', 'zero_gpu', 'mode']:
            trial = fixture()
            if change == 'cancelled': trial['state'] = 'cancelled'
            if change == 'empty': trial['blocks'][1]['frames'] = 0
            if change == 'gpu': trial['blocks'][2]['gpu_pending'] = 1
            if change == 'zero_gpu': trial['blocks'][0]['gpu_ns'] = 0
            if change == 'mode': trial['blocks'][0]['clipped_draws'] = 1
            self.assertTrue(analyze({}, [], trial)['measurement_errors'], change)

    def test_geometry_feedback_stays_provisional(self):
        trial = fixture()
        trial['blocks'][1]['scene_draws'] //= 2
        result = analyze({}, [], trial)
        self.assertTrue(result['comparison_warnings'])
        self.assertTrue(result['investigation_direction'].startswith('Provisional'))

    def test_death_or_menu_phase_is_not_pooled_with_normal_scene(self):
        trial = fixture()
        menu = trial['blocks'][3]
        menu.update(frames=1500, gpu_jobs=1500, gpu_ns=18_000_000_000,
                    scene_draws=0, scene_triangles_approx=0)
        result = analyze({}, [], trial)
        self.assertFalse(result['full_comparison_available'])
        self.assertEqual(result['excluded_world_missing_blocks'], [4])
        self.assertIsNone(result['normal'])
        self.assertIsNone(result['scissor_1x1'])
        self.assertIsNone(result['cadence_ratio_clipped_over_normal'])
        self.assertIsNone(result['gpu_interval_fraction_reduced'])
        self.assertEqual(len(result['provisional_adjacent_pairs']), 1)
        pair = result['provisional_adjacent_pairs'][0]
        self.assertEqual((pair['normal_phase'], pair['clipped_phase']), (1, 2))
        self.assertEqual(pair['status'], 'provisional_only')
        self.assertEqual(pair['cadence_ratio_clipped_over_normal'], 2.5)

    def test_recovery_deduplicates_sessions_and_prefers_completed_checkpoint(self):
        session = {'run_id': 'current', 'app_version': '0.26-pixel-cost-test', 'pixel_probe': fixture()}
        old_trial = copy.deepcopy(fixture());old_trial['trial'] = 99
        rows = [{'run_id': 'old', 'renderer': {'pixel_probe': old_trial}},
                {'run_id': 'current', 'renderer': {'pixel_probe': {'trial': 1, 'state': 'running',
                    'updated_monotonic_ns': '998', 'blocks': []}}}]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'capture.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('game-start-checkpoint.json', json.dumps(session))
                archive.writestr('game-performance.jsonl', '\n'.join(json.dumps(r) for r in rows))
            loaded, samples, trial = load(path)
            self.assertEqual(loaded['run_id'], 'current')
            self.assertEqual(len(samples), 1)
            self.assertEqual(trial['state'], 'complete')
            self.assertEqual(trial['trial'], 1)
            self.assertFalse(analyze(loaded, samples, trial)['measurement_errors'])


if __name__ == '__main__':
    unittest.main()
