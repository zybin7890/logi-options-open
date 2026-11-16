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


def _clamp(x, lo=0.0, hi=1.0):
    return lo if x < lo else (hi if x > hi else x)


def _smoothstep(e0, e1, x):
    t = _clamp((x - e0) / (e1 - e0))
    return t * t * (3.0 - 2.0 * t)


def _mix_rgb(c1, c2, t):
    return (c1[0] + (c2[0] - c1[0]) * t, c1[1] + (c2[1] - c1[1]) * t, c1[2] + (c2[2] - c1[2]) * t)


def _blend(dst, src_rgb, src_a):
    a = _clamp(src_a)
    if a <= 0.0:
        return dst
    inv = 1.0 - a
    out_a = a + dst[3] * inv
    if out_a <= 1e-6:
        return (0.0, 0.0, 0.0, 0.0)
    return (
        (src_rgb[0] * a + dst[0] * dst[3] * inv) / out_a,
        (src_rgb[1] * a + dst[1] * dst[3] * inv) / out_a,
        (src_rgb[2] * a + dst[2] * dst[3] * inv) / out_a,
        out_a,
    )


def _sd_squircle(px, py, r_box, power=4.4):
    ax = abs(px) / r_box
    ay = abs(py) / r_box
    if ax < 1e-9 and ay < 1e-9:
        return -r_box
    return ((ax**power + ay**power) ** (1.0 / power) - 1.0) * r_box


def _sd_round_box(px, py, bx, by, r):
    qx = abs(px) - bx + r
    qy = abs(py) - by + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def _sd_capsule(px, py, ax, ay, bx, by, r):
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    h = _clamp((pax * bax + pay * bay) / (bax * bax + bay * bay))
    return math.hypot(pax - bax * h, pay - bay * h) - r


def _render_project_master(W=512):
    buf = bytearray(W * W * 4)
    invW = 1.0 / W
    aa = 1.4 / (W * 0.5)
    cos_t, sin_t = math.cos(math.radians(-11)), math.sin(math.radians(-11))
    hero = 1.0 / 1.10  # Slightly enlarged hero silhouette for crisp 16x16 tray & taskbar clarity.

    for iy in range(W):
        ny = (iy + 0.5) * invW * 2.0 - 1.0
        for ix in range(W):
            nx = (ix + 0.5) * invW * 2.0 - 1.0
            col = (0.0, 0.0, 0.0, 0.0)

            d_sh = _sd_squircle(nx, ny - 0.032, 0.90, 4.4)
            sh_a = (1.0 - _smoothstep(-0.02, 0.082, d_sh)) * 0.38
            col = _blend(col, (4, 8, 14), sh_a)

            d_bg = _sd_squircle(nx, ny, 0.90, 4.4)
            bg_mask = 1.0 - _smoothstep(-aa, aa, d_bg)
            if bg_mask > 0.001:
                rad = _clamp(math.hypot(nx * 0.85, (ny + 0.25) * 0.85))
                diag = _clamp((nx * 0.4 + (ny + 1.0) * 0.5) * 0.7)
                c_bg = _mix_rgb((28, 39, 54), (13, 18, 28), rad)
                c_bg = _mix_rgb(c_bg, (10, 26, 36), (1.0 - diag) * 0.28)
                glow_d = math.hypot(nx - 0.04, ny + 0.06)
                c_bg = _mix_rgb(c_bg, (0, 210, 192), math.exp(-glow_d * glow_d * 4.2) * 0.26)
                col = _blend(col, c_bg, bg_mask)

                rim = _smoothstep(-0.032, -0.004, d_bg) * (1.0 - _smoothstep(-0.004, aa, d_bg))
                rim_t = _clamp(0.55 - ny * 0.55)
                col = _blend(col, _mix_rgb((36, 56, 74), (0, 218, 200), rim_t * 0.68), rim * bg_mask * 0.84)

                hx, hy = nx * hero, (ny - 0.01) * hero
                r_dist = math.hypot(hx, hy - 0.01)
                ang = math.degrees(math.atan2(hy - 0.01, hx))
                d_ring = abs(r_dist - 0.57) - 0.040
                ring_a = 1.0 - _smoothstep(-aa, aa, d_ring)
                in_main_arc = (-158.0 <= ang <= 26.0)
                in_sub_arc = (106.0 <= ang <= 168.0)
                if (in_main_arc or in_sub_arc) and ring_a > 0.001:
                    if in_main_arc:
                        t_cap = min(_smoothstep(-158.0, -142.0, ang), 1.0 - _smoothstep(10.0, 26.0, ang))
                        grad_t = _clamp((ang + 158.0) / 184.0)
                        col = _blend(col, _mix_rgb((0, 226, 202), (56, 189, 248), grad_t), ring_a * t_cap * 0.92)
                    else:
                        t_cap = min(_smoothstep(106.0, 118.0, ang), 1.0 - _smoothstep(156.0, 168.0, ang))
                        col = _blend(col, (0, 188, 178), ring_a * t_cap * 0.50)

                mx = (hx - 0.01) * cos_t - (hy - 0.01) * sin_t
                my = (hx - 0.01) * sin_t + (hy - 0.01) * cos_t

                y_norm = _clamp((my + 0.50) / 1.02)
                half_w = 0.255 + 0.065 * math.sin(y_norm * math.pi) - 0.045 * (y_norm ** 2.2)
                thumb_flare = 0.055 * math.exp(-((my - 0.08) / 0.26) ** 2) if mx < 0 else 0.0
                left_edge = -(half_w + thumb_flare)
                right_edge = half_w - 0.015 * y_norm
                cx_m = 0.5 * (left_edge + right_edge)
                bx_m = 0.5 * (right_edge - left_edge)
                rx_m = bx_m * 0.96
                d_body = _sd_round_box(mx - cx_m, my, bx_m, 0.51, min(rx_m, 0.25))

                d_msh = _sd_round_box(mx - cx_m - 0.02, my - 0.04, bx_m + 0.02, 0.52, min(rx_m, 0.25))
                col = _blend(col, (4, 8, 12), (1.0 - _smoothstep(-0.02, 0.08, d_msh)) * 0.52)

                body_a = 1.0 - _smoothstep(-aa, aa, d_body)
                if body_a > 0.001:
                    dome = _clamp(1.0 - ((mx / 0.34) ** 2 * 0.55 + ((my + 0.10) / 0.55) ** 2 * 0.65))
                    is_upper = my < -0.02 + 0.05 * (mx / 0.28) ** 2
                    base_c = _mix_rgb((33, 41, 51), (78, 92, 108), dome * (0.85 if is_upper else 0.60))
                    spec = math.exp(-((mx + 0.08) / 0.14) ** 2 - ((my + 0.24) / 0.22) ** 2) * 0.22
                    base_c = _mix_rgb(base_c, (200, 224, 240), spec)
                    col = _blend(col, base_c, body_a)

                    if my < -0.02:
                        d_seam_v = abs(mx) - 0.008
                        seam_a = (1.0 - _smoothstep(-aa, aa, d_seam_v)) * _smoothstep(-0.48, -0.42, my)
                        col = _blend(col, (14, 19, 25), seam_a * 0.85 * body_a)
                    d_seam_h = abs(my + 0.02 - 0.045 * (mx / 0.28) ** 2) - 0.007
                    col = _blend(col, (16, 22, 29), (1.0 - _smoothstep(-aa, aa, d_seam_h)) * 0.72 * body_a)

                    m_rim = 1.0 - _smoothstep(0.0, 0.021, abs(d_body))
                    col = _blend(col, (0, 214, 196), m_rim * 0.90 * bg_mask)

                    d_sb1 = _sd_capsule(mx, my, -0.275, -0.14, -0.285, -0.02, 0.015)
                    d_sb2 = _sd_capsule(mx, my, -0.285, 0.04, -0.270, 0.16, 0.015)
                    sb_a = max(1.0 - _smoothstep(-aa, aa, d_sb1), 1.0 - _smoothstep(-aa, aa, d_sb2))
                    col = _blend(col, (150, 174, 188), sb_a * 0.78 * body_a)

                    d_well = _sd_round_box(mx, my + 0.225, 0.056, 0.145, 0.054)
                    col = _blend(col, (12, 16, 22), 1.0 - _smoothstep(-aa, aa, d_well))

                    d_wheel = _sd_round_box(mx, my + 0.225, 0.036, 0.122, 0.034)
                    wheel_a = 1.0 - _smoothstep(-aa, aa, d_wheel)
                    if wheel_a > 0.001:
                        w_shade = _clamp(1.0 - abs(mx / 0.036) * 0.55)
                        notch = 0.5 + 0.5 * math.cos((my + 0.225) * 210.0)
                        wc = _mix_rgb((138, 152, 165), (238, 246, 252), w_shade * (0.72 + 0.28 * notch))
                        col = _blend(col, wc, wheel_a)

                    d_btn = _sd_round_box(mx, my + 0.015, 0.024, 0.042, 0.022)
                    col = _blend(col, (15, 21, 28), 1.0 - _smoothstep(-aa, aa, d_btn))
                    d_dot = math.hypot(mx, my - 0.16) - 0.035
                    col = _blend(col, (0, 234, 214), 1.0 - _smoothstep(-aa, aa, d_dot))
                    d_hi = math.hypot(mx + 0.009, my - 0.151) - 0.011
                    col = _blend(col, (218, 255, 250), (1.0 - _smoothstep(-aa, aa, d_hi)) * 0.78)

            idx = (iy * W + ix) * 4
            buf[idx] = int(_clamp(col[0], 0, 255) + 0.5)
            buf[idx + 1] = int(_clamp(col[1], 0, 255) + 0.5)
            buf[idx + 2] = int(_clamp(col[2], 0, 255) + 0.5)
            buf[idx + 3] = int(_clamp(col[3] * 255.0, 0, 255) + 0.5)
    return buf


def _encode_rgba_png(w, h, buf):
    rows = [b"\0" + bytes(buf[y * w * 4:(y + 1) * w * 4]) for y in range(h)]
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def _downsample_rgba(src, W, S):
    if W == S:
        return src
    out = bytearray(S * S * 4)
    for y in range(S):
        sy0 = (y * W) // S
        sy1 = max(sy0 + 1, ((y + 1) * W) // S)
        for x in range(S):
            sx0 = (x * W) // S
            sx1 = max(sx0 + 1, ((x + 1) * W) // S)
            r = g = b = a = 0.0
            cnt = 0
            for sy in range(sy0, sy1):
                for sx in range(sx0, sx1):
                    i = (sy * W + sx) * 4
                    pa = src[i + 3]
                    r += src[i] * pa
                    g += src[i + 1] * pa
                    b += src[i + 2] * pa
                    a += pa
                    cnt += 1
            oi = (y * S + x) * 4
            if a > 0.5:
                out[oi] = min(255, int(r / a + 0.5))
                out[oi + 1] = min(255, int(g / a + 0.5))
                out[oi + 2] = min(255, int(b / a + 0.5))
                out[oi + 3] = min(255, int(a / cnt + 0.5))
    return out


_PROJECT_MASTER = None


def app_icon(kind, size=256):
    global _PROJECT_MASTER
    if kind == "project":
        if _PROJECT_MASTER is None:
            _PROJECT_MASTER = _render_project_master(512)
        return _encode_rgba_png(size, size, _downsample_rgba(_PROJECT_MASTER, 512, size))
    c = Canvas(size, size)
    if True:
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
