#!/usr/bin/env python3
"""Build all arms on one runner: incremental native builds, one signing key.

Use only a dedicated checkout. Generated sources are restored after every arm.
Successful artifacts survive an unrelated arm's failure.
"""
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import zipfile

import prepare

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "pr341-artifacts"


def verify(apk, variant):
    assert apk.is_file() and apk.stat().st_size, "No Release APK"
    with zipfile.ZipFile(apk) as z:
        libraries = [n for n in z.namelist() if n.startswith("lib/") and n.endswith(".so")]
        assert libraries and all(n.split("/")[1] == "arm64-v8a" for n in libraries), libraries
        for name in libraries:
            h = z.read(name)[:20]
            assert h[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", h, 18)[0] == 183, name
        assert any(b"PR341_AB_VARIANT" in z.read(name) and variant.encode() in z.read(name) for name in libraries), "APK native code has no expected variant marker (stale build?)"
    sdk = pathlib.Path(os.environ.get("ANDROID_HOME") or os.environ["ANDROID_SDK_ROOT"])
    signers = sorted(sdk.glob("build-tools/*/apksigner"))
    assert signers, "Android build-tools apksigner missing"
    result = subprocess.check_output([str(signers[-1]), "verify", "--print-certs", str(apk)], text=True)
    digest = re.search(r"Signer #1 certificate SHA-256 digest: ([0-9a-fA-F]+)", result)
    assert digest, result
    return digest.group(1).lower()


def main():
    if (ROOT / prepare.HEADER).exists():
        raise RuntimeError("Use a pristine, dedicated checkout")
    original = prepare.load_original(ROOT)
    OUT.mkdir(exist_ok=False)
    results = {}
    certificate = None
    pr = os.environ.get("PR_NUMBER", "local")
    for variant in prepare.VARIANTS:
        arm = OUT / variant
        try:
            subprocess.run([sys.executable, str(ROOT / "experiments/pr341/prepare.py"),
                            "--source", str(ROOT), "--variant", variant, "--output", str(arm)], check=True)
            subprocess.run(["./gradlew", "--no-daemon", "--stacktrace", "--parallel", "--configure-on-demand",
                            "--build-cache", "assembleDevRelease"], cwd=ROOT, check=True)
            apk = ROOT / "app/build/outputs/apk/dev/release/app-dev-release.apk"
            current_certificate = verify(apk, variant)
            if certificate is not None and certificate != current_certificate:
                raise RuntimeError("A/B APK certificates differ; update-install comparison is unsafe")
            certificate = current_certificate
            filename = f"strato-pr{pr}-{variant}-arm64-release.apk"
            destination = arm / filename
            shutil.copy2(apk, destination)
            (arm / "apk-sha256.txt").write_text(hashlib.sha256(destination.read_bytes()).hexdigest() + "  " + filename + "\n")
            (arm / "certificate-sha256.txt").write_text(certificate + "\n")
            results[variant] = {"status": "success", "apk": filename, "certificate_sha256": certificate}
            print("VERIFIED", variant, filename, flush=True)
        except Exception as error:
            results[variant] = {"status": "failure", "error": str(error)}
            print("FAILED", variant, str(error), file=sys.stderr, flush=True)
        finally:
            # Only these exact sources were changed by prepare.py. Keep generated
            # Gradle/native caches to make subsequent arms incremental.
            for path, content in original.items():
                (ROOT / path).write_text(content)
            (ROOT / prepare.HEADER).unlink(missing_ok=True)
            (OUT / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if any(r["status"] != "success" for r in results.values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
