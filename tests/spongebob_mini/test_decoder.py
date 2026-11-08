#!/usr/bin/env python3
import importlib.util
import struct
import tempfile
from pathlib import Path

module_path = Path(__file__).resolve().parents[2] / "tools" / "decode_spongebob_mini.py"
spec = importlib.util.spec_from_file_location("decode_spongebob_mini", module_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def record(op, role, data, version=1, shader=0x1100, fmt=37):
    values = [op,role,42,7,15,fmt,fmt,512,512,224,224,64,64,0,0,1,1,4,len(data)]
    if version in (2, 3):
        values += [0xABCDEF,345,0x100,0x200,0,0,0,shader,
                   1,3,1,0,6,1,0 if role == 3 else 0xffffffff,
                   17 if role == 3 else 0xffffffff,
                   1 if role == 3 else 0,
                   0x12345678 if role == 3 else 0]
    if version == 3:
        values += [1] # Natural render pass; not an isolated draw.
    return (",".join(map(str, values))+"\n").encode() + data + b"\n"

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    before = bytes(64*64*4)
    after = bytes([255]) * (64*64*4)
    source = bytes([67]) * (64*64*4)
    capture = root / "valid.mcap"
    capture.write_bytes(b"SBMINI1\n" + record(1,1,before) +
                        record(1,3,source) + record(1,2,after))
    rows, comparisons = module.decode(capture, root / "decoded")
    assert len(rows) == 3
    assert comparisons[0]["target_changed_bytes"] == len(before)
    assert comparisons[0]["mean_absolute_byte_difference"] == 255
    assert (root / "decoded" / "manifest.json").exists()

    for bad in (b"WRONG\n", capture.read_bytes()[:-3]):
        (root / "invalid.mcap").write_bytes(bad)
        try:
            module.decode(root / "invalid.mcap", root / "invalid-decoded")
        except ValueError:
            pass
        else:
            raise AssertionError("invalid capture decoded successfully")
    # New version embeds a stable pipeline key, six shader hashes and TIC/IOVA.
    capture.write_bytes(b"SBMINI2\n" + record(1,1,before,2) +
                        record(1,3,source,2) + record(1,2,after,2))
    rows2, comparisons2 = module.decode(capture, root / "decoded-v2")
    assert len(rows2) == 3
    assert comparisons2[0]["pipeline_hash"] == "0000000000abcdef"
    assert comparisons2[0]["shader_hashes"][5] == "0000000000001100"
    assert next(row for row in rows2 if row["role"] == 3)["tic_iova"] == 0x12345678
    assert comparisons2[0]["target_changed_bytes"] == len(before)

    capture.write_bytes(b"SBMINI3\n" + record(1,1,before,3) +
                        record(1,3,source,3) + record(1,2,after,3))
    rows_pass, ops_pass = module.decode(capture, root / "decoded-pass")
    assert ops_pass[0]["capture_scope"] == "natural_render_pass"
    assert ops_pass[0]["target_changed_bytes"] == len(before)
    assert rows_pass[0]["capture_scope"] == 1

    # A mismatch of shader hashes across images in the same draw is invalid.
    capture.write_bytes(b"SBMINI2\n" + record(1,1,before,2) +
                        record(1,3,source,2,shader=0x777) + record(1,2,after,2))
    try:
        module.decode(capture, root / "invalid-metadata")
    except ValueError as exc:
        assert "Conflicting metadata" in str(exc)
    else:
        raise AssertionError("mixed shader metadata accepted")

    # A G-channel NaN in R11G11B10 (VkFormat 122) must be attributed
    # to the isolated after region, not the before region.
    green_nan = struct.pack("<I", (0x7c1 << 11))
    after_hdr = green_nan + bytes(64*64*4 - 4)
    capture.write_bytes(b"SBMINI2\n" + record(1,1,before,2,fmt=122) +
                        record(1,3,source,2,fmt=122) +
                        record(1,2,after_hdr,2,fmt=122))
    rows3, comparisons3 = module.decode(capture, root / "decoded-hdr")
    target_after = next(row for row in rows3 if row["role"] == 2)
    assert target_after["nan_components"] == [0, 1, 0]
    assert comparisons3[0]["target_new_nan_pixels"] == 1
    assert comparisons3[0]["target_nan_pixels_before"] == 0

print("PASS: SpongeBob Mini decoder")
