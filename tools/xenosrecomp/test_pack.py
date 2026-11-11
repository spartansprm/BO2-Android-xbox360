import csv
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def container(value):
    virtual = bytearray(72)
    struct.pack_into(">III", virtual, 0, 0x102A1100, len(virtual), 12)
    struct.pack_into(">I", virtual, 24, 36)
    struct.pack_into(">II", virtual, 36, 0, 12)
    return bytes(virtual) + struct.pack(">III", value, value + 1, value + 2)


def spirv(generator):
    return struct.pack("<10I", 0x07230203, 0x00010600, generator, 2, 0,
                       5 << 16 | 15, 4, 1, 0x6E69616D, 0)


def entries(pack):
    content = pack.read_bytes()
    position = 24
    result = {}
    for _ in range(struct.unpack_from("<I", content, 12)[0]):
        size, words = struct.unpack_from("<II", content, position)
        position += 16
        original = content[position:position + size]
        position += size
        result[original] = content[position:position + words * 4]
        position += words * 4
    assert position == len(content)
    return result


def main():
    executable = Path(sys.argv[1]).resolve() / "bo2zm_pack.exe"

    def run(*arguments, success=True):
        result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        return result

    with tempfile.TemporaryDirectory(prefix="bo2-pack-test-") as temporary:
        root = Path(temporary)
        source, compiled = root / "containers", root / "spirv"
        source.mkdir()
        compiled.mkdir()
        (source / "ps_a.bin").write_bytes(container(100))
        (compiled / "p_ps_a.spv").write_bytes(spirv(1))
        base = root / "base.spvpack"
        run(source, compiled, base)
        assert entries(base) == {container(100): spirv(1)}
        base_bytes = base.read_bytes()


        (compiled / "p_ps_a.spv").write_bytes(spirv(2))
        (source / "ps_b.bin").write_bytes(container(200))
        (compiled / "p_ps_b.spv").write_bytes(spirv(3))
        merged = root / "merged.spvpack"
        run(source, compiled, merged, "--base", base)
        assert entries(merged) == {container(100): spirv(1), container(200): spirv(3)}
        assert base.read_bytes() == base_bytes

        coverage = list(csv.DictReader(io.StringIO(run("--coverage", merged).stdout), delimiter="\t"))
        assert len(coverage) == 2 and all(row["stage"] == "ps" and row["words"] == "3" for row in coverage)
        assert len({row["microcode_hash"] for row in coverage}) == 2
        assert {row["microcode_hash"] for row in coverage} >= {
            row["microcode_hash"] for row in csv.DictReader(io.StringIO(run("--coverage", base).stdout), delimiter="\t")}
        run(source, compiled, merged, "--base", base, success=False)
        assert entries(merged) == {container(100): spirv(1), container(200): spirv(3)}

        tampered = root / "tampered.spvpack"
        tampered.write_bytes(base_bytes[:-1] + bytes([base_bytes[-1] ^ 1]))
        run(source, compiled, root / "invalid.spvpack", "--base", tampered, success=False)
        assert not (root / "invalid.spvpack").exists()
    print("pack extension, base preservation, coverage, output protection and integrity checks passed")


if __name__ == "__main__":
    main()
