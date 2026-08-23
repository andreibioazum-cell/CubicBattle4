#!/usr/bin/env python3
"""BMP (24bpp, из visual_dump.c) -> PNG, без внешних зависимостей."""
import struct, sys, zlib

def bmp_to_png(src, dst):
    with open(src, 'rb') as f:
        data = f.read()
    off = struct.unpack_from('<I', data, 10)[0]
    w = struct.unpack_from('<i', data, 18)[0]
    h = struct.unpack_from('<i', data, 22)[0]
    bpp = struct.unpack_from('<H', data, 28)[0]
    assert bpp == 24, f"expected 24bpp, got {bpp}"
    row = w * 3
    pad = (4 - (row % 4)) % 4
    raw = bytearray()
    for y in range(h - 1, -1, -1):
        base = off + y * (row + pad)
        line = data[base:base + row]
        px = bytearray()
        for x in range(0, row, 3):
            b, g, r = line[x], line[x + 1], line[x + 2]
            px += bytes((r, g, b))
        raw += b'\x00' + px  # фильтр None на каждую строку
    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(dst, 'wb') as f:
        f.write(png)
    print(f"{dst}: {w}x{h}")

if __name__ == '__main__':
    for src in sys.argv[1:]:
        bmp_to_png(src, src.rsplit('.', 1)[0] + '.png')
