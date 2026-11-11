#!/usr/bin/env python3

import argparse
import json
from pathlib import Path
from analyze_android_draw_batch import load


def analyze(session, samples):
    candidates = [s for s in samples if s.get('renderer', {}).get('gpu_pass_profile', {}).get('schema') == 1]
    if not candidates:
        return dict(available=False, run_id=session.get('run_id'), errors=['No 0.29 GPU pass profile in this session'])
    latest = max(candidates, key=lambda s: (
        sum(s['renderer']['gpu_pass_profile'].get(k, 0) for k in
            ['sampled_jobs', 'baseline_jobs', 'failed_jobs', 'discarded_jobs']), s.get('elapsed_ms', 0)))
    p = latest['renderer']['gpu_pass_profile']
    errors, warnings = [], []
    n, baseline = p.get('sampled_jobs', 0), p.get('baseline_jobs', 0)
    total, covered = p.get('sampled_gpu_ns', 0), p.get('pass_gpu_ns', 0)
    if not p.get('enabled'):
        errors.append('Sampled pass profiling disabled for this execution')
    if not p.get('supported'):
        errors.append('Vulkan GPU timestamp queries unavailable')
    if not n or not total:
        errors.append('No valid sampled gameplay jobs; allow warmup and sampling time')
    if covered > total or min(n, baseline, total, covered) < 0:
        errors.append('Invalid GPU time accounting')
    categories = p.get('categories', [])
    if sum(c.get('gpu_ns', 0) for c in categories) > total:
        errors.append('Category intervals exceed sampled job time')
    passes = p.get('top_passes', [])
    listed = sum(g.get('gpu_ns', 0) for g in passes)
    other = p.get('other_tracked_pass_gpu_ns', 0)
    unknown = p.get('untracked_group_gpu_ns', 0)
    if listed + other + unknown != covered:
        errors.append('Listed/other/untracked pass accounting mismatch')
    if n < 8:
        warnings.append('Fewer than eight sampled jobs; ranking is preliminary')
    if p.get('truncated_jobs', 0):
        warnings.append('Timestamp or metadata budget reached in some jobs; inspect outside-pass time')
    if p.get('failed_jobs', 0):
        warnings.append('Some GPU query results were rejected')
    if unknown:
        warnings.append('Pass-group table saturated; some pass time has no retained group identity')
    if any(g.get('untracked_pair_draws', 0) for g in passes):
        warnings.append('Some shader pairs were not retained; listed shader counts are incomplete')
    sampled_mean = total / n / 1e6 if n else None
    baseline_mean = p.get('baseline_gpu_ns', 0) / baseline / 1e6 if baseline else None
    observed_difference = ((sampled_mean / baseline_mean - 1) * 100
                           if sampled_mean is not None and baseline_mean else None)
    if observed_difference is not None and abs(observed_difference) > 20:
        warnings.append('Sampled and baseline job times differ by over 20%; marker effects and workload variation may bias attribution')
    rankings = []
    for g in passes:
        count = g.get('samples', 0)
        if count <= 0:
            errors.append('Pass entry has no valid samples')
            continue
        rankings.append(dict(target_hash=g.get('target_hash'), ordinal=g.get('ordinal'),
            category=g.get('category'), width=g.get('width'), height=g.get('height'), samples=count,
            gpu_ms_per_occurrence=g['gpu_ns'] / count / 1e6,
            gpu_ms_per_sampled_job=g['gpu_ns'] / n / 1e6 if n else None,
            share_of_sampled_job_time_percent=g['gpu_ns'] * 100 / total if total else None,
            logical_draws_per_occurrence=g.get('logical_draws', 0) / count,
            shader_pairs_by_logical_draws=g.get('shader_pairs', []),
            shader_gpu_time_individually_measured=False))
    rankings.sort(key=lambda g: g['gpu_ms_per_sampled_job'] or 0, reverse=True)
    return dict(available=not errors, run_id=session.get('run_id'), version=session.get('app_version'),
        sampled_jobs=n, baseline_jobs=baseline, sample_period_jobs=p.get('sample_period_jobs'),
        warmup_seconds=p.get('warmup_seconds'), sampled_gpu_ms_per_job=sampled_mean,
        baseline_gpu_ms_per_job=baseline_mean, observed_sampled_vs_baseline_difference_percent=observed_difference,
        overhead_causally_measured=False, pass_coverage_percent=covered * 100 / total if total else None,
        outside_pass_gpu_ms_per_sampled_job=(total-covered) / n / 1e6 if n else None,
        categories=[dict(id=c['id'], name=c['name'], gpu_ms_per_sampled_job=c['gpu_ns'] / n / 1e6 if n else None)
                    for c in categories], ranked_passes=rankings,
        shader_scope=p.get('shader_scope'), timing_scope=p.get('timing_scope'),
        android_presenter_included=False, physical_fps_improvement_proven=False,
        errors=errors, warnings=warnings)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    session, samples = load(args.capture)
    print(json.dumps(analyze(session, samples), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
