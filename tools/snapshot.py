#!/usr/bin/env python3
"""Save a screen of the display as a PNG, rendered by the device itself (GET /api/snapshot).

    python tools/snapshot.py 192.168.1.156 status            -> snapshot_status.png
    python tools/snapshot.py 192.168.1.156 weather out.png

Screens: weather, extras, status, radar, update, alert, hourly0..hourly6, settings, settings1..settings3 (scrolled
down), phone (settings QR), setup0 / setup1 (Wi-Fi setup texts), current (the one shown). The device renders the screen
off-display, so the board isn't disturbed. Pixels outside the round panel are tinted red, so anything the
circle would cut off stands out (--square to skip). Needs the PC on the same network; the certificate is
self-signed. Standard library only.
"""
import ssl
import struct
import sys
import urllib.request
import zlib


def bmp_to_png(bmp: bytes, mask: bool = True) -> bytes:
    off, = struct.unpack_from("<I", bmp, 10)
    w, h = struct.unpack_from("<ii", bmp, 18)
    bpp, = struct.unpack_from("<H", bmp, 28)
    if bpp != 24:
        raise ValueError(f"expected a 24-bit BMP, got {bpp}")
    top_down = h < 0
    h = abs(h)
    row = (w * 3 + 3) & ~3
    raw = bytearray()
    for y in range(h):
        src = off + (y if top_down else h - 1 - y) * row
        line = bytearray(bmp[src:src + w * 3])
        line[0::3], line[2::3] = line[2::3], line[0::3]          # BGR -> RGB
        if mask:                                                 # outside the circle: tint red
            r2, cy = (w / 2) ** 2, y + 0.5 - h / 2
            for x in range(w):
                if (x + 0.5 - w / 2) ** 2 + cy * cy > r2:
                    line[3 * x] = 90 + line[3 * x] // 2
        raw += b"\0" + line

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main() -> None:
    args = [a for a in sys.argv[1:] if a != "--square"]
    if not args:
        sys.exit(__doc__)
    ip = args[0]
    screen = args[1] if len(args) > 1 else "current"
    out = args[2] if len(args) > 2 else f"snapshot_{screen}.png"
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE                              # per-device self-signed certificate
    with urllib.request.urlopen(f"https://{ip}/api/snapshot?screen={screen}", context=ctx, timeout=30) as r:
        bmp = r.read()
    with open(out, "wb") as f:
        f.write(bmp_to_png(bmp, "--square" not in sys.argv))
    print(f"{out}: {struct.unpack_from('<i', bmp, 18)[0]}x{abs(struct.unpack_from('<i', bmp, 22)[0])}, {len(bmp)} bytes received")


if __name__ == "__main__":
    main()
