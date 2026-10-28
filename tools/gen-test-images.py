#!/usr/bin/env python3
"""Generates the images tests/test_image.c decodes: PNGs of every colour type
and bit depth with every filter, TGAs and PNMs, from the standard library
only. Usage: gen-test-images.py <output_dir>"""
import os, struct, sys, zlib, random

def png(w, h, rows, color, depth=8, palette=None, trns=None):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(bytes([f]) + r for f, r in rows)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, 0))
    if palette: out += chunk(b"PLTE", palette)
    if trns: out += chunk(b"tRNS", trns)
    return out + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")

def filt(rows, bpp, filters):
    out, prev = [], None
    for i, r in enumerate(rows):
        f = filters[i % len(filters)]
        r = bytearray(r); o = bytearray(len(r))
        for x in range(len(r)):
            a = r[x - bpp] if x >= bpp else 0
            b = prev[x] if prev else 0
            c = prev[x - bpp] if (prev and x >= bpp) else 0
            if f == 0: o[x] = r[x]
            elif f == 1: o[x] = (r[x] - a) & 255
            elif f == 2: o[x] = (r[x] - b) & 255
            elif f == 3: o[x] = (r[x] - ((a + b) >> 1)) & 255
            else:
                p = a + b - c; pa = abs(p - a); pb = abs(p - b); pc = abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                o[x] = (r[x] - pr) & 255
        out.append((f, bytes(o))); prev = r
    return out

def main(d):
    os.makedirs(d, exist_ok=True)
    W, H = 5, 4
    w = lambda name, data: open(os.path.join(d, name), "wb").write(data)
    rgba = [bytes(sum([[x * 50, y * 60, (x + y) * 20, 255 if (x + y) % 2 == 0 else 128] for x in range(W)], [])) for y in range(H)]
    w("rgba.png", png(W, H, filt(rgba, 4, [0, 1, 2, 3, 4]), 6))
    rgb = [bytes(sum([[x * 50, y * 60, 7] for x in range(W)], [])) for y in range(H)]
    w("rgb.png", png(W, H, filt(rgb, 3, [4, 3, 2, 1]), 2))
    gray = [bytes([x * 40 + y for x in range(W)]) for y in range(H)]
    w("gray.png", png(W, H, [(0, r) for r in gray], 0))
    rgb16 = [b"".join(struct.pack(">HHH", x * 12000, y * 15000, 999) for x in range(W)) for y in range(H)]
    w("rgb16.png", png(W, H, [(0, r) for r in rgb16], 2, 16))
    pal = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9])
    rows = []
    for y in range(H):
        bits = "".join(format((x + y) % 4, "02b") for x in range(W))
        bits += "0" * ((8 - len(bits) % 8) % 8)
        rows.append((0, bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))))
    w("pal2.png", png(W, H, rows, 3, 2, palette=pal, trns=bytes([255, 200, 0, 255])))
    ga = [bytes(sum([[x * 30, y * 70] for x in range(W)], [])) for y in range(H)]
    w("ga.png", png(W, H, [(0, r) for r in ga], 4))
    random.seed(3)
    big = [bytes(random.choice([0, 10, 200, 255]) for _ in range(64 * 4)) for _ in range(64)]
    w("big.png", png(64, 64, filt(big, 4, [1, 2, 3, 4]), 6))
    hdr = lambda t, bits, desc: bytes([0, 0, t, 0, 0, 0, 0, 0, 0, 0, 0, 0, W & 255, W >> 8, H & 255, H >> 8, bits, desc])
    raw = b"".join(bytes([(x + y) * 20 & 255, y * 60 & 255, x * 50 & 255, 128 + x]) for y in reversed(range(H)) for x in range(W))
    w("bgra.tga", hdr(2, 32, 0) + raw)
    rle = b"".join(bytes([0x80 | (W - 1)]) + bytes([1, 2, 3 + y]) for y in range(H))
    w("rle.tga", hdr(10, 24, 0x20) + rle)
    w("rgb.ppm", b"P6\n# c\n5 4\n255\n" + b"".join(bytes([x * 50, y * 60, 7]) for y in range(H) for x in range(W)))
    w("gray.pgm", b"P5 5 4 255\n" + bytes(x * 40 + y for y in range(H) for x in range(W)))
    w("deflate_stored.z", zlib.compress(b"hello hello hello hello", 0))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "test_images")
