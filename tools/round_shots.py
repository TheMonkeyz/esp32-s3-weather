#!/usr/bin/env python3
"""Turn screen snapshots into round pictures with a transparent corner, for the README and the flasher site.

    python tools/round_shots.py tools/harness/reports/<run> web/flash/img [screen ...]

Reads <run>/screen_<name>.png (harness) or snapshot_<name>.png (tools/snapshot.py), undoes the red tint that marks
the area outside the round panel, and writes <out>/<name>.png as RGBA with an anti-aliased circular edge.
Default screens: weather hourly radar extras status settings. Standard library only.
"""
import os
import struct
import sys
import zlib

SCREENS = ["weather", "hourly", "radar", "extras", "status", "settings"]
SS = 4                                                           # supersampling for the edge


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f"{path}: needs 8-bit RGB/RGBA, not interlaced")
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def write_png(path, w, h, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def round_shot(src, dst):
    w, h, bpp, rows = read_png(src)
    r2, out = (w / 2) ** 2, []
    for y, line in enumerate(rows):
        o = bytearray(w * 4)
        cy = y + 0.5 - h / 2
        for x in range(w):
            r, g, b = line[x * bpp:x * bpp + 3]
            cx = x + 0.5 - w / 2
            if cx * cx + cy * cy > r2:                           # tinted by snapshot.py: r' = 90 + r / 2
                r = max(0, min(255, (r - 90) * 2))
            inside = sum((x + (i + 0.5) / SS - w / 2) ** 2 + (y + (j + 0.5) / SS - h / 2) ** 2 <= r2
                          for i in range(SS) for j in range(SS)) if abs(cx * cx + cy * cy - r2) < 4 * w else (
                SS * SS if cx * cx + cy * cy < r2 else 0)
            o[x * 4:x * 4 + 4] = bytes((r, g, b, round(255 * inside / (SS * SS))))
        out.append(o)
    write_png(dst, w, h, out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for name in sys.argv[3:] or SCREENS:
        for cand in (f"screen_{name}.png", f"snapshot_{name}.png"):
            if os.path.exists(os.path.join(src, cand)):
                round_shot(os.path.join(src, cand), os.path.join(dst, f"{name}.png"))
                print(f"{dst}/{name}.png  <- {cand}")
                break
        else:
            print(f"{name}: no screen_{name}.png or snapshot_{name}.png in {src}", file=sys.stderr)


if __name__ == "__main__":
    main()
