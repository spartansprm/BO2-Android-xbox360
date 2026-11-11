import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


sys.dont_write_bytecode = True
TOOL = Path(__file__).resolve().parents[1] / "inspect/gsc_names.py"
SPEC = importlib.util.spec_from_file_location("gsc_names", TOOL)
gsc = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gsc
SPEC.loader.exec_module(gsc)


def object_bytes(order="BE", name="maps/mp/test.gsc", legacy=False):
    pass
    data = bytearray(512)
    data[:8] = gsc.MAGIC
    prefix = ">" if order == "BE" else "<"
    struct.pack_into(prefix + "I", data, 0x14, 256)
    struct.pack_into(prefix + "I", data, 0x1c, 288)
    struct.pack_into(prefix + "I", data, 0x2c, 0 if legacy else 32)
    struct.pack_into(prefix + "H", data, 0x30, 64)
    raw = name.encode("ascii")
    data[64:64 + len(raw) + 1] = raw + b"\0"
    return data


def output_for(paths, config=None):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        args = (["--config", config] if config else []) + [str(p) for p in paths]
        code = gsc.main(args)
    return code, output.getvalue()


class MetadataTests(unittest.TestCase):
    def test_both_orders_and_legacy_size(self):
        for order in ("BE", "LE"):
            for legacy in (False, True):
                with self.subTest(order=order, legacy=legacy):
                    result = gsc.inspect_bytes(object_bytes(order, legacy=legacy))
                    self.assertEqual(result.kind, "T6")
                    viable = [c for c in result.candidates if c.plausible]
                    self.assertEqual([(c.endian, c.name) for c in viable],
                                     [(order, "maps/mp/test.gsc")])
                    self.assertIn("legacy" if legacy else "stored", viable[0].detail)

    def test_stored_size_does_not_fall_back_to_exports(self):
        data = object_bytes()
        struct.pack_into(">I", data, 0x2c, 0xffffffff)
        self.assertEqual(gsc.inspect_bytes(data).kind, "invalid")

    def test_reject_code_bounds_and_overlapping_name(self):
        for code, size in ((63, 32), (256, 7), (500, 32), (64, 32), (0xffffffff, 32)):
            with self.subTest(code=code, size=size):
                data = object_bytes()
                struct.pack_into(">I", data, 0x14, code)
                struct.pack_into(">I", data, 0x2c, size)
                self.assertEqual(gsc.inspect_bytes(data).kind, "invalid")
        data = object_bytes(legacy=True)
        struct.pack_into(">I", data, 0x1c, 255)
        self.assertEqual(gsc.inspect_bytes(data).kind, "invalid")

    def test_name_offset_nul_and_path_boundaries(self):
        for offset in (0, 63, 512, 65535):
            data = object_bytes()
            struct.pack_into(">H", data, 0x30, offset)
            self.assertEqual(gsc.inspect_bytes(data).kind, "invalid")
        for name in ("", "maps/mp/../bad.gsc", "/maps/mp/test.gsc", "maps//mp/test.gsc",
                     "maps/mp/./test.gsc", "maps/mp/test", "maps/mp/a\x1b.gsc",
                     "maps/mp/a\\b.gsc", "maps/mp/a:b.gsc", "maps/mp/a\x7f.gsc"):
            with self.subTest(name=repr(name)):
                self.assertEqual(gsc.inspect_bytes(object_bytes(name=name)).kind, "invalid")
        data = object_bytes()
        data[64:192] = b"a" * 128
        self.assertEqual(gsc.inspect_bytes(data).kind, "invalid")
        data = object_bytes()
        struct.pack_into(">H", data, 0x30, 500)
        data[500:] = b"a" * 12
        self.assertEqual(gsc.inspect_bytes(data).kind, "invalid")
        for length in (127, 128):
            name = "maps/mp/" + "a" * (length - 12) + ".gsc"
            result = gsc.inspect_bytes(object_bytes(name=name))
            self.assertEqual(result.kind, "T6" if length == 127 else "invalid")

    def test_full_magic_source_container_invalid_and_truncated(self):
        cases = [(b"main() { println(\"hello\"); }\n", "source-like text"),
                 (b"\xef\xbb\xbf// source\n", "source-like text"),
                 (b"GSCR\x00\xff", "GSCR"), (b"\xff\x00garbage", "invalid")]
        cases.extend((gsc.MAGIC[:n], "truncated") for n in range(8))
        cases.extend((gsc.MAGIC + b"\0" * n, "truncated") for n in (0, 1, 40, 55))
        for data, kind in cases:
            with self.subTest(data=data):
                self.assertEqual(gsc.inspect_bytes(data).kind, kind)
        for index in range(8):
            data = object_bytes()
            data[index] ^= 1
            self.assertEqual(gsc.inspect_bytes(data).kind, "invalid")

    def test_ambiguous_endianness_does_not_choose_a_name(self):

        data = bytearray(0x30000)
        data[:8] = gsc.MAGIC
        struct.pack_into(">I", data, 0x14, 0x10100)
        struct.pack_into(">I", data, 0x1c, 0x20200)
        data[0x30:0x32] = b"\x01\x02"
        for offset, name in ((0x102, b"maps/mp/be.gsc\0"), (0x201, b"common_scripts/le.gsc\0")):
            data[offset:offset + len(name)] = name
        result = gsc.inspect_bytes(data)
        self.assertEqual(result.kind, "ambiguous")
        self.assertEqual([c.name for c in result.candidates if c.plausible],
                         ["maps/mp/be.gsc", "common_scripts/le.gsc"])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "both.gsc"
            path.write_bytes(data)
            code, output = output_for([path])
            self.assertEqual(code, 1)
            self.assertIn("BE candidate plausible", output)
            self.assertIn("LE candidate plausible", output)
            self.assertNotIn("routing:", output)


class FilesystemTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, data=None):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(object_bytes() if data is None else data)
        return path

    def test_multiple_files_both_roots_unsupported_root_and_hashes(self):
        paths = [self.write("arbitrary.gsc", object_bytes(name="maps/mp/generic.gsc")),
                 self.write("utility.gsc", object_bytes("LE", "common_scripts/utility.gsc")),
                 self.write("upper.gsc", object_bytes(name="maps/mp/Upper.gsc")),
                 self.write("other.gsc", object_bytes(name="other_scripts/utility.gsc"))]
        before = [hashlib.sha256(p.read_bytes()).digest() for p in paths]
        code, output = output_for(paths)
        self.assertEqual(code, 0)
        self.assertIn('internal-name="maps/mp/generic.gsc"', output)
        self.assertIn('internal-name="common_scripts/utility.gsc"', output)
        self.assertEqual(output.count("within current loader path policy"), 2)
        self.assertIn("unsupported root for current loader (maps/mp/ or common_scripts/ only)", output)
        self.assertIn("unsupported name characters for current loader", output)
        self.assertIn("not compatibility proof", output)
        self.assertIn("not Xbox 360 vs PS3 origin", output)
        self.assertIn("No scripts executed", output)
        self.assertIn("names=internal preserves this exact embedded path", output)
        self.assertIn("files may be flat or nested", output)
        self.assertEqual(before, [hashlib.sha256(p.read_bytes()).digest() for p in paths])

    def test_known_studio_placeholder_flags_without_guessing_targets(self):
        paths = [self.write(f"arbitrary{n}.gsc", object_bytes(name=gsc.STUDIO_PLACEHOLDER))
                 for n in range(2)]
        code, output = output_for(paths)
        self.assertEqual(code, 0)
        self.assertEqual(output.count('internal-name="maps/mp/_imcsx_gsc_studio.gsc"'), 2)
        self.assertEqual(output.count("generic Studio placeholder"), 2)
        self.assertIn("intended replacement target is unknown", output)
        self.assertIn("consider names=files only with known target paths", output)
        self.assertIn("base=", output)
        self.assertIn("No target guessed", output)
        self.assertNotIn("maps/mp/arbitrary", output)
        self.assertNotIn("names=internal preserves", output)

    def test_unrecognized_studio_name_is_not_assumed_generic(self):
        path = self.write("unrelated.gsc", object_bytes(name="maps/mp/custom_studio.gsc"))
        code, output = output_for([path])
        self.assertEqual(code, 0)
        self.assertIn('internal-name="maps/mp/custom_studio.gsc"', output)
        self.assertNotIn("generic Studio placeholder", output)
        self.assertIn("names=internal preserves", output)
        self.assertNotIn("maps/mp/unrelated.gsc", output)

    def test_rejected_candidate_does_not_receive_routing_advice(self):
        data = object_bytes(name=gsc.STUDIO_PLACEHOLDER)
        struct.pack_into(">I", data, 0x2c, 0xffffffff)
        code, output = output_for([self.write("rejected.gsc", data)])
        self.assertEqual(code, 1)
        self.assertIn(gsc.STUDIO_PLACEHOLDER, output)
        self.assertNotIn("routing:", output)

    def test_config_recommends_exact_internal_names_for_either_mode_without_writing(self):
        paths = [self.write("flat.gsc", object_bytes(name="maps/mp/zm/test.gsc")),
                 self.write("nested/other.gsc", object_bytes("BE", "common_scripts/utility.gsc"))]
        before = [hashlib.sha256(p.read_bytes()).digest() for p in paths]
        for mode in ("zombies", "multiplayer"):
            code, output = output_for(paths, config=mode)
            self.assertEqual(code, 0)
            self.assertIn(f"mode={mode}\nnames=internal\n", output)
            self.assertIn("compatibility and execution not checked", output)
            self.assertIn("no files written or converted", output)
            self.assertNotIn("names=files\n", output)
        self.assertFalse((self.root / "mod.ini").exists())
        self.assertEqual(before, [hashlib.sha256(p.read_bytes()).digest() for p in paths])

    def test_config_does_not_overwrite_existing_ini(self):
        self.write("object.gsc")
        path = self.write("mod.ini", b"mode=multiplayer\nnames=files\n")
        before = path.read_bytes()
        code, output = output_for([self.root], config="zombies")
        self.assertEqual(code, 0)
        self.assertIn("mode=zombies\nnames=internal\n", output)
        self.assertEqual(path.read_bytes(), before)

    def test_config_refuses_little_endian_bytecode(self):
        path = self.write("pc.gsc", object_bytes("LE", "maps/mp/test.gsc"))
        code, output = output_for([path], config="multiplayer")
        self.assertEqual(code, 1)
        self.assertIn("little-endian bytecode is unsupported", output)
        self.assertNotIn("Recommended mod.ini", output)

    def test_config_generic_and_duplicate_names_require_mapping(self):
        for name, blockers in ((gsc.STUDIO_PLACEHOLDER, ("generic Studio placeholder", "duplicate internal names")),
                               ("maps/mp/shared.gsc", ("duplicate internal names",))):
            with self.subTest(name=name):
                paths = [self.write(f"target{n}.gsc", object_bytes(name=name)) for n in range(2)]
                code, output = output_for(paths, config="multiplayer")
                self.assertEqual(code, 1)
                self.assertIn("No INI recommendation", output)
                self.assertIn("explicit target mapping", output)
                for blocker in blockers:
                    self.assertIn(blocker, output)
                self.assertNotIn("mode=multiplayer\n", output)
                self.assertNotIn("names=internal\n", output)
                self.assertNotIn("maps/mp/target", output)

    def test_config_placeholder_under_other_supported_root_still_needs_mapping(self):
        name = "common_scripts/_imcsx_gsc_studio.gsc"
        code, output = output_for([self.write("arbitrary.gsc", object_bytes(name=name))],
                                  config="zombies")
        self.assertEqual(code, 1)
        self.assertIn(f'internal-name="{name}"', output)
        self.assertIn("generic Studio placeholder needs explicit target mapping", output)
        self.assertNotIn("mode=zombies\n", output)

    def test_config_refuses_unsupported_root_and_name_characters(self):
        for name in ("other_scripts/a.gsc", "maps/mp/Upper.gsc"):
            code, output = output_for([self.write("outside.gsc", object_bytes(name=name))],
                                      config="zombies")
            self.assertEqual(code, 1)
            self.assertIn("embedded name is outside loader path policy", output)
            self.assertNotIn("mode=zombies\n", output)

    def test_config_refuses_incomplete_scan_even_with_a_valid_object(self):
        path = self.write("valid.gsc")
        results = [(path, gsc.inspect_bytes(object_bytes())),
                   (self.root, gsc.Inspection("scan-limit", "entry limit reached"))]
        with mock.patch.object(gsc, "scan", return_value=iter(results)):
            code, output = output_for([self.root], config="zombies")
        self.assertEqual(code, 1)
        self.assertIn("incomplete or rejected metadata scan", output)
        self.assertNotIn("mode=zombies\n", output)

    def test_config_refuses_empty_source_invalid_and_ambiguous_inputs(self):
        paths = [self.root, self.write("source.gsc", b"main() {}\n"),
                 self.write("bad.gsc", b"\xffbad")]

        empty = self.root / "empty"
        empty.mkdir()
        paths[0] = empty
        for path in paths:
            code, output = output_for([path], config="zombies")
            self.assertEqual(code, 1)
            self.assertIn("No INI recommendation", output)
            self.assertNotIn("mode=zombies\n", output)
        with mock.patch.object(gsc, "scan", return_value=iter([
                (self.root, gsc.Inspection("ambiguous", "endianness unresolved"))])):
            code, output = output_for([self.root], config="zombies")
        self.assertEqual(code, 1)
        self.assertNotIn("mode=zombies\n", output)

    def test_errors_do_not_prevent_other_inputs(self):
        source = self.write("source.gsc", b"main() {}\n")
        compiled = self.write("object.gsc")
        code, output = output_for([self.root / "missing.gsc", source, compiled])
        self.assertEqual(code, 1)
        self.assertIn("source-like text", output)
        self.assertIn("BE candidate plausible", output)
        self.assertIn("error", output)

    def test_rejected_inputs_remain_unchanged(self):
        missing_nul = object_bytes()
        missing_nul[64:192] = b"a" * 128
        payloads = [b"main() {}\n", b"GSCR\0", b"\xffbad", gsc.MAGIC + b"\0",
                    missing_nul, b"x" * (gsc.MAX_FILE_SIZE + 1)]
        paths = [self.write(f"rejected{n}.gsc", data) for n, data in enumerate(payloads)]
        before = [hashlib.sha256(p.read_bytes()).digest() for p in paths]
        code, output = output_for(paths)
        self.assertEqual(code, 1)
        for status in ("source-like text", "GSCR", "invalid", "truncated", "oversized"):
            self.assertIn(status, output)
        self.assertEqual(before, [hashlib.sha256(p.read_bytes()).digest() for p in paths])

    def test_folder_selects_gsc_case_insensitively(self):
        selected = self.write("nested/a.GSC")
        self.write("mod.ini", b"mode=zombies\n")
        results = list(gsc.scan([self.root]))
        self.assertEqual([(p, r.kind) for p, r in results], [(selected, "T6")])

    def test_folder_entry_budget_counts_other_files_and_roots(self):
        for n in range(6):
            self.write(f"{n}.txt", b"ignored")
        results = list(gsc.scan([self.root, self.root], gsc.ScanLimits(entries=3)))
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0][1].kind, "scan-limit")
        self.assertIn("entry limit 3", results[0][1].detail)

    def test_file_budget_shared_across_multiple_inputs(self):
        paths = [self.write(f"{n}.gsc") for n in range(3)]
        results = list(gsc.scan(paths, gsc.ScanLimits(files=2)))
        self.assertEqual([r.kind for _, r in results], ["T6", "T6", "scan-limit"])

    def test_cli_folder_limit_returns_failure(self):
        for n in range(129):
            self.write(f"{n}.gsc")
        code, output = output_for([self.root])
        self.assertEqual(code, 1)
        self.assertEqual(output.count("BE candidate plausible"), 128)
        self.assertIn("scan-limit file limit 128 reached", output)

    def test_directory_depth_limit(self):
        self.write("a/b/c/deep.gsc")
        results = list(gsc.scan([self.root], gsc.ScanLimits(depth=1)))
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0][1].kind, "scan-limit")
        self.assertIn("depth limit 1", results[0][1].detail)

    def test_oversized_file_rejected_before_opening(self):
        path = self.write("large.gsc", b"x" * (gsc.MAX_FILE_SIZE + 1))
        with mock.patch.object(gsc.os, "open", side_effect=AssertionError("must not open")):
            self.assertEqual(gsc.inspect_file(path).kind, "oversized")

    def test_file_at_exact_size_limit_is_inspected(self):
        data = object_bytes()
        data.extend(b"\0" * (gsc.MAX_FILE_SIZE - len(data)))
        path = self.write("maximum.gsc", data)
        self.assertEqual(gsc.inspect_file(path).kind, "T6")

    def test_file_growth_still_uses_a_bounded_read(self):
        path = self.write("growing.gsc", b"x" * 2048)
        actual_lstat = Path.lstat

        def stale_size(p):
            info = actual_lstat(p)
            if p == path:

                return SimpleNamespace(st_mode=info.st_mode, st_ino=info.st_ino,
                                       st_dev=info.st_dev, st_size=64,
                                       st_file_attributes=getattr(info, "st_file_attributes", 0))
            return info

        with mock.patch.object(gsc, "MAX_FILE_SIZE", 128), \
                mock.patch.object(Path, "lstat", stale_size), \
                mock.patch.object(gsc, "inspect_bytes", wraps=gsc.inspect_bytes) as inspect:
            self.assertEqual(gsc.inspect_file(path).kind, "oversized")
            self.assertEqual(len(inspect.call_args.args[0]), 129)

    def test_reparse_attribute_is_rejected(self):
        info = mock.Mock(st_mode=0o040755, st_file_attributes=0x400)
        self.assertTrue(gsc._is_link(info))

    def test_directory_junction_or_symlink_and_explicit_descendant(self):
        target = self.root / "target"
        self.write("target/a.gsc")
        link = self.root / "alias"
        if os.name == "nt":
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            link.symlink_to(target, target_is_directory=True)
        self.addCleanup(os.rmdir if os.name == "nt" else os.unlink, link)
        with mock.patch.object(gsc.os, "open", side_effect=AssertionError("must not open")):
            for paths in ([link], [link / "a.gsc"]):
                results = list(gsc.scan(paths))
                self.assertEqual([r.kind for _, r in results], ["skipped"])
        results = list(gsc.scan([self.root]))
        self.assertCountEqual([r.kind for _, r in results], ["T6", "skipped"])

    def test_empty_folder(self):
        code, output = output_for([self.root])
        self.assertEqual(code, 0)
        self.assertIn("No .gsc files found", output)


if __name__ == "__main__":
    unittest.main()
