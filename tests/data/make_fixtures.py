"""Regenerates the PNG test fixtures. Requires Pillow. Each fixture NAME.png has the expected
decoded pixels (8-bit RGBA, row-major) in NAME.rgba, produced by Pillow/libpng, an independent decoder."""
import random, struct, zlib, os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
random.seed(2026)

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

def write(name, mode, w, h, data, palette=None, filt=5, level=9, strategy=zlib.Z_DEFAULT_STRATEGY):
    ct = {"L": 0, "RGB": 2, "P": 3, "LA": 4, "RGBA": 6}[mode]
    bpp = {"L": 1, "RGB": 3, "P": 1, "LA": 2, "RGBA": 4}[mode]
    stride = w * bpp; raw = bytearray(); prev = bytes(stride)
    for y in range(h):
        row = data[y * stride:(y + 1) * stride]; f = filt if filt < 5 else y % 5
        raw.append(f)
        for i in range(stride):
            a = row[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            raw.append((row[i] - [0, a, b, (a + b) // 2, paeth(a, b, c)][f]) & 255)
        prev = row
    co = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
    z = co.compress(bytes(raw)) + co.flush()
    half = max(1, len(z) // 2)  # two IDAT chunks
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0))
    if palette: out += chunk(b"PLTE", bytes(palette))
    out += chunk(b"IDAT", z[:half]) + chunk(b"IDAT", z[half:]) + chunk(b"IEND", b"")
    path = os.path.join(HERE, name + ".png")
    open(path, "wb").write(out)
    open(os.path.join(HERE, name + ".rgba"), "wb").write(Image.open(path).convert("RGBA").tobytes())

def gradient(n): return bytes((i * 5 + i // 9) & 255 for i in range(n))
def noise(n): return bytes(random.randrange(256) for _ in range(n))

write("rgb_cycle_fixed", "RGB", 40, 30, gradient(40 * 30 * 3), filt=5, level=9)
write("rgba_paeth_stored", "RGBA", 33, 21, noise(33 * 21 * 4), filt=4, strategy=zlib.Z_FIXED)
write("gray_stored", "L", 17, 13, noise(17 * 13), filt=1, level=0)
write("graya_up_dynamic", "LA", 25, 25, gradient(25 * 25 * 2), filt=2, strategy=zlib.Z_RLE)
write("palette_avg", "P", 31, 19, bytes(random.randrange(16) for _ in range(31 * 19)),
      palette=[random.randrange(256) for _ in range(48)], filt=3, level=6)
def tile(w, h):  # compressible, tile-like content with some texture
    return bytes(((x // 8 + y // 8) % 2 * 90 + (x * y) % 17 + c * 40) & 255 for y in range(h) for x in range(w) for c in range(4))
write("rgba_tile_dynamic", "RGBA", 48, 48, tile(48, 48), filt=5, level=9)
write("one_pixel", "RGBA", 1, 1, bytes([10, 200, 30, 128]), filt=0)
write("rgb_wide_dynamic", "RGB", 300, 3, bytes((i // 3 % 7) * 30 for i in range(300 * 3 * 3)), filt=5, level=9)
