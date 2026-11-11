#!/usr/bin/env python3
"""Run real Vulkan raw transport or production automatic mixed authority.

Default mode verifies only the transport substrate. --automatic compiles exact
production manager/Buffer/executor authority and trap-dispatch bodies with an
isolated enabled header copy; production's activation gate is not modified.
"""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--automatic", action="store_true", help="Test actual automatic mixed manager/executor authority, using an isolated enabled header copy")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
env = dict(os.environ)
env["TEXMAN_MIXED_ACTIVATION_TEST" if args.automatic else "TEXMAN_BUFFER_IMAGE_PROBE"] = "1"
subprocess.run([os.environ.get("PYTHON", "python3"),
                str(root / "tests/gpu_backing_promotion/run.py")], env=env, check=True)
