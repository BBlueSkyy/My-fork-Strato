#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Check an exported compute candidate and invoke spirv-val without rewriting it."""
import argparse
import hashlib
import pathlib
import shutil
import struct
import subprocess
import sys

MAX_SHADER_BYTES = 2 * 1024 * 1024
MAX_METADATA_BYTES = 64 * 1024


def read_fields(path, maximum):
    if path.stat().st_size > maximum:
        raise ValueError(f"{path.name}: exceeds diagnostic size limit")
    result = {}
    for line in path.read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            # Multiple descriptor lines are expected; identity fields must be unique.
            if key in result and key != "descriptor":
                raise ValueError(f"{path.name}: duplicate {key}")
            result[key] = value
    return result


def inspect_capture(path):
    metadata = read_fields(path / "metadata.txt", MAX_METADATA_BYTES)
    if metadata.get("status") != "pending" or metadata.get("format") != "1" or metadata.get("stage") != "compute":
        raise ValueError("metadata is omitted, incomplete, or an unsupported capture format")
    for name, field in (("guest.bin", "guest_bytes"), ("shader.spv", "spirv_bytes")):
        size = (path / name).stat().st_size
        if size > MAX_SHADER_BYTES or size != int(metadata[field]):
            raise ValueError(f"{name}: size mismatch or exceeds diagnostic limit")
    spirv = (path / "shader.spv").read_bytes()
    if len(spirv) < 20 or len(spirv) % 4:
        raise ValueError("shader.spv: incomplete SPIR-V word stream/header")
    magic, version, _, _, schema = struct.unpack("<5I", spirv[:20])
    if magic != 0x07230203 or schema != 0:
        raise ValueError("shader.spv: invalid SPIR-V magic/schema")
    supported = int(metadata["profile.supported_spirv"])
    if supported not in (0x00010000, 0x00010400) or version > supported:
        raise ValueError("shader.spv: version inconsistent with recorded compiler profile")
    # Strato requests Vulkan 1.1; SPIR-V 1.4 is enabled through VK_KHR_spirv_1_4.
    if int(metadata["application_api_version"]) != (1 << 22 | 1 << 12):
        raise ValueError("unsupported application Vulkan API version")
    result_path = path / "result.txt"
    result = read_fields(result_path, 1024) if result_path.exists() else {"vk_result": "pending"}
    if result["vk_result"] != "pending":
        int(result["vk_result"])
    return {
        "guest_hash": metadata["guest_hash"],
        "vk_result": result["vk_result"],
        "target_env": "vulkan1.1spv1.4" if supported == 0x00010400 else "vulkan1.1",
        "sha256": hashlib.sha256(spirv).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path, help="exported compute_diagnostics directory")
    parser.add_argument("--inspect-only", action="store_true", help="check completeness without spirv-val")
    args = parser.parse_args()
    try:
        result = inspect_capture(args.capture)
        print(" ".join(f"{key}={value}" for key, value in result.items()))
        if args.inspect_only:
            return 0
        validator = shutil.which("spirv-val")
        if not validator:
            raise ValueError("spirv-val is unavailable; install SPIRV-Tools or use --inspect-only")
        return subprocess.run([validator, "--target-env", result["target_env"], str(args.capture / "shader.spv")], check=False).returncode
    except (OSError, ValueError, KeyError) as error:
        print(f"capture validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
