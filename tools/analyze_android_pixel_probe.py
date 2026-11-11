#!/usr/bin/env python3

import argparse
import json
from pathlib import Path
import zipfile


def load(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None:
                raise ValueError('ZIP CRC failure')
            session = json.loads(archive.read('game-start-checkpoint.json'))
            lines = archive.read('game-performance.jsonl').decode().splitlines()
            samples = [json.loads(line) for line in lines if line.strip()]
    else:
        report = json.loads(path.read_text())
        session = report.get('game_session', report)
        samples = session.get('performance_capture', {}).get('samples', [])
    run = session.get('run_id')
    samples = [s for s in samples if s.get('run_id') == run]
    candidates = [session.get('pixel_probe', {})]
    candidates += [s.get('renderer', {}).get('pixel_probe', {}) for s in samples]
    candidates = [c for c in candidates if c.get('trial', 0)]
    if not candidates:
        raise ValueError('No manually started pixel-cost trial in this session')
    latest_trial = max(c['trial'] for c in candidates)


    trial = max((c for c in candidates if c['trial'] == latest_trial),
                key=lambda c: (int(c.get('updated_monotonic_ns', 0)),
                               c.get('state') == 'complete',
                               sum(b.get('gpu_jobs', 0) for b in c.get('blocks', []))))
    return session, samples, trial


def metrics(blocks, process_clock=True, ring_clock=True):
    keys = ['frames', 'frame_wall_ns', 'process_cpu_ns', 'ring_cpu_ns', 'draws', 'scene_draws',
            'triangles_approx', 'scene_triangles_approx', 'clipped_draws', 'gpu_jobs', 'gpu_ns',
            'gpu_pending', 'gpu_failed', 'gpu_jobs_excluded', 'failed_presents']
    sums = {k: sum(b.get(k, 0) for b in blocks) for k in keys}
    n, wall, jobs = sums['frames'], sums['frame_wall_ns'], sums['gpu_jobs']
    return {**sums,
            'frame_period_ms': wall / n / 1e6 if n else None,
            'internal_swaps_per_second_not_display_fps': n * 1e9 / wall if wall else None,
            'gpu_ms_per_measured_job': sums['gpu_ns'] / jobs / 1e6 if jobs else None,
            'gpu_jobs_per_frame': jobs / n if n else None,
            'process_cpu_percent_one_core': sums['process_cpu_ns'] * 100 / wall if wall and process_clock else None,
            'ring_cpu_percent_one_core': sums['ring_cpu_ns'] * 100 / wall if wall and ring_clock else None,
            'draws_per_frame': sums['draws'] / n if n else None,
            'scene_draws_per_frame': sums['scene_draws'] / n if n else None,
            'scene_triangles_per_frame_approx': sums['scene_triangles_approx'] / n if n else None,
            'clipped_scene_draw_fraction': sums['clipped_draws'] / sums['scene_draws'] if sums['scene_draws'] else None}


def analyze(session, samples, trial):
    errors, warnings = [], []
    if trial.get('kind', 'scissor') != 'scissor':
        errors.append('This is a targeted shader trial; use analyze_android_shader_probe.py')
    blocks = trial.get('blocks', [])
    if trial.get('state') != 'complete':
        errors.append('Trial did not complete: ' + str(trial.get('state')))
    if len(blocks) != 4 or [b.get('phase') for b in blocks] != [1, 2, 3, 4]:
        raise ValueError('Four ordered ABBA blocks are required')
    if [b.get('mode') for b in blocks] != ['normal', 'scissor_1x1', 'scissor_1x1', 'normal']:
        errors.append('Unexpected ABBA modes')
    proc = trial.get('process_cpu_clock_available', False)
    ring = trial.get('ring_cpu_clock_available', False)
    measured = [metrics([b], proc, ring) for b in blocks]
    world_missing = []
    for index, b in enumerate(measured, 1):
        if b['frames'] < 30 or b['frame_wall_ns'] < 20e9:
            errors.append(f'Block {index}: too little settled frame data')
        if b['gpu_pending'] or b['gpu_failed'] or not b['gpu_jobs'] or not b['gpu_ns']:
            errors.append(f'Block {index}: GPU timings incomplete or unavailable')
        if b['failed_presents']:
            warnings.append(f'Block {index}: failed presentations')
        if b['scene_draws_per_frame'] is None or b['scene_draws_per_frame'] < 50:
            warnings.append(f'Block {index}: few scene draws; verify active gameplay and correct target group')
        if not b['scene_draws'] or not b['scene_triangles_approx']:
            world_missing.append(index)
            errors.append(f'Block {index}: no geometry in the world target group; exclude from a gameplay comparison')
        if b['gpu_jobs_per_frame'] is not None and not 0.95 <= b['gpu_jobs_per_frame'] <= 1.05:
            warnings.append(f'Block {index}: GPU jobs and frames have different coverage; compare per-job timings cautiously')
        fraction = b['clipped_scene_draw_fraction']
        if index in (1, 4) and b['clipped_draws']:
            errors.append(f'Block {index}: unexpected clipping in normal mode')
        if index in (2, 3) and (fraction is None or fraction < 0.8):
            errors.append(f'Block {index}: scene clipping did not affect enough draws')
    a = metrics([blocks[0], blocks[3]], proc, ring)
    b = metrics([blocks[1], blocks[2]], proc, ring)
    for key in ['scene_draws_per_frame', 'scene_triangles_per_frame_approx']:
        if a[key] and b[key] is not None and abs(b[key] / a[key] - 1) > .1:
            warnings.append(f'{key}: geometry differs by more than 10%; camera, simulation or visibility feedback may confound the result')
    for left, right, label in [(0, 3, 'normal'), (1, 2, 'clipped')]:
        first, last = measured[left]['frame_period_ms'], measured[right]['frame_period_ms']
        if first and last and abs(last / first - 1) > .15:
            warnings.append(f'{label}: frame period differs by more than 15% between repeated blocks')


    full_comparison_available = not errors
    cadence_gain = a['frame_period_ms'] / b['frame_period_ms'] if full_comparison_available and a['frame_period_ms'] and b['frame_period_ms'] else None
    gpu_reduction = 1 - b['gpu_ms_per_measured_job'] / a['gpu_ms_per_measured_job'] if full_comparison_available and a['gpu_ms_per_measured_job'] and b['gpu_ms_per_measured_job'] is not None else None
    partial_pairs = []
    for normal_index, clipped_index in [(0, 1), (3, 2)]:
        normal, clipped = measured[normal_index], measured[clipped_index]
        pair = [normal, clipped]
        if any(m['frames'] < 30 or m['frame_wall_ns'] < 20e9 or not m['gpu_jobs'] or not m['gpu_ns'] or
               m['gpu_pending'] or m['gpu_failed'] or not m['scene_draws'] or
               not m['scene_triangles_approx'] for m in pair):
            continue
        if normal['clipped_draws'] or clipped['clipped_scene_draw_fraction'] < .8:
            continue
        geometry = {key: clipped[key] / normal[key] - 1 for key in
                    ['scene_draws_per_frame', 'scene_triangles_per_frame_approx']}
        if any(abs(value) > .1 for value in geometry.values()):
            continue
        partial_pairs.append({'normal_phase': normal_index + 1, 'clipped_phase': clipped_index + 1,
            'status': 'provisional_only', 'geometry_relative_changes': geometry,
            'cadence_ratio_clipped_over_normal': normal['frame_period_ms'] / clipped['frame_period_ms'],
            'gpu_interval_fraction_reduced': 1 - clipped['gpu_ms_per_measured_job'] / normal['gpu_ms_per_measured_job'],
            'limits': 'Geometry counts are similar but do not prove equal cameras, materials or thermal conditions; this is not a complete repeated ABBA result.'})
    if errors:
        direction = 'Insufficient evidence; repair or repeat the trial before attributing a bottleneck.'
    elif warnings:
        direction = 'Provisional comparison: inspect workload, repeatability and timing coverage warnings before attributing the difference to pixel cost.'
    elif cadence_gain and cadence_gain >= 1.2 and gpu_reduction is not None and gpu_reduction >= .2:
        direction = 'Pixel-dependent rendering is a promising optimization target; this does not separate fragment ALU, texture bandwidth, overdraw or tile storage.'
    else:
        direction = 'The pixel reduction did not produce a large consistent gain; investigate remaining copies, clears, geometry, presentation and CPU/GPU scheduling without declaring a CPU bottleneck.'
    return {'schema': 1, 'run_id': session.get('run_id'), 'app_version': session.get('app_version'),
            'trial': trial['trial'], 'state': trial['state'], 'measurement_errors': errors,
            'comparison_warnings': warnings, 'full_comparison_available': full_comparison_available,
            'excluded_world_missing_blocks': world_missing,
            'normal': a if full_comparison_available else None,
            'scissor_1x1': b if full_comparison_available else None,
            'provisional_adjacent_pairs': partial_pairs,
            'blocks': [{'phase': i + 1, 'mode': blocks[i]['mode'], **m} for i, m in enumerate(measured)],
            'cadence_ratio_clipped_over_normal': cadence_gain,
            'gpu_interval_fraction_reduced': gpu_reduction, 'investigation_direction': direction,
            'sampled_android_thermal_status_by_phase': {
                str(i): sorted({s['thermal_status'] for s in samples if s.get('thermal_status') is not None
                    and s.get('renderer', {}).get('pixel_probe', {}).get('trial') == trial['trial']
                    and s.get('renderer', {}).get('pixel_probe', {}).get('phase') == i}) for i in range(1, 5)},
            'limits': ['This experiment intentionally breaks the picture; its FPS are not playable-game performance.',
                       'GPU elapsed timestamps are not utilization and internal swaps are not physical display FPS.',
                       'ABBA reduces a linear time trend but does not eliminate temperature, simulation or camera changes.',
                       'Scissoring can change occlusion results and subsequent game draws; inspect geometry counters.',
                       'The experiment preserves whole-target copies, clears, allocations and most vertex/draw-call work.',
                       'Counts and clocks have different boundaries; GPU timings are grouped by submitted job tags.',
                       'CPU percentages describe process/thread CPU time, not equal fractions of the heterogeneous chip.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(*load(args.capture))
    text = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')
    return 2 if result['measurement_errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
