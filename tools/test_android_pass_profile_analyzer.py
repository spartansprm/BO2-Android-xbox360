import copy
import unittest
import json
import tempfile
import zipfile
from pathlib import Path
from analyze_android_draw_batch import load
from analyze_android_pass_profile import analyze


class ProfileAnalyzerTest(unittest.TestCase):
    def sample(self):
        profile=dict(schema=1,enabled=True,supported=True,sampled_jobs=10,baseline_jobs=630,
            sample_period_jobs=64,warmup_seconds=10,sampled_gpu_ns=500_000_000,
            baseline_gpu_ns=31_500_000_000,pass_gpu_ns=400_000_000,
            other_tracked_pass_gpu_ns=0,untracked_group_gpu_ns=0,
            categories=[dict(id=2,name='scene_with_depth',gpu_ns=500_000_000)],
            top_passes=[dict(target_hash='abcd',ordinal=0,category=2,width=1280,height=720,
                samples=10,gpu_ns=400_000_000,logical_draws=10000,
                shader_pairs=[dict(vs=1,ps=2,logical_draws=9000)])])
        return dict(elapsed_ms=120_000,renderer=dict(gpu_pass_profile=profile))

    def test_ranking_and_scope(self):
        r=analyze({},[self.sample()]);self.assertTrue(r['available'])
        self.assertEqual(r['sampled_gpu_ms_per_job'],50)
        self.assertEqual(r['ranked_passes'][0]['gpu_ms_per_occurrence'],40)
        self.assertEqual(r['pass_coverage_percent'],80)
        self.assertFalse(r['ranked_passes'][0]['shader_gpu_time_individually_measured'])
        self.assertFalse(r['physical_fps_improvement_proven'])

    def test_legacy_capture(self):
        self.assertFalse(analyze({},[dict(renderer={})])['available'])

    def test_no_samples_or_unsupported(self):
        for key,value in [('sampled_jobs',0),('supported',False),('enabled',False)]:
            s=self.sample();s['renderer']['gpu_pass_profile'][key]=value
            self.assertFalse(analyze({},[s])['available'])

    def test_inconsistent_accounting(self):
        s=self.sample();s['renderer']['gpu_pass_profile']['pass_gpu_ns']+=1
        self.assertFalse(analyze({},[s])['available'])

    def test_cached_samples_not_double_counted(self):
        s=self.sample();r=analyze({},[s,copy.deepcopy(s),copy.deepcopy(s)])
        self.assertEqual(r['sampled_jobs'],10)

    def test_marker_effect_warning(self):
        s=self.sample();p=s['renderer']['gpu_pass_profile'];p['baseline_gpu_ns']/=2
        r=analyze({},[s]);self.assertTrue(r['warnings']);self.assertFalse(r['overhead_causally_measured'])

    def test_saturation_and_truncation_warn(self):
        s=self.sample();p=s['renderer']['gpu_pass_profile'];p['untracked_group_gpu_ns']=1
        p['top_passes'][0]['gpu_ns']-=1;p['truncated_jobs']=1
        r=analyze({},[s]);self.assertTrue(r['available']);self.assertGreaterEqual(len(r['warnings']),2)

    def test_rotated_zip_and_session_filter(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'capture.zip'
            session=dict(run_id='current',app_version='0.29-gpu-pass-profile')
            current=self.sample();current['run_id']='current'
            old=copy.deepcopy(current);old['run_id']='previous'
            with zipfile.ZipFile(path,'w') as z:
                z.writestr('game-start-checkpoint.json',json.dumps(session))
                z.writestr('game-performance.previous.jsonl',json.dumps(old)+'\n'+json.dumps(current)+'\n')
                z.writestr('game-performance.jsonl','')
            s,rows=load(path)
            self.assertEqual(len(rows),1);self.assertTrue(analyze(s,rows)['available'])


if __name__=='__main__':unittest.main()
