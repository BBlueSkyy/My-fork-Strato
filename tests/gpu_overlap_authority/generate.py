"""Extend the existing Vulkan harness only at its explicit Android host facades."""
def prepare(root, directory):
    common = root / 'app/src/main/cpp/skyline/common'
    def without_includes(path):
        return '\n'.join(line for line in path.read_text().splitlines()
                         if not line.startswith('#include') and line != '#pragma once')
    (directory / 'traps.inc').write_text(
        without_includes(common / 'interval_map.h') + '\n' +
        without_includes(common / 'trap_manager.h') + '\n' +
        without_includes(common / 'trap_manager.cpp'))
    base = (root / 'tests/gpu_backing_promotion/tests.cpp').read_text()
    start = base.index('            struct Memory {')
    end = base.index('        struct State {', start)
    base = base[:start] + (root / 'tests/gpu_overlap_authority/host_boundaries.inc').read_text() + '\n        };\n' + base[end:]
    base = base.replace('namespace skyline::gpu {', '#include "traps.inc"\nnamespace skyline::gpu {', 1)
    base = base.replace('int main() {', 'int unusedBackingPromotionMain() {')
    # Renamed main no longer has the language's implicit return.
    end = base.rfind('}')
    base = base[:end] + '    return 0;\n' + base[end:]
    (directory / 'backing_harness.inc').write_text(base)
