import copy
import unittest
from analyze_android_shader_probe import analyze


class ShaderProbeTest(unittest.TestCase):
    def trial(self):
        blocks=[]
        for i in range(4):
            flat=i in (1,2)
            blocks.append(dict(phase=i+1,mode='flat_fragment' if flat else 'normal',frames=100,
                frame_wall_ns=5_000_000_000,draws=200000,triangles_approx=20000000,
                target_passes=100,target_draws=100000,target_elements=30000000,eligible_draws=80000,
                replaced_draws=80000 if flat else 0,replaced_elements=24000000 if flat else 0,
                gpu_jobs=100,gpu_ns=4_100_000_000 if flat else 5_000_000_000,
                target_gpu_jobs=100,target_gpu_ns=700_000_000 if flat else 1_600_000_000))
        return dict(kind='target_shader',state='complete',trial=1,blocks=blocks)

    def test_effect_is_diagnostic(self):
        r=analyze({},[],self.trial());self.assertTrue(r['available'])
        self.assertEqual(r['target_pass_ms_saved'],9);self.assertFalse(r['playable_fps_improvement_proven'])
        self.assertFalse(r['textures_vs_arithmetic_individually_isolated'])

    def test_kind_and_incomplete(self):
        t=self.trial();t['kind']='scissor';self.assertFalse(analyze({},[],t)['available'])
        t=self.trial();t['state']='running';self.assertFalse(analyze({},[],t)['available'])

    def test_skips_compile_bad_gpu_or_clipping(self):
        for key in ['normal_pipeline_misses','flat_compile_ns','gpu_pending','target_gpu_failed','clipped_draws']:
            t=self.trial();t['blocks'][1][key]=1;self.assertFalse(analyze({},[],t)['available'],key)

    def test_coverage_and_normal_restore(self):
        t=self.trial();t['blocks'][1]['replaced_draws']=100;self.assertFalse(analyze({},[],t)['available'])
        t=self.trial();t['blocks'][3]['replaced_draws']=100;self.assertFalse(analyze({},[],t)['available'])

    def test_geometry_drift(self):
        t=self.trial();t['blocks'][1]['target_elements']*=2;self.assertFalse(analyze({},[],t)['available'])

    def test_low_coverage_and_timing_drift_warn(self):
        t=self.trial()
        for b in t['blocks']:b['target_draws']*=3
        t['blocks'][3]['target_gpu_ns']*=2
        r=analyze({},[],t);self.assertTrue(r['available']);self.assertTrue(r['warnings'])


if __name__=='__main__':unittest.main()
