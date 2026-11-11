import argparse
import csv
import glob
import hashlib
import importlib.util
import json
import multiprocessing
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parent.parent
LANGUAGE = re.compile(r"^(bp|fj|fr|ge|it|ja|ms|po|ru|sp)_")
SHARED = {"frontend_patch", "patch", "weapon_attach_shared", "patch_ui_zm"}


def multiplayer_zone(name):
    stem, extension = os.path.splitext(name.lower())
    if extension != ".ff" or LANGUAGE.match(stem) or stem.startswith(("so_", "su_")):
        return False
    return stem in SHARED or bool(re.search(r"(^|_)mp(_|$)", stem))


def zones(data):
    return sorted(p for folder in ("game", "update", "content")
                  for p in (data / folder).rglob("*") if p.is_file() and multiplayer_zone(p.name))


def say(message):
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def pack_containers(path):
    pass
    result = []
    with open(path, "rb") as stream:
        header = stream.read(24)
        if len(header) != 24 or header[:8] != b"BZMSPV\0\0" or struct.unpack_from("<I", header, 8)[0] != 1:
            raise ValueError(f"invalid pack header: {path}")
        count = struct.unpack_from("<I", header, 12)[0]
        if not 1 <= count <= 32768:
            raise ValueError(f"invalid shader count: {path}")
        for _ in range(count):
            lengths = stream.read(16)
            if len(lengths) != 16:
                raise ValueError(f"truncated pack: {path}")
            size, words = struct.unpack_from("<II", lengths)
            if not 24 <= size <= 65536 or not 5 <= words <= 1048576:
                raise ValueError(f"invalid entry: {path}")
            blob = stream.read(size)
            if len(blob) != size:
                raise ValueError(f"invalid entry: {path}")
            result.append(hashlib.sha1(blob).hexdigest())
            stream.seek(words * 4, 1)
        if stream.tell() != os.path.getsize(path):
            raise ValueError(f"incorrect pack length: {path}")
    return result


def extract(found, data, out, image, jobs, cache):
    spec = importlib.util.spec_from_file_location("ff_shaders", ROOT / "tools/inspect/ff_shaders.py")
    ff = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = ff
    spec.loader.exec_module(ff)
    sys.path.insert(0, str(ROOT / "tools/inspect"))
    key, _ = ff.load_key(None, str(image))
    unique = out / "unique"
    unique.mkdir(exist_ok=True)
    manifest = {"zones": [], "shaders": []}
    with multiprocessing.Pool(max(jobs, ff.STREAM_COUNT)) as pool:
        for number, path in enumerate(found, 1):
            relative = path.relative_to(data).as_posix()
            say(f"extract {number}/{len(found)}: {relative}")
            cached = cache / (path.stem + ".zone") if cache else None
            stat = path.stat()
            stamp = f"{stat.st_size}:{int(stat.st_mtime)}"
            if (cached and cached.is_file() and Path(str(cached) + ".src").is_file()
                    and Path(str(cached) + ".src").read_text() == stamp):
                zone = cached.read_bytes()
            else:
                _, zone = ff.decode_zone(str(path), key, pool)
            if ff.parse_xfile(zone)["size"] != len(zone) - 0x28:
                raise ValueError(f"decompressed zone size mismatch: {relative}")
            records = ff.find_shaders(zone)
            unmatched = [{k: s.get(k) for k in ("container", "kind", "why")} for s in records if not s["ok"]]
            manifest["zones"].append({"path": relative, "containers": len(records), "unmatched": unmatched})
            for shader in records:
                if not shader["ok"]:
                    continue
                name = f"{shader['kind']}_{shader['sha1'][:16]}.bin"
                destination = unique / name
                if destination.exists():
                    if destination.read_bytes() != shader["blob"]:
                        raise ValueError(f"container filename collision: {name}")
                else:
                    destination.write_bytes(shader["blob"])
                manifest["shaders"].append({"file": name, "zone": relative, "name": shader["name"],
                                            "record": shader["record"], "container": shader["container"],
                                            "sha1": shader["sha1"]})
            (out / "extraction.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            say(f"  {len(records) - len(unmatched)} matched; {len(unmatched)} unmatched")
    return manifest


def compile_one(name, hlsl, spirv, dxc):
    stem = Path(name).stem
    target = spirv / (stem + ".spv")
    vertex = stem.startswith("v_")
    command = [str(dxc / "dxc.exe"), "-spirv", "-T", "vs_6_6" if vertex else "ps_6_6", "-E", "main",
               "-HV", "2021", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout"]
    if vertex:
        command.append("-fvk-invert-y")
    result = subprocess.run([*command, "-Fo", str(target), str(hlsl / name)], capture_output=True, text=True,
                            errors="replace")
    stage = "dxc"
    if result.returncode == 0:
        stage = "spirv-val"
        result = subprocess.run([str(dxc / "spirv-val.exe"), "--target-env", "vulkan1.2", "--scalar-block-layout",
                                 str(target)], capture_output=True, text=True, errors="replace")
    if result.returncode:
        target.unlink(missing_ok=True)
    return {"file": name, "status": stage if result.returncode else "ok",
            "detail": (result.stdout + result.stderr).strip()}


def audit(pack, tools, logs, out):
    coverage = out / "coverage.tsv"
    with coverage.open("w", encoding="utf-8") as stream:
        subprocess.run([str(tools / "bo2zm_pack.exe"), "--coverage", str(pack)], stdout=stream, check=True)
    with coverage.open(encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    identities = {}
    hashes = pack_containers(pack)
    provenance = {}
    manifest = out / "extraction.json"
    if manifest.is_file():
        for shader in json.loads(manifest.read_text(encoding="utf-8"))["shaders"]:
            provenance.setdefault(shader["file"], []).append(shader)
    for row in rows:
        key = (row["stage"], int(row["words"]), row["microcode_hash"])
        name = f"{row['stage']}_{hashes[int(row['index'])][:16]}.bin"
        identities.setdefault(key, []).append({"file": name, "index": int(row["index"]),
                                               "sources": provenance.get(name, [])})
    pixel_groups = {}
    for row in rows:
        if row["stage"] == "ps":
            pixel_groups.setdefault((row["words"], row["microcode_hash"]), []).append(row)
    repeated = [group for group in pixel_groups.values() if len(group) > 1]
    different = [group for group in repeated if len({row["spirv_hash"] for row in group}) > 1]
    summary = {"containers": len(rows), "vertex": sum(row["stage"] == "vs" for row in rows),
               "pixel": sum(row["stage"] == "ps" for row in rows), "repeated_pixel_microcode_groups": len(repeated),
               "pixel_groups_with_different_spirv": different}
    (out / "coverage_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    expression = re.compile(r"unidentified (pixel|vertex) shader: (\d+) words, hash ([0-9A-Fa-f]{16})")
    report = []
    for log in logs:
        misses = {}
        for number, line in enumerate(log.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            match = expression.search(line)
            if not match:
                continue
            kind, words, fingerprint = match.groups()
            key = ("ps" if kind == "pixel" else "vs", int(words), fingerprint.upper())
            misses.setdefault(key, number)
        for key, line in misses.items():
            report.append({"log": str(log), "line": line, "stage": key[0], "words": key[1], "hash": key[2],
                           "covered": key in identities, "comparison": "exact" if key[0] == "ps" else "unpatched only",
                           "containers": identities.get(key, [])})
        pixel = [key for key in misses if key[0] == "ps"]
        say(f"{log.name}: {sum(key in identities for key in pixel)}/{len(pixel)} logged pixel shader misses covered")
    (out / "log_coverage.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--tools", type=Path, required=True)
    ap.add_argument("--dxc", type=Path)
    ap.add_argument("--base-pack", type=Path)
    ap.add_argument("--xex-image", type=Path, default=ROOT / "work/dump_tu18/default_mp.image.bin")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--zone-cache", type=Path, help="optional read-only ff_shaders.py decompression cache")
    ap.add_argument("--log", type=Path, action="append", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-extract", action="store_true")
    ap.add_argument("--audit-only", type=Path, help="check an existing pack against --log without building")
    a = ap.parse_args()
    if a.jobs < 1:
        ap.error("--jobs must be positive")
    if a.data is None:
        located = subprocess.run(["bash", str(ROOT / "tools/data_dir.sh")], capture_output=True, text=True, check=True).stdout.strip()
        located = re.sub(r"^/([a-zA-Z])/", lambda m: m[1].upper() + ":/", located)
        a.data = Path(located)
    a.data = a.data.resolve()
    found = zones(a.data)
    if a.list:
        for path in found:
            print(path)
        print(f"{len(found)} zones")
        return
    out = (a.out or a.data / "shaders/multiplayer").resolve()
    if out == ROOT or ROOT in out.parents or any(out == a.data / d or a.data / d in out.parents for d in ("game", "update", "content")):
        ap.error("--out must be outside the source checkout and game folders")
    out.mkdir(parents=True, exist_ok=True)
    if a.audit_only:
        audit(a.audit_only, a.tools, a.log, out)
        return
    pack = out / "bo2zm_shaders.spvpack"
    if pack.exists():
        ap.error(f"output pack exists; choose a new --out: {pack}")
    dxc = a.dxc or Path((sorted(glob.glob("C:/VulkanSDK/*/Bin")) or [""])[-1])
    for executable in (a.tools / "bo2zm_hlsl.exe", a.tools / "bo2zm_pack.exe", dxc / "dxc.exe", dxc / "spirv-val.exe"):
        if not executable.is_file():
            ap.error(f"missing tool: {executable}")
    if not a.no_extract:
        extract(found, a.data, out, a.xex_image, a.jobs, a.zone_cache)
    manifest = json.loads((out / "extraction.json").read_text(encoding="utf-8"))
    if not manifest["shaders"] or any(not (out / "unique" / row["file"]).is_file() for row in manifest["shaders"]):
        raise RuntimeError("extracted containers are missing; run extraction again")
    base = set(pack_containers(a.base_pack)) if a.base_pack else set()

    inputs, hlsl, spirv = (out / name for name in ("translate-input", "hlsl", "spirv"))
    for folder in (inputs, hlsl, spirv):
        if folder.exists():
            ap.error(f"scratch folder exists; preserve it or remove it before rebuilding: {folder}")
        folder.mkdir()
    for path in sorted((out / "unique").glob("*.bin")):
        if hashlib.sha1(path.read_bytes()).hexdigest() not in base:
            shutil.copyfile(path, inputs / path.name)
    expected_translations = len(list(inputs.glob("*.bin")))
    say(f"{expected_translations} new containers; {len(base)} base containers retained")
    with (out / "translate.log").open("w", encoding="utf-8") as stream:
        subprocess.run([str(a.tools / "bo2zm_hlsl.exe"), str(inputs), str(hlsl),
                        str(ROOT / "tools/xenosrecomp/translator/shader_common.h")], stdout=stream, check=True)
    with (hlsl / "translate.tsv").open(encoding="utf-8") as stream:
        translated = list(csv.DictReader(stream, delimiter="\t"))
    if len(translated) != expected_translations or any(row["status"] != "ok" for row in translated):
        raise RuntimeError("translation incomplete; inspect hlsl/translate.tsv")
    sources = sorted(path.name for path in hlsl.glob("*.hlsl"))
    if len(sources) != expected_translations:
        raise RuntimeError("translated source count differs from the input count")
    results = []
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for number, result in enumerate(pool.map(lambda name: compile_one(name, hlsl, spirv, dxc), sources), 1):
            results.append(result)
            if number % 250 == 0:
                say(f"{number}/{len(sources)} compiled")
    (out / "compile.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    if any(row["status"] != "ok" for row in results):
        raise RuntimeError("compilation incomplete; inspect compile.json; no pack written")
    pending = out / "bo2zm_shaders.pending.spvpack"
    command = [str(a.tools / "bo2zm_pack.exe"), str(out / "unique"), str(spirv), str(pending)]
    if a.base_pack:
        command += ["--base", str(a.base_pack)]
    with (out / "pack.log").open("w", encoding="utf-8") as stream:
        subprocess.run(command, stdout=stream, check=True)
    expected_containers = base | {hashlib.sha1(path.read_bytes()).hexdigest() for path in (out / "unique").glob("*.bin")}
    if set(pack_containers(pending)) != expected_containers:
        raise RuntimeError("packed container set differs from base plus extracted shaders; no pack published")
    os.replace(pending, pack)
    audit(pack, a.tools, a.log, out)
    say(f"built {pack} ({pack.stat().st_size / 1e6:.1f} MB); not verified on hardware")


if __name__ == "__main__":
    main()
