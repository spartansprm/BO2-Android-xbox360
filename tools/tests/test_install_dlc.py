from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "install_dlc.py"
TITLE = "415608C3"
PACKAGE_ZM = "A" * 40
PACKAGE_MP = "B" * 42
PACKAGE_OTHER = "C" * 40
ENGLISH_ZM = {
    "dlc1.id", "dlczm1.id", "zm_authored.ff", "en_zm_authored.ff",
    "zm_authored.english.sabs",
}
ENGLISH_MP = {
    "mp_authored.ff", "en_mp_authored.ff", "mp_authored.english.sabl",
    "mpl_authored.ff", "authored_mp.ff", "authored_mp_loc.ff",
}
FOREIGN = {
    "fr_zm_authored.ff", "fr_mp_authored.ff", "zm_authored.french.sabs",
    "mp_authored.spanish.sabl",
}
MIXED = ENGLISH_ZM | ENGLISH_MP | FOREIGN
MP_ONLY = {"dlc2.id", "mp_authored.ff", "en_mp_authored.ff", "fr_mp_authored.ff"}


def payload(name):
    return f"Authored installer fixture: {name}\n".encode("ascii")


def write_package(path, names):
    pass
    block = 0x1000
    base = 0xA000
    names = sorted(names)
    data = bytearray(base + (len(names) + 2) * block)
    data[:4] = b"LIVE"
    struct.pack_into(">III", data, 0x340, base, 2, 0)
    struct.pack_into(">I", data, 0x360, int(TITLE, 16))
    display = "Authored DLC fixture".encode("utf-16-be")
    data[0x411:0x411 + len(display)] = display
    struct.pack_into(">QII", data, 0x22C, 0, 3, 1)
    data[0x37B] = 1
    struct.pack_into("<H", data, 0x37C, 1)
    struct.pack_into(">I", data, 0x395, len(names) + 1)
    for index in range(len(names) + 1):
        struct.pack_into(">I", data, base + index * 24 + 20, 0xFFFFFF)
    for index, name in enumerate(names):
        encoded = name.encode("ascii")
        content = payload(name)
        assert 0 < len(encoded) <= 40 and len(content) <= block
        entry = base + block + index * 0x40
        data[entry:entry + len(encoded)] = encoded
        data[entry + 0x28] = len(encoded)
        data[entry + 0x2C:entry + 0x2F] = (1).to_bytes(3, "little")
        data[entry + 0x2F:entry + 0x32] = (index + 1).to_bytes(3, "little")
        struct.pack_into(">HI", data, entry + 0x32, 0xFFFF, len(content))
        offset = base + (index + 2) * block
        data[offset:offset + len(content)] = content
    path.write_bytes(data)


def snapshot(root):
    return {
        path.relative_to(root).as_posix(): (
            None if path.is_dir() else path.read_bytes(), path.stat().st_mtime_ns
        )
        for path in root.rglob("*")
    }


class InstallDlcTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="install-dlc-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "packages"
        self.source.mkdir()
        self.output = self.root / "content"
        self.installed = self.output / "0000000000000000" / TITLE
        write_package(self.source / PACKAGE_ZM, MIXED)
        write_package(self.source / PACKAGE_MP, MP_ONLY)
        write_package(self.source / PACKAGE_OTHER, {"dlczm3.id", "zm_other.ff"})

    def run_installer(self, *args, status=0):
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(TOOL), str(self.source),
             "--out", str(self.output), *args],
            capture_output=True, text=True, encoding="utf-8", timeout=15,
        )
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def assert_package(self, package, expected):
        directory = self.installed / "00000002" / package
        self.assertEqual({p.name for p in directory.iterdir()}, expected)
        for name in expected:
            self.assertEqual((directory / name).read_bytes(), payload(name))
        header = self.installed / "Headers" / "00000002" / (package + ".header")
        content = header.read_bytes()
        self.assertEqual(len(content), 0x14C)
        self.assertEqual(struct.unpack_from(">I", content, 0x140)[0], int(TITLE, 16))
        self.assertEqual(content[0x108:0x108 + len(package)], package.encode("ascii"))
        self.assertEqual(struct.unpack_from("<I", content, 0x148)[0], 3)

    def test_default_installs_english_zombies_and_skips_mp_only_package(self):
        before = snapshot(self.source)
        result = self.run_installer()
        self.assert_package(PACKAGE_ZM, ENGLISH_ZM)
        self.assert_package(PACKAGE_OTHER, {"dlczm3.id", "zm_other.ff"})
        self.assertFalse((self.installed / "00000002" / PACKAGE_MP).exists())
        self.assertFalse((self.installed / "Headers" / "00000002" / (PACKAGE_MP + ".header")).exists())
        self.assertIn("multiplayer only", result.stdout)
        self.assertEqual(snapshot(self.source), before)

    def test_multiplayer_keeps_both_modes_but_still_excludes_foreign_languages(self):
        self.run_installer("--multiplayer")
        self.assert_package(PACKAGE_ZM, ENGLISH_ZM | ENGLISH_MP)
        self.assert_package(PACKAGE_MP, MP_ONLY - {"fr_mp_authored.ff"})

    def test_all_keeps_every_language_and_multiplayer(self):
        self.run_installer("--all")
        self.assert_package(PACKAGE_ZM, MIXED)
        self.assert_package(PACKAGE_MP, MP_ONLY)

    def test_single_package_selection(self):
        self.run_installer("--multiplayer", "--package", PACKAGE_MP)
        self.assert_package(PACKAGE_MP, MP_ONLY - {"fr_mp_authored.ff"})
        self.assertEqual({p.name for p in (self.installed / "00000002").iterdir()}, {PACKAGE_MP})

    def test_repeated_package_selection(self):
        self.run_installer("--multiplayer", "--package", PACKAGE_ZM, "--package", PACKAGE_MP)
        self.assert_package(PACKAGE_ZM, ENGLISH_ZM | ENGLISH_MP)
        self.assert_package(PACKAGE_MP, MP_ONLY - {"fr_mp_authored.ff"})
        self.assertFalse((self.installed / "00000002" / PACKAGE_OTHER).exists())

    def test_package_selection_accepts_hex_case_without_renaming_output(self):
        self.run_installer("--package", PACKAGE_ZM.lower())
        self.assert_package(PACKAGE_ZM, ENGLISH_ZM)
        self.assertFalse((self.installed / "00000002" / PACKAGE_OTHER).exists())

    def test_selected_mp_package_still_obeys_default_zombies_filter(self):
        self.run_installer("--package", PACKAGE_MP)
        self.assertFalse(self.output.exists())

    def test_package_paths_and_invalid_names_are_rejected_before_writes(self):
        for name in (
            "../" + PACKAGE_ZM, "..\\" + PACKAGE_ZM, "nested/" + PACKAGE_ZM,
            "nested\\" + PACKAGE_ZM, str(self.source / PACKAGE_ZM),
            "A" * 39, "A" * 43, "G" * 40, PACKAGE_ZM + ".header", PACKAGE_ZM + "\n", "",
        ):
            with self.subTest(name=name):
                result = self.run_installer("--package", PACKAGE_ZM, "--package", name, status=2)
                self.assertIn("original hexadecimal package filename", result.stderr)
                self.assertFalse(self.output.exists())

    def test_missing_package_and_directory_are_rejected_before_writes(self):
        missing = "D" * 40
        directory = "E" * 40
        (self.source / directory).mkdir()
        for name in (missing, directory):
            with self.subTest(name=name):
                result = self.run_installer("--package", PACKAGE_ZM, "--package", name, status=2)
                self.assertIn("package not found", result.stderr)
                self.assertFalse(self.output.exists())

    def test_list_leaves_source_and_absent_output_unchanged(self):
        before = snapshot(self.root)
        for options, expected in (
            ((), ENGLISH_ZM), (("--multiplayer",), ENGLISH_ZM | ENGLISH_MP), (("--all",), MIXED),
        ):
            with self.subTest(options=options):
                result = self.run_installer("--list", "--package", PACKAGE_ZM, *options)
                listed = {line.strip().split(" (")[0] for line in result.stdout.splitlines()
                          if line.startswith("  ")}
                self.assertEqual(listed, expected)
                self.assertIn("selected ", result.stdout)
                self.assertEqual(snapshot(self.root), before)
                self.assertFalse(self.output.exists())

    def test_list_preserves_existing_files_headers_and_partial_files(self):
        directory = self.installed / "00000002" / PACKAGE_ZM
        headers = self.installed / "Headers" / "00000002"
        directory.mkdir(parents=True)
        headers.mkdir(parents=True)
        (directory / "zm_authored.ff").write_bytes(b"Existing authored data")
        (directory / "zm_authored.ff.part").write_bytes(b"Existing partial data")
        (headers / (PACKAGE_ZM + ".header")).write_bytes(b"Existing authored header")
        before = snapshot(self.root)
        self.run_installer("--list", "--multiplayer")
        self.assertEqual(snapshot(self.root), before)


if __name__ == "__main__":
    unittest.main()
