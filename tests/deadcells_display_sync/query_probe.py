#!/usr/bin/env python3
"""Offline query of Diagnóstico.zip/probe.jsonl (no code change, no new capture).

Prints, per `upload` record, the fields that separate "never written by the GPU"
from "GPU-written but overwritten by an upload": field names are the ones emitted by
deadcells_upload_probe.h ObserveUpload (cpu_write_trap_hits, last_gpu_write_serial,
ever_used_as_gpu_writer, aliases, guest_memory_freed).

usage: query_probe.py probe.jsonl [upload_index ...]
"""
import json
import sys
from collections import Counter

FIELDS = ("index", "guest_mapping_ptr", "texture_ptr", "cpu_write_trap_hits",
          "last_gpu_write_serial", "ever_used_as_gpu_writer", "aliases",
          "guest_memory_freed", "opaque_alpha_pixels", "tile_mode")

def main(argv):
    path, wanted = argv[1], {int(a) for a in argv[2:]}
    counts = Counter()
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            kind = record.get("type") or record.get("event") or "?"
            counts[kind] += 1
            if kind == "upload" and (not wanted or record.get("index") in wanted):
                print({key: record.get(key) for key in FIELDS})
    print("records by type:", dict(counts))

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv)
