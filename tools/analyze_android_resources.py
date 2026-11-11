#!/usr/bin/env python3

import argparse
import json
from collections import defaultdict


def summarize(report):
    session = report.get('game_session', report)
    capture = session.get('performance_capture', {})
    run = session.get('run_id', capture.get('run_id'))
    rows = [r for r in capture.get('samples', []) if r.get('run_id') == run]
    threads = defaultdict(lambda: {'weighted': 0., 'ms': 0., 'peak': 0., 'cpus': set()})
    windows = {}
    for row in rows:
        for t in row.get('threads', []):
            value, duration = t.get('cpu_percent_one_core'), t.get('cpu_interval_ms')
            if isinstance(value, (int, float)) and isinstance(duration, (int, float)) and duration > 0:
                item = threads[(t['tid'], t.get('start_time_ticks'), t['name'])]
                item['weighted'] += value * duration
                item['ms'] += duration
                item['peak'] = max(item['peak'], value)
                item['cpus'].add(t.get('last_cpu'))
        gpu = row.get('renderer', {})
        if gpu.get('gpu_window_id') not in (None, '0') and gpu.get('gpu_window_swaps', 0) > 0:
            windows[gpu['gpu_window_id']] = gpu

    def observed(path):
        result = []
        for row in rows:
            value = row
            for key in path.split('.'):
                value = value.get(key) if isinstance(value, dict) else None
            if isinstance(value, (float, int)) and not isinstance(value, bool):
                result.append(value)
        return {'available_samples': len(result), 'minimum': min(result) if result else None,
                'maximum': max(result) if result else None}

    busiest = sorted(({'tid': tid, 'start_time_ticks': start, 'name': name, 'mean_cpu_percent_one_core': d['weighted']/d['ms'],
                       'peak_cpu_percent_one_core': d['peak'], 'observed_cpu_ids': sorted(c for c in d['cpus'] if c is not None)}
                      for (tid, start, name), d in threads.items()), key=lambda d: -d['mean_cpu_percent_one_core'])
    return {'run_id': run, 'samples': len(rows), 'top_threads': busiest[:16],
            'process_cpu_percent_one_core': observed('process_cpu_percent_one_core'),
            'rss_kib': observed('rss_kib'), 'pss_kib': observed('memory_details.pss_kib'),
            'system_available_bytes': observed('memory_details.system_available_bytes'),
            'thermal_status': observed('thermal_status'), 'battery_temperature_c': observed('battery_temperature_c'),
            'gpu_windows': list(windows.values()),
            'notes': ['CPU 100% means one logical CPU. CPU IDs are sampled, not residency measurements.',
                      'GPU windows are deduplicated; Vulkan job time is not a hardware utilization percentage.',
                      'PSS is sampled every 30 seconds; repeated cached values are not independent measurements.',
                      'Capture can include loading/background/menu intervals. Compare matching map and foreground intervals.',
                      'No automatic CPU/GPU/RAM bottleneck conclusion is drawn from these values.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report')
    args = parser.parse_args()
    with open(args.report, encoding='utf-8') as stream:
        value = json.load(stream)
    print(json.dumps(summarize(value), indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
