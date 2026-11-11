#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import argparse
import ctypes
import ctypes.util
import json
from pathlib import Path
import re
import struct
import sys
from collections import Counter, defaultdict

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/inspect"))
from ff_shaders import find_shaders, parse_xfile

NAME = re.compile(r"^pimp_shader_(.+?)_([0-9a-f]{7,8})\.hlsl(?:\*[^/\\]*)?$", re.I)
MAX_DISTANCE = 16384
MAX_PAIRS = 100000


class Hash:
    def __init__(self, library=None):
        path = library or ctypes.util.find_library("xxhash")
        if not path:
            raise RuntimeError("libxxhash.so is required to match private shader IDs")
        self.lib = ctypes.CDLL(str(path))
        self.lib.XXH3_64bits.restype = ctypes.c_uint64
        self.lib.XXH3_64bits.argtypes = (ctypes.c_void_p, ctypes.c_size_t)

    def __call__(self, data):
        return self.lib.XXH3_64bits(ctypes.c_char_p(bytes(data)) if data else None, len(data))


def load_index(path, hash64):
    data = Path(path).read_bytes()
    if len(data) < 80 or data[:8] != b"BZMSIDX\0" or struct.unpack_from("<I", data, 8)[0] != 1:
        raise ValueError("not a valid BZMSIDX v1 shader index")
    n, = struct.unpack_from("<I", data, 12)
    nf, = struct.unpack_from("<I", data, 32)
    if not 1 <= n <= 65536 or len(data) != 80 + n * 72 + nf * 2:
        raise ValueError("invalid shader index length")
    checksum, = struct.unpack_from("<Q", data, 72)
    if hash64(data[80:]) != checksum:
        raise ValueError("shader index checksum failed")
    result = {}
    for i in range(n):
        off = 80 + i * 72
        digest, = struct.unpack_from("<Q", data, off)
        words, = struct.unpack_from("<I", data, off + 36)
        kind = "vs" if data[off + 68] & 1 else "ps"
        if digest in result:
            raise ValueError("ambiguous shader hash in index")
        result[digest] = (i + 1, kind, words)
    return result


def family(name):
    match = NAME.fullmatch(name or "")
    return match.group(1).lower() if match else None


def find_pairs(zone, index, hash64, *, distance=MAX_DISTANCE):
    if len(zone) < 40 or parse_xfile(zone)["size"] != len(zone) - 40:
        raise ValueError("invalid decompressed BO2 XFile")
    originals = sorted((r for r in find_shaders(zone) if r["ok"]), key=lambda r:r["record"])
    recognized = []
    unknown = unnamed = 0
    for row in originals:
        shader = index.get(hash64(row["blob"]))
        if shader is None or shader[1] != row["kind"] or not shader[2]:
            unknown += 1
            continue
        group = family(row.get("name"))
        if group is None:
            unnamed += 1
            continue
        recognized.append((row["record"], shader[0], shader[1], group))
    vertices = defaultdict(list)
    found = {}
    for offset, number, kind, group in recognized:
        if kind == "vs":
            vertices[group].append((offset, number))
        else:
            for before, vs in reversed(vertices[group]):
                gap = offset - before
                if gap > distance:
                    break
                if gap <= 0:
                    continue
                k = (vs, number)
                if k not in found or gap < found[k]["distance"]:
                    found[k] = {"vs": vs, "ps": number, "family": group, "distance": gap}
    return list(found.values()), {"containers":len(originals), "indexed_named":len(recognized),
                                  "unindexed":unknown, "unnamed":unnamed, "candidate_pairs":len(found)}


def known_pairs(path):
    from pipelines_list import read_list
    size, records = read_list(str(path))
    if size != 424:
        raise ValueError("unsupported pipeline record size")
    return {struct.unpack_from("<II", r, 0) for r in records}


class Catalog:
    def __init__(self, index, hash64, known=()):
        self.index = load_index(index, hash64)
        self.hash64 = hash64
        self.known = set(known)
        self.sources = {}
        self.entries = {}

    def add(self, name, zone):
        pairs, stats = find_pairs(zone, self.index, self.hash64)
        self.sources[name] = stats
        for pair in pairs:
            key = (pair["vs"], pair["ps"])
            if key not in self.entries:
                if len(self.entries) >= MAX_PAIRS:
                    raise ValueError("candidate limit reached; refusing truncation")
                self.entries[key] = {**pair, "known": key in self.known, "examples": []}
            entry = self.entries[key]
            if len(entry["examples"]) < 8 and name not in entry["examples"]:
                entry["examples"].append(name)
            entry["distance"] = min(entry["distance"], pair["distance"])

    def report(self):
        rows = sorted(self.entries.values(),key=lambda e:(e["vs"],e["ps"]))
        count = sum(x["known"] for x in rows)
        return {"schema":1,"status":"candidate_pairs_only",
                "safety":"Do not turn this catalog into Vulkan pipelines: exact MaterialTechnique links, "
                         "render formats, vertex input, specialization, blend, raster and depth are not recovered.",
                "indexed_shaders":len(self.index),"zones":len(self.sources),
                "candidate_pairs":len(rows),"already_observed_pairs":count,
                "unverified_pairs":len(rows)-count,
                "sources":self.sources,"pairs":rows}


def atomic_json(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".partial")
    temp.write_text(json.dumps(content,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    temp.replace(path)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--shader-index",type=Path,required=True)
    p.add_argument("--known-pipelines",type=Path)
    p.add_argument("--zone",type=Path,nargs="+")
    p.add_argument("--export-index",type=Path)
    p.add_argument("--batch-dir",type=Path)
    p.add_argument("--xex-image",type=Path)
    p.add_argument("--lzx-library",type=Path)
    p.add_argument("--xxhash-library")
    p.add_argument("--jobs",type=int,default=2)
    p.add_argument("--out",type=Path,required=True)
    a=p.parse_args(argv)
    if bool(a.zone)==bool(a.export_index):
        p.error("Use exactly one of --zone or --export-index")
    hash64=Hash(a.xxhash_library)
    cat=Catalog(a.shader_index,hash64,known_pairs(a.known_pipelines) if a.known_pipelines else ())
    if a.zone:
        for path in a.zone:
            cat.add(path.name,path.read_bytes())
        atomic_json(a.out,cat.report())
    else:
        if not (a.batch_dir and a.xex_image and a.lzx_library):
            p.error("Full export requires --batch-dir, --xex-image and --lzx-library")
        from process_android_zone_export import Extractor, Processor
        from game_files import zone_modes
        class ZoneCandidateExtractor(Extractor):
            def extract(self,path,meta,state):
                info,zone=self.decode(path,meta["id"])
                candidates,stats=find_pairs(zone,cat.index,cat.hash64)
                collection=state.setdefault("pipeline_candidate_catalog",{})
                for candidate in candidates:
                    k=f'{candidate["vs"]}:{candidate["ps"]}'
                    if k not in collection:
                        if len(collection)>=MAX_PAIRS:
                            raise ValueError("candidate limit reached")
                        collection[k]={**candidate,"zone_ids":[]}
                    sample=collection[k]["zone_ids"]
                    if len(sample)<8 and meta["id"] not in sample:
                        sample.append(meta["id"])
                return {"status":"extracted","decoded_bytes":len(zone),
                        "modes":sorted(zone_modes(path.stem.lower())),
                        "candidate_stats":stats,"containers":[],"unmatched":[]}
        if a.out.suffix:
            p.error("Full export requires --out DIRECTORY, not a JSON path")
        previous=(a.out/"extraction-state.json").exists()
        extr=ZoneCandidateExtractor(a.out,a.xex_image,a.lzx_library,a.jobs)
        try:
            proc=Processor(a.export_index,a.out,extr)
            if previous and proc.state.get("pipeline_candidate_schema") != 1:
                raise ValueError("Refusing to resume a journal from another extraction task")
            proc.state["pipeline_candidate_schema"]=1
            proc.checkpoint()
            while len(proc.state["batches"])<len(proc.batches):
                i=len(proc.state["batches"])
                proc.ingest(a.batch_dir/proc.batches[i]["file"])
            if proc.state.get("status")!="extracted" or proc.state["summary"]["failed_zones"]:
                raise ValueError("incomplete or failed fastfile scan")
            cat.sources={f'{row["origin"]}/{row["path"]}':row["candidate_stats"] for row in proc.state["files"].values()}
            for rec in proc.state["pipeline_candidate_catalog"].values():
                pair=(rec["vs"],rec["ps"])
                cat.entries[pair]={**rec,"known":pair in cat.known,"examples":rec["zone_ids"]}
            atomic_json(a.out/"pipeline-candidates.json",cat.report())
        finally:
            extr.close()
    r=cat.report()
    print(f'{r["zones"]} zones; {r["candidate_pairs"]} candidate VS/PS pairs; '
          f'{r["unverified_pairs"]} not observed in cache. NOT Vulkan pipelines.')


if __name__=="__main__":
    main()
