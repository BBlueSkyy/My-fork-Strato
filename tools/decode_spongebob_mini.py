#!/usr/bin/env python3
"""Decode SBMINI1/SBMINI2 snapshots with stable draw identity and HDR NaN counts.

This is a Strato diagnostic format, not a native RenderDoc .rdc file.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

FIELDS_V1 = (
    "operation role image generation write_serial host_format guest_format "
    "width height x y roi_width roi_height mip layer block_width block_height bpb bytes"
).split()
FIELDS_V2 = FIELDS_V1 + (
    "pipeline_hash channel_sequence shader0 shader1 shader2 shader3 shader4 shader5 "
    "draw_kind topology indexed first count instances sampled_ordinal tic_index matching_tics tic_iova"
).split()
MAX_ROWS = 18
MAX_BYTES = 65536


def hdr_nonfinite(data: bytes):
    """Bit-level R11G11B10_UFLOAT (VkFormat 122) NaN/+Inf classification."""
    counts = {"nan_components": [0, 0, 0], "infinite_components": [0, 0, 0],
              "nan_pixels": 0, "infinite_pixels": 0}
    nan_mask = []
    for (word,) in struct.iter_unpack("<I", data):
        channels = ((word & 0x7ff, 6), ((word >> 11) & 0x7ff, 6),
                    ((word >> 22) & 0x3ff, 5))
        found_nan = found_inf = False
        for i, (bits, mantissa_bits) in enumerate(channels):
            exponent = (bits >> mantissa_bits) & 31
            mantissa = bits & ((1 << mantissa_bits) - 1)
            if exponent == 31 and mantissa:
                found_nan = True
                counts["nan_components"][i] += 1
            elif exponent == 31:
                found_inf = True
                counts["infinite_components"][i] += 1
        nan_mask.append(found_nan)
        counts["nan_pixels"] += found_nan
        counts["infinite_pixels"] += found_inf
    return counts, nan_mask


def decode(capture: Path, output: Path):
    output.mkdir(parents=True, exist_ok=True)
    records = []
    with capture.open("rb") as source:
        magic = source.readline()
        if magic not in (b"SBMINI1\n", b"SBMINI2\n"):
            raise ValueError("Unsupported Mini capture format")
        fields = FIELDS_V2 if magic == b"SBMINI2\n" else FIELDS_V1
        while line := source.readline():
            if len(records) == MAX_ROWS or len(line) > 1024:
                raise ValueError("Unbounded capture records")
            parts = line.rstrip(b"\n").split(b",")
            if len(parts) != len(fields):
                raise ValueError("Invalid record metadata")
            row = dict(zip(fields, (int(value) for value in parts)))
            if not (1 <= row["operation"] <= 3 and 1 <= row["role"] <= 6 and
                    1 <= row["bytes"] <= MAX_BYTES and
                    1 <= row["roi_width"] <= 64 and 1 <= row["roi_height"] <= 64 and
                    1 <= row["width"] <= 16384 and 1 <= row["height"] <= 16384 and
                    row["x"] + row["roi_width"] <= row["width"] and
                    row["y"] + row["roi_height"] <= row["height"] and
                    1 <= row["block_width"] <= 16 and 1 <= row["block_height"] <= 16 and
                    1 <= row["bpb"] <= 32):
                raise ValueError("Invalid capture bounds")
            expected = ((row["roi_width"] + row["block_width"] - 1) // row["block_width"] *
                        (row["roi_height"] + row["block_height"] - 1) // row["block_height"] *
                        row["bpb"])
            if expected != row["bytes"]:
                raise ValueError("Inconsistent captured byte count")
            data = source.read(row["bytes"])
            if len(data) != row["bytes"] or source.read(1) != b"\n":
                raise ValueError("Truncated capture data")
            stem = f"op{row['operation']}-role{row['role']}-fmt{row['host_format']}"
            (output / (stem + ".bin")).write_bytes(data)
            row["raw"] = stem + ".bin"
            row["sha256"] = hashlib.sha256(data).hexdigest()
            row["nonzero_bytes"] = sum(c != 0 for c in data)
            row["zero_bytes"] = len(data) - row["nonzero_bytes"]
            row["_data"] = data
            if row["host_format"] == 122 and row["block_width"] == 1 and row["bpb"] == 4:
                counts, _ = hdr_nonfinite(data)
                row.update(counts)
            # Optional preview: true four-channel uncompressed Vulkan formats.
            fmt = row["host_format"]
            if (fmt in (37, 43, 44, 50) and row["block_width"] == 1 and
                    row["block_height"] == 1 and row["bpb"] == 4):
                try:
                    from PIL import Image
                    if fmt in (44, 50):
                        image = Image.frombytes("RGBA", (row["roi_width"], row["roi_height"]), data, "raw", "BGRA")
                    else:
                        image = Image.frombytes("RGBA", (row["roi_width"], row["roi_height"]), data)
                    image.save(output / (stem + ".png"))
                    row["preview"] = stem + ".png"
                except ImportError:
                    pass
            records.append(row)
    if not records:
        raise ValueError("Empty Mini capture")
    groups = {}
    for row in records:
        entries = groups.setdefault(row["operation"], {})
        if row["role"] in entries:
            raise ValueError("Duplicate operation/role")
        entries[row["role"]] = row
    results = []
    for operation, entries in sorted(groups.items()):
        if not all(role in entries for role in (1, 2)) or not any(role >= 3 for role in entries):
            raise ValueError("Incomplete target/input operation")
        before = entries[1]["_data"]
        after = entries[2]["_data"]
        if len(before) != len(after):
            raise ValueError("Target before/after length mismatch")
        meta = entries[1]
        if magic == b"SBMINI2\n":
            for row in entries.values():
                if any(row[key] != meta[key] for key in (
                    "pipeline_hash", "channel_sequence", "shader0", "shader1", "shader2",
                    "shader3", "shader4", "shader5", "draw_kind", "topology", "indexed",
                    "first", "count", "instances"
                )):
                    raise ValueError("Conflicting metadata within one draw operation")
            if (entries[1]["image"] != entries[2]["image"] or
                    entries[1]["generation"] != entries[2]["generation"]):
                raise ValueError("Target backing changed within one draw")
        introduced = {}
        if (entries[1]["host_format"] == entries[2]["host_format"] == 122):
            _, before_nan = hdr_nonfinite(before)
            _, after_nan = hdr_nonfinite(after)
            introduced = {
                "target_nan_pixels_before": sum(before_nan),
                "target_nan_pixels_after": sum(after_nan),
                "target_new_nan_pixels": sum(b and not a for a, b in zip(before_nan, after_nan)),
            }
        results.append({
            "operation": operation,
            **introduced,
            **({
                "pipeline_hash": f"{meta['pipeline_hash']:016x}",
                "shader_hashes": [f"{meta[f'shader{i}']:016x}" for i in range(6)],
                "draw_kind": meta["draw_kind"],
                "topology": meta["topology"],
                "indexed": meta["indexed"],
                "first": meta["first"],
                "count": meta["count"],
                "instances": meta["instances"],
            } if magic == b"SBMINI2\n" else {}),
            "input_count": sum(role >= 3 for role in entries),
            "target_changed_bytes": sum(a != b for a, b in zip(before, after)),
            "target_bytes": len(before),
            "mean_absolute_byte_difference": round(
                sum(abs(a - b) for a, b in zip(before, after)) / len(before), 5
            )
        })
    for row in records:
        del row["_data"]
    (output / "manifest.json").write_text(
        json.dumps({"snapshots": records, "comparisons": results}, indent=2)
    )
    return records, results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    snapshots, operations = decode(args.capture, args.output)
    print(f"Decoded {len(snapshots)} snapshots, {len(operations)} render operations")
    for op in operations:
        print(op)
