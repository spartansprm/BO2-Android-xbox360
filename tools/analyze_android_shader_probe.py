#!/usr/bin/env python3

import argparse
import json
from pathlib import Path
from analyze_android_pixel_probe import load


FIELDS = ['frames', 'frame_wall_ns', 'draws', 'triangles_approx', 'target_passes', 'target_draws',
          'target_elements', 'eligible_draws', 'replaced_draws', 'replaced_elements', 'flat_fallbacks',
          'normal_pipeline_misses', 'flat_compile_ns', 'gpu_jobs', 'gpu_ns', 'gpu_pending', 'gpu_failed',
          'gpu_jobs_excluded', 'target_gpu_jobs', 'target_gpu_ns', 'target_gpu_failed', 'jobs_without_target']


def metrics(blocks):
    s = {key: sum(b.get(key, 0) for b in blocks) for key in FIELDS}
    frames, jobs, target_jobs = s['frames'], s['gpu_jobs'], s['target_gpu_jobs']
    s.update(target_ms_per_measured_occurrence=s['target_gpu_ns']/target_jobs/1e6 if target_jobs else None,
             whole_job_ms=s['gpu_ns']/jobs/1e6 if jobs else None,
             target_draws_per_frame=s['target_draws']/frames if frames else None,
             target_elements_per_frame=s['target_elements']/frames if frames else None,
             logical_draws_per_frame=s['draws']/frames if frames else None,
             eligible_fraction=s['eligible_draws']/s['target_draws'] if s['target_draws'] else None,
             replaced_fraction_of_eligible=s['replaced_draws']/s['eligible_draws'] if s['eligible_draws'] else None,
             internal_swaps_per_second=frames*1e9/s['frame_wall_ns'] if s['frame_wall_ns'] else None)
    return s


def analyze(session, samples, trial):
    errors, warnings = [], []
    if trial.get('kind') != 'target_shader':
        return dict(available=False, errors=['Not a targeted shader trial; do not interpret a scissor/pass profile as this test'])
    if trial.get('state') != 'complete':
        errors.append('Trial did not complete')
    blocks = trial.get('blocks', [])
    if len(blocks) != 4 or [b.get('phase') for b in blocks] != [1, 2, 3, 4]:
        return dict(available=False, errors=['Missing ABBA blocks'])
    expected_modes = ['normal', 'flat_fragment', 'flat_fragment', 'normal']
    for i, block in enumerate(blocks):
        label = f'Block {i+1}'
        if block.get('mode') != expected_modes[i]:
            errors.append(label+': incorrect mode')
        if not block.get('frames') or not block.get('target_gpu_jobs') or not block.get('target_draws'):
            errors.append(label+': no measured target workload')
        if block.get('gpu_pending') or block.get('gpu_failed') or block.get('target_gpu_failed'):
            errors.append(label+': incomplete or invalid GPU timestamps')
        if block.get('normal_pipeline_misses'):
            errors.append(label+': original draws skipped while pipelines compiled')
        if block.get('flat_compile_ns'):
            errors.append(label+': diagnostic pipeline compilation inside measured frames')
        if block.get('clipped_draws'):
            errors.append(label+': scissor clipping contaminated the shader trial')
        m=metrics([block])
        if i in (0, 3) and block.get('replaced_draws'):
            errors.append(label+': shader substitution leaked into normal mode')
        if i in (1, 2) and (m['replaced_fraction_of_eligible'] is None or m['replaced_fraction_of_eligible'] < .99):
            errors.append(label+': fewer than 99% of eligible draws used the flat module')
        if block.get('flat_fallbacks'):
            warnings.append(label+': original shader fallback occurred')
        if m['eligible_fraction'] is not None and m['eligible_fraction'] < .5:
            warnings.append(label+': less than half of target draws were eligible; weak shading coverage')
    a, b = metrics([blocks[0], blocks[3]]), metrics([blocks[1], blocks[2]])
    for key in ['target_draws_per_frame', 'target_elements_per_frame', 'logical_draws_per_frame']:
        if a[key] and b[key] is not None and abs(b[key]/a[key]-1)>.1:
            errors.append(key+': workload differs by over 10% between normal and flat phases')
    if a['eligible_fraction'] and b['eligible_fraction'] is not None and abs(b['eligible_fraction']/a['eligible_fraction']-1)>.1:
        errors.append('Eligible shader mix changed by over 10%')
    for left, right in [(0, 3), (1, 2)]:
        x, y = metrics([blocks[left]]), metrics([blocks[right]])
        if x['target_ms_per_measured_occurrence'] and y['target_ms_per_measured_occurrence'] is not None and abs(y['target_ms_per_measured_occurrence']/x['target_ms_per_measured_occurrence']-1)>.15:
            warnings.append('Repeated target-pass timings differ by over 15%; scene or thermal drift may confound the result')
    result = dict(available=not errors, comparison_reliable=not errors and not warnings, run_id=session.get('run_id'), version=session.get('app_version'),
                  trial=trial.get('trial'), normal=a, flat_fragment=b, errors=errors, warnings=warnings,
                  scope='Selected pass and eligible opaque shaders only; VS varying/output work can also change',
                  textures_vs_arithmetic_individually_isolated=False,
                  playable_fps_improvement_proven=False)
    if not errors and a['target_ms_per_measured_occurrence']:
        saved=a['target_ms_per_measured_occurrence']-b['target_ms_per_measured_occurrence']
        result.update(target_pass_ms_saved=saved, target_pass_reduction_percent=100*saved/a['target_ms_per_measured_occurrence'],
                      whole_job_ms_difference=a['whole_job_ms']-b['whole_job_ms'] if a['whole_job_ms'] is not None and b['whole_job_ms'] is not None else None)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('capture',type=Path);args=parser.parse_args()
    try:
        session,samples,trial=load(args.capture);result=analyze(session,samples,trial)
    except (ValueError,KeyError) as error:
        result=dict(available=False,errors=[str(error)])
    print(json.dumps(result,ensure_ascii=False,indent=2))


if __name__=='__main__':main()
