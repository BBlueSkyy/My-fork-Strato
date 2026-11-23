#!/usr/bin/env python3
"""Check the disabled renderer against the exact audit checkpoint."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE = "ec8e8b891f516482db5b3e3053fba17057ecae2d"


def disabled(text):
    stack, active, out = [], True, []
    for line in text.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b(.*)", line)
        if directive:
            op, condition = directive.groups()
            if op in ("if", "ifdef", "ifndef"):
                target = "STRATO_PHYSICAL_AUTHORITY_TRACE" in condition
                stack.append((target, active))
                if target:
                    active = active and op == "ifndef"
                    continue
            elif op in ("else", "elif") and stack[-1][0]:
                active = stack[-1][1] and not active
                continue
            elif op == "endif":
                target, previous = stack.pop()
                active = previous
                if target:
                    continue
        if active:
            out.append(line)
    text = "".join(out)
    text = re.sub(r'^\s*#include [^\n]*physical_authority_trace.h[^\n]*\n', '', text, flags=re.M)
    while match := re.search(r"GPU_PHYS_(?:TRACE|SCOPE)\s*\(", text):
        end, depth, quote = match.end(), 1, None
        while depth:
            char = text[end]
            if quote:
                if char == "\\":
                    end += 2
                    continue
                if char == quote:
                    quote = None
            elif char in ('"', "'"):
                quote = char
            elif char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
            end += 1
        while text[end:end+1].isspace():
            end += 1
        if text[end:end+1] == ';':
            end += 1
        text = text[:match.start()] + text[end:]
    # The only two added scopes surround one unchanged production statement.
    text = re.sub(r'\{\s*(texture->SynchronizeGuest\(false,\s*true\);)\s*\}', r'\1', text)
    text = re.sub(r'\{\s*(texture->texture->MarkGpuDirty\(ctx.executor.usageTracker\);)\s*\}', r'\1', text)
    text = re.sub(r'\{\s*return false;\s*\}', 'return false;', text)
    text = re.sub(r'(for \(u32 layer\{\}; layer < layerCount; \+\+layer\))\s*\{\s*(texture::CopyBlockLinearToLinear\(mip.dimensions,[\s\S]*?mip.tileWidthSpacing\);)\s*\}', r'\1 \2', text)
    return re.sub(r'\s+', '', text)


def main():
    paths = subprocess.check_output(['git', 'diff', BASE, '--name-only', '--', 'app/src/main/cpp'], cwd=ROOT, text=True).splitlines()
    checked = []
    for path in paths:
        if path.endswith('physical_authority_trace.h'):
            continue
        original = subprocess.check_output(['git', 'show', BASE + ':' + path], cwd=ROOT, text=True)
        assert disabled((ROOT/path).read_text()) == disabled(original), path
        checked.append(path)
    assert len(checked) >= 7
    print('PASS disabled source equivalence for', len(checked), 'renderer/kernel files (instrumentation-only scopes normalized)')


if __name__ == '__main__':
    main()
