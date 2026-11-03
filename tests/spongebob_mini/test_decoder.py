#!/usr/bin/env python3
import importlib.util
import tempfile
from pathlib import Path

module_path = Path(__file__).resolve().parents[2] / "tools" / "decode_spongebob_mini.py"
spec = importlib.util.spec_from_file_location("decode_spongebob_mini", module_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def record(op, role, data):
    values = [op,role,42,7,15,37,37,512,512,224,224,64,64,0,0,1,1,4,len(data)]
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
print("PASS: SpongeBob Mini decoder")
