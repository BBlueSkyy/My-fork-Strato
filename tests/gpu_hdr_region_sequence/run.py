#!/usr/bin/env python3
"""Prove HDR image-only capability and the logged-footprint admission limits.

--require-actual-coherence is a deliberately failing integration gate, not a
supported configuration. It requires stale retained readers in the unadmitted
PR453 footprints to be repaired. Production is not modified by either mode.
"""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--require-actual-coherence", action="store_true")
args = parser.parse_args()
env = dict(os.environ, TEXMAN_HDR_REGION_SEQUENCE="1")
if args.require_actual_coherence:
    env["TEXMAN_HDR_REGION_REQUIRE_ACTUAL_COHERENCE"] = "1"
root = Path(__file__).resolve().parents[2]
subprocess.run([os.environ.get("PYTHON", "python3"),
                str(root / "tests/gpu_backing_promotion/run.py")], env=env, check=True)
