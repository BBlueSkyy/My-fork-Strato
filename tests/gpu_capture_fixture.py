"""Remove opt-in mini-RenderDoc blocks from extracted host GPU fixtures.

These fixtures compile production execution code against tiny replacement GPU interfaces.
The diagnostics require full Vulkan, Texture and shader interfaces and are deliberately
not part of these behavioral tests. The APK still builds and runs the full code.
"""

CAPTURE_BLOCK = 'if (diagnostic::NarutoUiCapture().active.load(std::memory_order_acquire)) {'


def strip_capture_blocks(source: str) -> str:
    while CAPTURE_BLOCK in source:
        start = source.index(CAPTURE_BLOCK)
        cursor = start + len(CAPTURE_BLOCK)
        depth = 1
        while depth and cursor < len(source):
            depth += (source[cursor] == '{') - (source[cursor] == '}')
            cursor += 1
        if depth:
            raise ValueError('Unterminated Naruto UI diagnostic block in production source')
        source = source[:start] + source[cursor:]
    return source
