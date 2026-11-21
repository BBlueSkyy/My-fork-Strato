#!/usr/bin/env python3
"""Compile the production CircularQueue against minimal host facades and test nontrivial items."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

with tempfile.TemporaryDirectory(prefix="strato-queue-") as directory:
    temporary = Path(directory)
    (temporary / "common").mkdir()
    (temporary / "common" / "trace.h").write_text(
        "#pragma once\n#define TRACE_EVENT_BEGIN(...) ((void)0)\n#define TRACE_EVENT_END(...) ((void)0)\n"
    )
    (temporary / "common" / "spin_lock.h").write_text(
        "#pragma once\n#include <mutex>\nnamespace skyline {"
        "class SpinLock { std::mutex mutex; public:"
        " void lock() { mutex.lock(); } void unlock() { mutex.unlock(); } }; }\n"
    )
    (temporary / "common" / "span.h").write_text(
        "#pragma once\n#include <span>\nnamespace skyline { template<class T> using span = std::span<T>; }\n"
    )

    binary = temporary / "queue_tests"
    command = [
        os.environ.get("CXX", "c++"), "-std=c++20", "-pthread", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror",
        "-I" + str(temporary), "-I" + str(ROOT / "app/src/main/cpp/skyline"),
        str(HERE / "queue_tests.cpp"), "-o", str(binary),
    ]
    sanitizer = os.environ.get("SANITIZE", "undefined")
    if sanitizer:
        command += ["-fsanitize=" + sanitizer, "-fno-sanitize-recover=all"]
    subprocess.run(command, check=True)
    for _ in range(int(os.environ.get("QUEUE_STRESS_RUNS", "1"))):
        subprocess.run([str(binary)], check=True, timeout=20)
