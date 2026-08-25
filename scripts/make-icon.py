#!/usr/bin/env python3
"""Generates Resources/icon.png, the app icon. Pure stdlib: no Pillow needed.

The motif is the signature effect — one fundamental cycle with its octave
riding on top."""

import math
import struct
import zlib
from pathlib import Path

SIZE = 1024
BG_TOP = (0x1c, 0x1d, 0x22)
BG_BOTTOM = (0x0e, 0x0f, 0x12)
EDGE = (0x3a, 0x3c, 0x45)
FUNDAMENTAL = (0xd8, 0x7b, 0x3a)
OCTAVE = (0xf3, 0xa9, 0x6a)


def clamp(x, lo=0.0, hi=1.0):
    return lo if x < lo else (hi if x > hi else x)


def over(dst, src, alpha):
    return tuple(int(round(s * alpha + d * (1.0 - alpha))) for d, s in zip(dst, src))


def rounded_box_distance(px, py, cx, cy, hx, hy, r):
    dx = abs(px - cx) - (hx - r)
    dy = abs(py - cy) - (hy - r)
    ax, ay = max(dx, 0.0), max(dy, 0.0)
    return math.hypot(ax, ay) + min(max(dx, dy), 0.0) - r


def build():
    inset = SIZE * 0.045
    cx = cy = SIZE / 2.0
    hx = hy = SIZE / 2.0 - inset
    radius = SIZE * 0.225

    left = inset + SIZE * 0.115
    right = SIZE - inset - SIZE * 0.115
    span = right - left

    # Two bands, never overlapping: the octave riding above its fundamental.
    bands = [
        dict(cycles=2.0, mid=cy - SIZE * 0.150, amp=SIZE * 0.078,
             half=SIZE * 0.028, colour=OCTAVE),
        dict(cycles=1.0, mid=cy + SIZE * 0.135, amp=SIZE * 0.110,
             half=SIZE * 0.038, colour=FUNDAMENTAL),
    ]

    # Per column: centre line and the slope correction that turns a vertical
    # distance into a perpendicular one.
    for b in bands:
        table = []
        for x in range(SIZE):
            t = (x + 0.5 - left) / span
            phase = 2.0 * math.pi * b["cycles"] * t
            y = b["mid"] + b["amp"] * math.sin(phase)
            slope = b["amp"] * 2.0 * math.pi * b["cycles"] / span * math.cos(phase)
            table.append((y, 1.0 / math.hypot(1.0, slope), t))
        b["table"] = table

        # Tangents at the two ends, so the stroke can be cut square to the
        # curve rather than square to the pixel grid.
        k = b["amp"] * 2.0 * math.pi * b["cycles"] / span
        b["startY"] = b["mid"]
        b["endY"] = b["mid"] + b["amp"] * math.sin(2.0 * math.pi * b["cycles"])
        b["startSlope"] = k * math.cos(0.0)
        b["endSlope"] = k * math.cos(2.0 * math.pi * b["cycles"])
        b["startNorm"] = math.hypot(1.0, b["startSlope"])
        b["endNorm"] = math.hypot(1.0, b["endSlope"])

    raw = bytearray()

    for y in range(SIZE):
        raw.append(0)  # PNG filter: none
        py = y + 0.5
        row = bytearray()

        for x in range(SIZE):
            px = x + 0.5
            d = rounded_box_distance(px, py, cx, cy, hx, hy, radius)
            shape = clamp(0.5 - d)

            if shape <= 0.0:
                row.extend((0, 0, 0, 0))
                continue

            g = y / SIZE
            colour = tuple(int(round(a + (b - a) * g)) for a, b in zip(BG_TOP, BG_BOTTOM))

            edge = clamp(0.5 - abs(d + 1.5)) * 0.55
            if edge > 0.0:
                colour = over(colour, EDGE, edge)

            for b in bands:
                wy, scale, t = b["table"][x]
                half = b["half"]

                # Distance to the curve, cut square to the tangent at each end.
                cover = clamp(half + 0.5 - abs(py - wy) * scale)

                if cover > 0.0:
                    u = ((px - left) + (py - b["startY"]) * b["startSlope"]) / b["startNorm"]
                    v = ((px - right) + (py - b["endY"]) * b["endSlope"]) / b["endNorm"]
                    cover *= clamp(u + 0.5) * clamp(0.5 - v)

                # Round the two ends off.
                cover = max(cover,
                            clamp(half + 0.5 - math.hypot(px - left, py - b["startY"])),
                            clamp(half + 0.5 - math.hypot(px - right, py - b["endY"])))

                if cover > 0.0:
                    colour = over(colour, b["colour"], cover)

            row.extend((colour[0], colour[1], colour[2], int(round(shape * 255))))

        raw.extend(row)

    return bytes(raw)


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))


def main():
    pixels = build()
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(pixels, 9))
           + chunk(b"IEND", b""))

    out = Path(__file__).resolve().parent.parent / "Resources" / "icon.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(png)
    print(out)


if __name__ == "__main__":
    main()
