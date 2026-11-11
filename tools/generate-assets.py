#!/usr/bin/env python3
"""Generate this project's original schematic artwork using only Python stdlib.

No source images, fonts, logos or assets from Logitech or other applications are
read. Coordinates describe generic mouse controls, not a product photograph.
The output PNGs have transparent backgrounds; ICO embeds the generated PNGs.
"""
from pathlib import Path
import math
import struct
import zlib

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets"


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


class Canvas:
    def __init__(self, width, height, scale=2):
        self.width, self.height, self.scale = width, height, scale
        self.w, self.h = width * scale, height * scale
        self.rows = [bytearray(self.w * 4) for _ in range(self.h)]

    def polygon(self, vertices, color):
        points = [(x * self.scale, y * self.scale) for x, y in vertices]
        rgba = bytes((*color, 255) if len(color) == 3 else color)
        low = max(0, math.floor(min(y for _, y in points)))
        high = min(self.h, math.ceil(max(y for _, y in points)))
        edges = list(zip(points, points[1:] + points[:1]))
        for row in range(low, high):
            y = row + .5
            crossings = sorted(x1 + (y - y1) * (x2 - x1) / (y2 - y1)
                               for (x1, y1), (x2, y2) in edges
                               if (y1 <= y < y2) or (y2 <= y < y1))
            for left, right in zip(crossings[::2], crossings[1::2]):
                a, b = max(0, math.ceil(left - .5)), min(self.w, math.ceil(right - .5))
                if b > a:
                    self.rows[row][a * 4:b * 4] = rgba * (b - a)

    def ellipse(self, box, color):
        x, y, w, h = box
        self.polygon([(x + w / 2 + w / 2 * math.cos(i * math.tau / 80),
                       y + h / 2 + h / 2 * math.sin(i * math.tau / 80)) for i in range(80)], color)

    def line(self, a, b, color, width=3):
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dy)
        if not length:
            return
        nx, ny = -dy * width / length / 2, dx * width / length / 2
        self.polygon([(a[0] + nx, a[1] + ny), (b[0] + nx, b[1] + ny),
                      (b[0] - nx, b[1] - ny), (a[0] - nx, a[1] - ny)], color)
        for x, y in (a, b):
            self.ellipse((x - width / 2, y - width / 2, width, width), color)

    def path(self, start, segments, fill, outline=None, width=2):
        points = [start]
        for p1, p2, p3 in segments:
            p0 = points[-1]
            for i in range(1, 33):
                t, u = i / 32, 1 - i / 32
                points.append((u**3 * p0[0] + 3 * u*u*t * p1[0] + 3*u*t*t*p2[0] + t**3*p3[0],
                               u**3 * p0[1] + 3 * u*u*t * p1[1] + 3*u*t*t*p2[1] + t**3*p3[1]))
        self.polygon(points, fill)
        if outline:
            for a, b in zip(points, points[1:] + points[:1]):
                self.line(a, b, outline, width)

    def pill(self, x, y, w, h, color, angle=0):
        radius = min(w, h) / 2
        points = []
        for cx, cy, start in ((x+w-radius, y+radius, -90), (x+w-radius, y+h-radius, 0),
                               (x+radius, y+h-radius, 90), (x+radius, y+radius, 180)):
            for i in range(13):
                r = math.radians(start + i * 90 / 12)
                points.append((cx + radius * math.cos(r), cy + radius * math.sin(r)))
        theta = math.radians(angle)
        cx, cy = x + w / 2, y + h / 2
        self.polygon([(cx + (a-cx)*math.cos(theta) - (b-cy)*math.sin(theta),
                       cy + (a-cx)*math.sin(theta) + (b-cy)*math.cos(theta)) for a, b in points], color)

    def png(self):
        s = self.scale
        rows = []
        for y in range(self.height):
            row = bytearray(b"\0")
            for x in range(self.width):
                pixels = [self.rows[y*s+dy][(x*s+dx)*4:(x*s+dx)*4+4] for dy in range(s) for dx in range(s)]
                total_alpha = sum(p[3] for p in pixels)
                if total_alpha:
                    row.extend(sum(p[c]*p[3] for p in pixels)//total_alpha for c in range(3))
                    row.append(total_alpha // (s*s))
                else:
                    row.extend(b"\0\0\0\0")
            rows.append(bytes(row))
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


EDGE, DARK, BODY, TOP, SILVER, ACCENT = (103, 116, 122), (31, 39, 44), (53, 65, 72), (69, 82, 89), (170, 185, 190), (0, 207, 184)


def side():
    c = Canvas(636, 1024)
    c.path((270, 40), [((370, 26), (498, 57), (549, 169)),
        ((621, 321), (624, 611), (578, 827)), ((562, 935), (490, 1000), (397, 990)),
        ((258, 975), (147, 884), (101, 741)), ((76, 679), (16, 659), (24, 570)),
        ((32, 483), (135, 430), (177, 377)), ((214, 311), (194, 76), (270, 40))], BODY, EDGE, 3)
    c.path((275, 52), [((363, 34), (494, 71), (534, 170)),
        ((579, 286), (584, 436), (582, 557)), ((481, 494), (421, 372), (368, 279)),
        ((319, 202), (291, 111), (275, 52))], TOP, EDGE, 2)
    c.path((180, 388), [((182, 476), (231, 606), (286, 715)),
        ((320, 807), (389, 909), (469, 962)), ((325, 951), (183, 854), (127, 728)),
        ((86, 658), (34, 637), (41, 570)), ((52, 490), (129, 445), (180, 388))], DARK)
    c.line((375, 70), (497, 314), DARK, 5)
    c.line((497, 314), (573, 461), DARK, 5)
    c.pill(409, 88, 65, 160, DARK, -22)
    c.pill(419, 99, 45, 139, SILVER, -22)
    for i in range(11):
        c.line((418 + i*4.5, 112+i*10.5), (451+i*4.5, 100+i*10.5), TOP, 2.5)
    c.pill(490, 303, 32, 93, DARK, -22)
    c.pill(497, 314, 18, 69, EDGE, -22)
    c.pill(316, 445, 62, 153, DARK, -22)
    c.pill(325, 455, 44, 133, SILVER, -22)
    for i in range(12):
        c.line((324+i*4, 468+i*9.2), (357+i*4, 455+i*9.2), TOP, 2)
    c.pill(205, 417, 27, 82, EDGE, -22)
    c.pill(270, 582, 28, 74, EDGE, -22)
    c.pill(42, 558, 22, 83, TOP, -14)
    c.pill(370, 632, 9, 31, ACCENT, -22)
    for i in range(8):
        c.line((133+i*15, 672+i*11), (214+i*16, 836+i*9), (45, 54, 60), 4)
    return c.png()


def upright(back=False):
    c = Canvas(692 if back else 691, 1024)
    c.path((346, 35), [((489, 35), (552, 135), (572, 296)),
        ((589, 459), (608, 621), (566, 802)), ((536, 935), (457, 990), (346, 990)),
        ((224, 990), (143, 921), (115, 802)), ((74, 624), (101, 457), (119, 296)),
        ((137, 126), (206, 35), (346, 35))], BODY if not back else DARK, EDGE, 3)
    if not back:
        c.path((346, 47), [((477, 47), (535, 146), (551, 303)),
            ((447, 344), (249, 344), (140, 303)), ((159, 147), (215, 47), (346, 47))], TOP)
        c.line((346, 48), (346, 315), DARK, 5)
        c.pill(311, 137, 70, 160, DARK)
        c.pill(324, 151, 44, 133, SILVER)
        for y in range(159, 279, 11): c.line((325, y), (367, y), TOP, 3)
        c.pill(328, 329, 36, 78, DARK)
        c.pill(341, 434, 10, 28, ACCENT)
        c.pill(99, 386, 21, 106, EDGE, -8)
        c.pill(98, 510, 20, 106, EDGE, 4)
    else:
        c.pill(175, 118, 340, 58, (132, 142, 146))
        c.pill(161, 829, 369, 62, (132, 142, 146))
        c.pill(290, 395, 112, 153, BODY)
        c.ellipse((320, 445, 52, 52), (12, 20, 24))
        c.ellipse((335, 459, 21, 21), ACCENT)
        c.pill(414, 294, 31, 76, BODY)
        c.pill(419, 300, 21, 28, ACCENT)
        c.ellipse((325, 632, 42, 42), TOP)
        for x in (301, 341, 381): c.ellipse((x, 708, 9, 9), ACCENT)
    return c.png()


def app_icon(kind, size=256):
    c = Canvas(size, size)
    s = size / 256
    if kind == "project":
        c.pill(8*s, 8*s, 240*s, 240*s, DARK)
        c.pill(70*s, 40*s, 116*s, 176*s, ACCENT)
        c.pill(77*s, 47*s, 102*s, 162*s, DARK)
        c.line((128*s, 47*s), (128*s, 113*s), ACCENT, 7*s)
        c.pill(121*s, 67*s, 14*s, 28*s, SILVER)
        c.line((77*s, 116*s), (179*s, 116*s), ACCENT, 6*s)
        c.ellipse((121*s, 171*s, 14*s, 14*s), ACCENT)
    else:
        color = {"firefox": (86, 103, 219), "chrome": (33, 146, 143), "wps": (186, 92, 88)}[kind]
        c.pill(12, 12, 232, 232, color)
        if kind == "wps":
            c.pill(68, 47, 120, 162, (239, 244, 247))
            for y in (86, 112, 138, 164): c.line((89, y), (165, y), color, 9)
        else:
            c.ellipse((55, 55, 146, 146), (239, 244, 247))
            c.ellipse((64, 64, 128, 128), color)
            c.line((60, 128), (196, 128), (239, 244, 247), 8)
            c.ellipse((96, 58, 64, 140), (239, 244, 247))
            c.ellipse((104, 62, 48, 132), color)
            c.line((104, 128), (151, 128), (239, 244, 247), 8)
    return c.png()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, data in (("side.png", side()), ("front.png", upright()), ("back.png", upright(True))):
        (OUT / name).write_bytes(data)
    for name in ("firefox", "chrome", "wps"):
        (OUT / (name + ".png")).write_bytes(app_icon(name))
    sizes = (16, 24, 32, 48, 64, 128, 256)
    images = [app_icon("project", size) for size in sizes]
    offset = 6 + len(images) * 16
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = bytearray()
    for size, data in zip(sizes, images):
        entries.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    (OUT / "loginative.ico").write_bytes(header + entries + b"".join(images))
    (OUT / "project.png").write_bytes(images[-1])
    print("Generated eight original PNG/ICO files without external assets or dependencies.")


if __name__ == "__main__":
    main()
