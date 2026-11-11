import copy
import unittest
from analyze_android_draw_batch import analyze


class AnalyzerTest(unittest.TestCase):
    def samples(self):
        def sample(i):
            return dict(elapsed_ms=i*5000, foreground=True, renderer=dict(client_in_game=True,
                native_swaps_total=i*100, pixel_probe=dict(state='idle'), draw_batch=dict(available=True,
                recorder_active=True, enabled=True, requested_total=i*200000, emitted_total=i*150000,
                merged_total=i*50000, eligible_total=i*100000, constant_uploads_reused_total=i*250)))
        return [sample(i) for i in range(3)]

    def test_measured_savings(self):
        r=analyze({},self.samples())
        self.assertFalse(r['errors']);self.assertEqual(r['reduction_percent'],25)
        self.assertEqual(r['emitted_draws_per_native_swap'],1500)
        self.assertFalse(r['physical_fps_improvement_proven'])

    def test_exclude_pixel_trial_and_menus(self):
        for change in ('trial','menu','background','unavailable'):
            s=self.samples()
            for v in s:
                if change=='trial':v['renderer']['pixel_probe']['state']='running'
                if change=='menu':v['renderer']['client_in_game']=False
                if change=='background':v['foreground']=False
                if change=='unavailable':v['renderer']['draw_batch']['available']=False
            self.assertTrue(analyze({},s)['errors'])

    def test_bad_accounting(self):
        s=self.samples();s[-1]['renderer']['draw_batch']['merged_total']=1
        self.assertTrue(analyze({},s)['errors'])

    def test_old_version(self):
        s=self.samples()
        for v in s:del v['renderer']['draw_batch']
        self.assertTrue(analyze({},s)['errors'])

    def test_zero_savings_is_reported(self):
        s=self.samples()
        for v in s:
            b=v['renderer']['draw_batch'];b['emitted_total']=b['requested_total'];b['merged_total']=0
        r=analyze({},s);self.assertEqual(r['reduction_percent'],0);self.assertTrue(r['warnings'])


if __name__=='__main__':unittest.main()
