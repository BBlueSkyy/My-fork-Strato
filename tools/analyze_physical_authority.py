#!/usr/bin/env python3
"""Read bounded GPU-PHYS logs. Report observations, never infer draw coverage."""
import argparse
import json
from pathlib import Path


def analyze(text):
    sessions = []
    for line in text.splitlines():
        if "[GPU-PHYS] " not in line:
            continue
        item = json.loads(line.split("[GPU-PHYS] ", 1)[1])
        if item.get("kind") == "config":
            sessions.append({"config": item, "events": []})
        elif sessions:
            sessions[-1]["events"].append(item)
        else:
            raise ValueError("event without config; log is incomplete")
    summaries = []
    for session in sessions:
        if not session["config"].get("enabled"):
            continue
        events = session["events"]
        numbered = [e for e in events if "seq" in e]
        ids = {e["image"] for e in numbered if e["kind"] == "image"}
        if len(numbered) > 64 or len(ids) > 8:
            raise ValueError("trace exceeds its promised bounds")
        if [e["seq"] for e in numbered] != list(range(1, len(numbered) + 1)):
            raise ValueError("missing or reordered records; preserve the full emulation log")
        roots = {tuple(e["root"]) for e in numbered}
        if len(roots) > 1:
            raise ValueError("family changed during capture")
        writes, observations = {}, []
        hdr, discarded, hashes = [], [], {}
        for event in numbered:
            kind, image = event["kind"], event["image"]
            if kind == "write-intent" and event["state_known"]:
                writes[image] = event
            if kind == "upload-source":
                unresolved = [i for i, w in writes.items() if i != image and w["write"] > w["readback"]]
                if unresolved:
                    observations.append({"seq": event["seq"], "kind": "upload_after_other_writer_intent",
                                         "other_images": unresolved, "verdict": "candidate_only; coverage and completion must be checked"})
            if kind == "readback-published" and image in writes and event["state_known"]:
                writes[image] = event
            if kind in ("readback-guest-before", "readback-guest-after", "upload-source") and event["sample"][1]:
                key = tuple(event["sample"][:2])
                previous = hashes.get(key)
                if previous and previous["hash"] != event["hash"]:
                    observations.append({"seq": event["seq"], "kind": "shared_sample_changed",
                                         "previous_seq": previous["seq"], "verdict": "change_only; not proof of incorrect authority"})
                hashes[key] = event
            if event["samples"]:
                hdr.append({k: event[k] for k in ("seq", "image", "kind", "samples", "nan", "excess")})
            if kind == "discard-result":
                discarded.append({k: event[k] for k in ("seq", "a", "b", "c", "result", "errno", "protection")})
        seed = numbered[0] if numbered else None
        signature = None
        if seed:
            signature = {"selector": session["config"]["selector"], "shape": seed["shape"],
                         "root_size": seed["root"][1],
                         "relative_maps": [[p - seed["root"][0], n] for p, n in seed["maps"]]}
        summaries.append({"free_guest": session["config"]["free_guest"], "events": len(numbered),
                          "images": len(ids), "family_signature": signature,
                          "sampling": session["config"].get("sampling", "original-unfiltered"),
                          "suppressed_activity": max((e.get("suppressed", 0) for e in numbered), default=0),
                          "critical_seen": max((e.get("critical_seen", 0) for e in numbered), default=0),
                          "family_relevance": "transfer-path-observed; menu link unproven" if any(e["kind"].startswith(("upload", "readback", "discard")) for e in numbered) else "no transfer evidence; relevance to menu unknown",
                          "ended": bool(numbered and numbered[-1]["kind"] in ("end", "budget_end")),
                          "lost": max((e["lost"] for e in numbered), default=0),
                          "image_limit": any(e["image_limit"] for e in numbered),
                          "mapping_truncation": any(e["maps_truncated"] for e in numbered),
                          "observations": observations, "hdr_prefix_only": hdr, "discard_results": discarded,
                          "confirmed_menu_violation": None,
                          "limitation": "Activity may be aggregated; absence of a write record is not absence of a write. Intent, pointer identity, serial and hash changes alone do not prove menu causality."})
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    result = [{"file": str(p), "sessions": analyze(p.read_text(errors="replace"))} for p in args.logs]
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
