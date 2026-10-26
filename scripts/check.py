"""Validate the source and installed packages while holding the SQLite lease."""
import json
import os
from pathlib import Path
import subprocess
import sys
from forgedeps.build_kit.session import leased_environment

root = Path(__file__).resolve().parents[1]
receipt = root / "sqlite-environment.json"
prefixes = json.loads(receipt.read_text())["prefixes"]
install = root / "build/install"
commands = [
    ["cmake", "-S", root, "-B", root / "build/main", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
     "-DBUILD_TESTING=ON", f"-DPython3_EXECUTABLE={sys.executable}", f"-DFORGEDEPS_TEST_ENVIRONMENT={receipt}"],
    ["cmake", "--build", root / "build/main", "--parallel", "4"],
    ["ctest", "--test-dir", root / "build/main", "--output-on-failure"],
    ["cmake", "--install", root / "build/main", "--prefix", install],
    ["cmake", "-S", root / "tests/installed", "-B", root / "build/consumer", "-G", "Ninja",
     "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_PREFIX_PATH=" + ";".join([str(install), *prefixes])],
    ["cmake", "--build", root / "build/consumer", "--parallel", "4"],
    ["ctest", "--test-dir", root / "build/consumer", "--output-on-failure"],
]
with leased_environment([receipt]) as environment:
    for command in commands:
        subprocess.run([str(value) for value in command], cwd=root, env=environment, check=True)
