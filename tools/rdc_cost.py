import glob
import json
import os
import sys

import renderdoc as rd


def walk(actions):
    for a in actions:
        yield a
        yield from walk(a.children)


def main():
    files = sorted(glob.glob(os.environ['RDC_GLOB']), key=lambda p: int(p.split('frame')[-1].split('.')[0]))
    out = open(os.environ['RDC_OUT'], 'w')
    for path in files:
        cap = rd.OpenCaptureFile()
        if cap.OpenFile(path, '', None) != rd.ResultCode.Succeeded:
            out.write(json.dumps({'file': path, 'error': 'open'}) + '\n')
            continue
        res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
        if res != rd.ResultCode.Succeeded:
            out.write(json.dumps({'file': path, 'error': str(res)}) + '\n')
            cap.Shutdown()
            continue
        texs = {t.resourceId: (t.width, t.height) for t in ctl.GetTextures()}
        acts = list(walk(ctl.GetRootActions()))
        by_eid = {a.eventId: a for a in acts}
        draws = [a for a in acts if a.flags & rd.ActionFlags.Drawcall]
        fastest = {}
        for _ in range(7):
            for c in ctl.FetchCounters([rd.GPUCounter.EventGPUDuration]):
                fastest[c.eventId] = min(fastest.get(c.eventId, 1e9), c.value.d)
        total = 0.0
        per = {}
        ndraw = {}
        null = rd.ResourceId.Null()
        for eid, v in fastest.items():
            total += v
            a = by_eid.get(eid)
            key = 'other'
            if a is not None:
                colour = [o for o in a.outputs if o != null]
                rid = a.depthOut if a.depthOut != null else (colour[0] if colour else null)
                if rid in texs:
                    w, h = texs[rid]
                    key = '%dx%d%s' % (w, h, ' depth' if not colour else '')
                if a.flags & rd.ActionFlags.Drawcall:
                    ndraw[key] = ndraw.get(key, 0) + 1
            per[key] = per.get(key, 0.0) + v
        out.write(json.dumps({'file': os.path.basename(path), 'draws': len(draws), 'gpu_ms': total * 1000.0,
                              'per_ms': {k: round(v * 1000.0, 3) for k, v in sorted(per.items(), key=lambda kv: -kv[1])},
                              'draws_per': ndraw}) + '\n')
        out.flush()
        ctl.Shutdown()
        cap.Shutdown()
    out.close()


main()
sys.exit(0)
