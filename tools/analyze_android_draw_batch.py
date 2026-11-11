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
            samples = []
            for name in ['game-performance.previous.jsonl', 'game-performance.jsonl']:
                if name in archive.namelist():
                    samples += [json.loads(s) for s in archive.read(name).decode().splitlines() if s.strip()]
    else:
        report = json.loads(Path(path).read_text())
        session = report.get('game_session', report)
        samples = session.get('performance_capture', {}).get('samples', [])
    run = session.get('run_id')
    if not run:
        raise ValueError('Missing session run_id')
    return session, [s for s in samples if s.get('run_id') == run]


def usable(sample):
    renderer = sample.get('renderer', {})
    batch = renderer.get('draw_batch', {})
    probe = renderer.get('pixel_probe', {})
    return (sample.get('foreground') is True and renderer.get('client_in_game') is True
            and batch.get('available') is True and batch.get('recorder_active') is True
            and probe.get('state', 'idle') in ('idle', 'complete', 'cancelled'))


def analyze(session, samples):
    totals = dict(requested=0, emitted=0, merged=0, constants_reused=0, eligible=0)
    keys = dict(requested='requested_total', emitted='emitted_total', merged='merged_total',
                constants_reused='constant_uploads_reused_total', eligible='eligible_total')
    errors, warnings, modes = [], [], set()
    seconds, swaps, intervals = 0., 0, 0
    samples = sorted(samples, key=lambda s: s.get('elapsed_ms', 0))
    for a, b in zip(samples, samples[1:]):
        if not usable(a) or not usable(b):
            continue
        ar, br = a['renderer'], b['renderer']
        x, y = ar['draw_batch'], br['draw_batch']
        if x.get('enabled') != y.get('enabled'):
            continue
        delta = {k: y[v] - x[v] for k, v in keys.items()}
        dt = (b['elapsed_ms'] - a['elapsed_ms']) / 1000
        dn = br['native_swaps_total'] - ar['native_swaps_total']
        if dt <= 0 or dn <= 0 or delta['requested'] <= 0:
            continue
        if min(delta.values()) < 0 or delta['requested'] != delta['emitted'] + delta['merged']:
            errors.append('Counter reset or inconsistent draw accounting; interval excluded')
            continue
        if dt > 15:
            warnings.append('Sampling gap longer than 15 seconds; interval excluded')
            continue
        modes.add(bool(y['enabled']))
        for k in totals:
            totals[k] += delta[k]
        seconds += dt
        swaps += dn
        intervals += 1
    if len(modes) > 1:
        errors.append('Mixed enabled/disabled sessions must be analyzed separately')
    if not intervals:
        errors.append('No adjacent foreground gameplay samples with valid 0.28 draw counters')
    result = dict(run_id=session.get('run_id'), app_version=session.get('app_version'),
                  draw_batch_enabled=next(iter(modes)) if len(modes) == 1 else None,
                  valid_intervals=intervals, seconds=seconds, native_swaps=swaps, totals=totals,
                  errors=errors, warnings=list(dict.fromkeys(warnings)),
                  scope='Deferred job-buffer Vulkan draws; sampled window boundaries; swaps are not displayed FPS',
                  physical_fps_improvement_proven=False)
    if intervals and not errors:
        result.update(reduction_percent=totals['merged'] * 100 / totals['requested'],
                      requested_draws_per_native_swap=totals['requested'] / swaps,
                      emitted_draws_per_native_swap=totals['emitted'] / swaps,
                      merged_draws_per_native_swap=totals['merged'] / swaps,
                      internal_swaps_per_second=swaps / seconds)
        if not totals['merged']:
            result['warnings'].append('No draw coalescing observed; a lower call count has not been demonstrated')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    session, samples = load(args.capture)
    print(json.dumps(analyze(session, samples), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
