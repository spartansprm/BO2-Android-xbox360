import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = ["join_code_test.cpp", "relay_wire_test.cpp"]


def main():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or "C:/Program Files/LLVM/bin/clang++.exe"
    with tempfile.TemporaryDirectory(prefix="bo2-join-code-") as temp:
        for source in TESTS:
            exe = Path(temp) / (Path(source).stem + ".exe")
            subprocess.run([compiler, "-std=c++20", "-O1", "-UNDEBUG", "-I" + str(ROOT / "sdk/include"),
                            str(ROOT / "tools/tests" / source), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == "__main__":
    main()
