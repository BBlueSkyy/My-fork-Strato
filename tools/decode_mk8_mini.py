#!/usr/bin/env python3
"""Decode bounded MK8MINI1 cubemap inputs into contact sheets and numeric stats."""
import argparse
import json
import math
from pathlib import Path
import struct
from PIL import Image


def channel(bits, mantissa_bits):
    exponent = bits >> mantissa_bits
    mantissa = bits & ((1 << mantissa_bits) - 1)
    if exponent == 31:
        return math.nan if mantissa else math.inf
    return math.ldexp(mantissa / (1 << mantissa_bits) + (1 if exponent else 0), (exponent - 15) if exponent else -14)


def decode(path, output):
    output.mkdir(parents=True, exist_ok=True)
    manifest = []
    with path.open('rb') as source:
        if source.readline() != b'MK8MINI1\n':
            raise ValueError('Unsupported Mini capture header')
        while line := source.readline():
            if len(manifest) >= 16:
                raise ValueError('Too many captured images')
            values = list(map(int, line.decode('ascii').strip().split(',')))
            keys = 'phase frame id address image generation format width height levels layers tic mip view_levels layer view_layers swizzle write_serial bytes'.split()
            if len(values) != len(keys):
                raise ValueError('Invalid image header')
            row = dict(zip(keys, values))
            if row['phase'] not in (1, 3) or row['format'] != 122 or row['width'] != 32 or row['height'] != 32 or row['layers'] != 6 or not 1 <= row['levels'] <= 6:
                raise ValueError('Unexpected cubemap layout')
            expected = sum(max(1, 32 >> level)**2 * 4 * 6 for level in range(row['levels']))
            if row['bytes'] != expected:
                raise ValueError('Size differs from packed mip/layer layout')
            data = source.read(expected)
            if len(data) != expected or source.read(1) != b'\n':
                raise ValueError('Truncated image payload')
            words = struct.unpack('<' + 'I' * (expected // 4), data)
            rgb = [(channel(w & 2047, 6), channel((w >> 11) & 2047, 6), channel(w >> 22, 5)) for w in words]
            finite = [v for pixel in rgb for v in pixel if math.isfinite(v)]
            row.update(zero_texels=sum(all(v == 0 for v in pixel) for pixel in rgb), texels=len(rgb), nonfinite=sum(not math.isfinite(v) for pixel in rgb for v in pixel), minimum=min(finite) if finite else None, maximum=max(finite) if finite else None)
            # Fixed tonemapping across both phases. Magenta exposes nonfinite input.
            def color(pixel):
                if any(not math.isfinite(v) for v in pixel):
                    return (255, 0, 255)
                return tuple(round(255 * (max(v, 0) / (1 + max(v, 0)))**(1/2.2)) for v in pixel)
            sheet = Image.new('RGB', (32 * 6, 32))
            for face in range(6):
                image = Image.new('RGB', (32, 32))
                image.putdata([color(p) for p in rgb[face*1024:(face+1)*1024]])
                sheet.paste(image, (face*32, 0))
            name = f"phase{row['phase']}-id{row['id']}-tic{row['tic']}.png"
            sheet.resize((768, 128), Image.Resampling.NEAREST).save(output / name)
            (output / name.replace('.png', '.bin')).write_bytes(data)
            row['preview'] = name
            manifest.append(row)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, allow_nan=False))
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    rows = decode(args.capture, args.output)
    print(f'Decoded {len(rows)} captured cubemaps')
