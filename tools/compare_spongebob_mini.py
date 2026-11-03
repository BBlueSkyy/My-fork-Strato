#!/usr/bin/env python3
"""Compare two SBMINI2 boots only when their draw identities actually match."""
import argparse
import tempfile
from pathlib import Path
from decode_spongebob_mini import decode


def signature(op, snapshots):
    before = next(row for row in snapshots if row["operation"] == op["operation"] and row["role"] == 1)
    return (op["pipeline_hash"], tuple(op["shader_hashes"]), op["draw_kind"],
            op["topology"], op["indexed"], op["first"], op["count"], op["instances"],
            before["host_format"], before["width"], before["height"])


def compare(a: Path, b: Path):
    with tempfile.TemporaryDirectory() as work:
        root = Path(work)
        inputs1, ops1 = decode(a, root / "a")
        inputs2, ops2 = decode(b, root / "b")
        if any("pipeline_hash" not in op for op in ops1 + ops2):
            print("SBMINI1 has no shader identity: do not compare unmatched boots.")
            return False
        seen1 = {signature(op, inputs1): op for op in ops1}
        seen2 = {signature(op, inputs2): op for op in ops2}
        common = sorted(set(seen1) & set(seen2), key=str)
        print(f"Boot 1: {len(ops1)} draws; Boot 2: {len(ops2)} draws; matching draw signatures: {len(common)}")
        if not common:
            print("INCONCLUSIVE: no matching pipeline/shader/draw/target signature.")
            return False
        for sig in common:
            op1, op2 = seen1[sig], seen2[sig]
            print(f"match pipeline={sig[0]} shaders={[x for x in sig[1] if x != '0000000000000000']}")
            print(f"  boot1 op={op1['operation']} target new NaNs={op1.get('target_new_nan_pixels','unknown')}")
            print(f"  boot2 op={op2['operation']} target new NaNs={op2.get('target_new_nan_pixels','unknown')}")
            for role in range(3, 7):
                x = next((i for i in inputs1 if i["operation"] == op1["operation"] and i["role"] == role), None)
                y = next((i for i in inputs2 if i["operation"] == op2["operation"] and i["role"] == role), None)
                if x and y and (x["host_format"], x["width"], x["height"], x["mip"], x["layer"]) == (y["host_format"], y["width"], y["height"], y["mip"], y["layer"]):
                    tic_match = (x["tic_iova"] == y["tic_iova"] and x["tic_index"] == y["tic_index"] and
                                 x["matching_tics"] == y["matching_tics"] == 1)
                    if tic_match:
                        print(f"  role{role}: SAME unambiguous TIC/IOVA; bytes_equal={x['sha256']==y['sha256']}")
                    else:
                        print(f"  role{role}: compatible view shape, TIC not uniquely matched; bytes equality inconclusive")
                else:
                    print(f"  role{role}: absent or mismatched view/format; skip comparison")
        return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("boot1", type=Path)
    parser.add_argument("boot2", type=Path)
    args = parser.parse_args()
    compare(args.boot1, args.boot2)
