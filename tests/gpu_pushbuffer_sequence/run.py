#!/usr/bin/env python3
"""Exercise the exact production mapping/flush/materialization block."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PATH = "app/src/main/cpp/skyline/soc/gm20b/gpfifo.cpp"
baseline = os.environ.get("PUSHBUFFER_SEQUENCE_BASELINE_REF")
source = (subprocess.check_output(["git", "show", baseline + ":" + PATH], cwd=ROOT, text=True)
          if baseline else (ROOT / PATH).read_text())
start = source.index("        auto pushBufferMappedRanges{", source.index("void ChannelGpfifo::Process("))
end = source.index("        // There will be at least one entry here", start)
with tempfile.TemporaryDirectory(prefix="strato-pushbuffer-sequence-") as directory:
    directory = Path(directory)
    (directory / "production.inc").write_text(source[start:end])
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I" + str(directory),
                    str(HERE / "tests.cpp"), "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True, timeout=10)
