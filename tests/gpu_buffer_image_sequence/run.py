#!/usr/bin/env python3
"""Run bounded raw-buffer/image transfer geometry against the real Vulkan fixture.

This is the transfer substrate gate; it is not evidence of mixed-authority
registration or automatic descriptor coherence.
"""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
env = dict(os.environ, TEXMAN_BUFFER_IMAGE_PROBE="1")
subprocess.run([os.environ.get("PYTHON", "python3"),
                str(root / "tests/gpu_backing_promotion/run.py")], env=env, check=True)
