#!/usr/bin/env python3
"""Reassemble Maxwell/IR traces and generic failed-shader captures from emulation.log(.zip).

Usage: python3 scripts/extract_mk11_shader_trace.py emulation.zip output_dir
No Android filesystem access or additional capture files are required.
"""
import argparse
import re
import sys
import zipfile
from pathlib import Path

TARGET_HASH = "55736CD732068324"
NAMES = {
    "maxwell": f"{TARGET_HASH}.maxwell.bin",
    "maxwell_translation_input": f"{TARGET_HASH}.maxwell_translation_input.bin",
    "before_texture_pass": f"{TARGET_HASH}.before_texture_pass.ir.txt",
    "after_texture_pass": f"{TARGET_HASH}.after_texture_pass.ir.txt",
}
BEGIN = re.compile(
    r"(?:MK11Shader|ShaderFailure)DumpBegin: hash=0x([0-9A-Fa-f]+) phase=(\w+) "
    r"totalBytes=(\d+) capturedBytes=(\d+) truncated=(true|false|0|1)"
)
CHUNK = re.compile(r"(?:MK11Shader|ShaderFailure)DumpChunk: phase=(\w+) offset=(\d+) hex=([0-9a-fA-F]+)")
END = re.compile(r"(?:MK11Shader|ShaderFailure)DumpEnd: hash=0x([0-9A-Fa-f]+) phase=(\w+) capturedBytes=(\d+)")


def read_log(source: Path) -> str:
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            names = [n for n in archive.namelist() if Path(n).name == "emulation.log"]
            if not names:
                raise ValueError("ZIP has no emulation.log")
            return archive.read(names[0]).decode("utf-8", errors="replace")
    return source.read_text(encoding="utf-8", errors="replace")


def extract(log: str, destination: Path, target_hash: str = TARGET_HASH) -> int:
    target_hash = target_hash.removeprefix("0x").removeprefix("0X").upper()
    if not re.fullmatch(r"[0-9A-F]{1,16}", target_hash):
        raise ValueError("Invalid shader hash")
    target_hash = target_hash.zfill(16)
    names = {phase: name.replace(TARGET_HASH, target_hash) for phase, name in NAMES.items()}
    parts = {}
    active = {}
    for line in log.splitlines():
        if match := BEGIN.search(line):
            shader_hash, phase, total, captured, truncated = match.groups()
            active[phase] = shader_hash.upper().zfill(16) == target_hash
            if active[phase] and phase in names:
                parts[phase] = {
                    "total": int(total),
                    "size": int(captured),
                    "truncated": truncated in ("true", "1"),
                    "chunks": {},
                    "ended": False,
                }
            continue
        if match := CHUNK.search(line):
            phase, offset, hex_data = match.groups()
            if phase in parts and active.get(phase, False):
                value = bytes.fromhex(hex_data)
                index = int(offset)
                previous = parts[phase]["chunks"].get(index)
                if previous is not None and previous != value:
                    raise ValueError(f"Conflicting {phase} chunk at offset {index}")
                parts[phase]["chunks"][index] = value
            continue
        if match := END.search(line):
            shader_hash, phase, captured = match.groups()
            if shader_hash.upper().zfill(16) == target_hash and phase in parts:
                parts[phase]["ended"] = int(captured) == parts[phase]["size"]

    if not parts:
        print("No target shader trace found (check APK commit and logging level).")
        return 1

    destination.mkdir(parents=True, exist_ok=True)
    for phase, blob in parts.items():
        offset = 0
        payload = bytearray()
        for index in sorted(blob["chunks"]):
            if index != offset:
                raise ValueError(f"Missing {phase} bytes at offset {offset}")
            payload.extend(blob["chunks"][index])
            offset = len(payload)
        if offset != blob["size"] or not blob["ended"]:
            raise ValueError(f"Incomplete {phase}: {offset}/{blob['size']} bytes, end={blob['ended']}")
        path = destination / names[phase]
        path.write_bytes(payload)
        print(f"{phase}: {len(payload)}/{blob['total']} bytes -> {path}"
              + (" (TRUNCATED)" if blob["truncated"] else ""))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--hash", default=TARGET_HASH, help="Shader hash to extract (hex)")
    args = parser.parse_args()
    try:
        return extract(read_log(args.log), args.output, args.hash)
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print(f"Extraction failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
