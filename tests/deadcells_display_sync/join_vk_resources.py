#!/usr/bin/env python3
"""Join PR #425 Vulkan debug names to RenderDoc resource IDs in one session.
No guessing of IDs or guest addresses from different boots.
"""
import argparse
import json
from collections import defaultdict
from pathlib import Path
from zipfile import ZipFile, is_zipfile

def read(path, suffix):
    p = Path(path)
    if is_zipfile(p):
        with ZipFile(p) as z:
            names = [n for n in z.namelist() if n.endswith(suffix)]
            if len(names) != 1:
                raise ValueError(f"Expected one {suffix}: {names}")
            return [json.loads(s) for s in z.read(names[0]).splitlines() if s.strip()]
    if p.name != suffix:
        raise ValueError(f"Expected {suffix}, got {p.name}")
    return [json.loads(s) for s in p.read_bytes().splitlines() if s.strip()]

def correlate(probe, resources, usages):
    links = [x for x in probe if x.get("type") == "vulkan_image_link"]
    by_name = defaultdict(list)
    by_image = defaultdict(list)
    for x in links:
        by_image[x.get("vk_image_handle_bits")].append(x)
        if x.get("debug_name_applied"):
            by_name[x.get("resource_name")].append(x)
    usages_by_id = {x["resource_id"]: x.get("events", []) for x in usages
                    if x.get("kind") == "texture"}
    matched = []
    for resource in resources:
        name = resource.get("name", "")
        if not name.startswith("DC425 "):
            continue
        exact = by_name.get(name, [])
        handles = {x["vk_image_handle_bits"] for x in exact}
        if len(handles) != 1:
            matched.append({"resource_id": resource.get("id"), "name": name,
                            "status": "UNRESOLVED_not_exactly_one_same_session_handle"})
            continue
        handle = next(iter(handles))
        history = sorted(by_image[handle], key=lambda x: x.get("event_seq", 0))
        matched.append({
            "resource_id": resource.get("id"), "name": name,
            "status": "LINKED_BY_EXACT_DEBUG_NAME",
            "vk_image_handle_bits": handle,
            "history": [{k: x.get(k) for k in ("role", "frame_number",
                "gpu_write_serial", "nvmap_handle", "guest_address",
                "guest_host_ptr", "possible_gpu_iovas",
                "iova_is_current_mapping_verified", "event_seq")} for x in history],
            "renderdoc_usage_events": usages_by_id.get(resource.get("id"), [])
        })
    return {"probe_vk_links": len(links),
        "linked_resources": sum(x["status"] == "LINKED_BY_EXACT_DEBUG_NAME" for x in matched),
        "status": "CORRELATION_AVAILABLE" if any(x["status"] == "LINKED_BY_EXACT_DEBUG_NAME"
             for x in matched) else "UNRESOLVED_NO_SAME_SESSION_IMAGE_LABELS",
        "warning": "GMMU IOVAs are candidates, not verified active translations. Queue and present frame IDs use separate domains. A writer declaration does not prove GPU execution.",
        "resources": matched}

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("probe", help="probe.jsonl or Strato diagnostics ZIP")
    ap.add_argument("renderdoc", help="RenderDoc AI diagnostics ZIP")
    args = ap.parse_args()
    print(json.dumps(correlate(read(args.probe, "probe.jsonl"),
                               read(args.renderdoc, "resources.jsonl"),
                               read(args.renderdoc, "resource-usage.jsonl")),
                     ensure_ascii=False, indent=2))
