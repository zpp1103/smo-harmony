#!/usr/bin/env python3
"""M42A / S1: verify the offscreen PPM the nx2android backend slice produced.

The picture is the repository's own triangle shader (platform/nvn_vk/shaders/tri.vert):
    pos = (-0.9,-0.9), (1.6,-0.9), (-0.9,1.6)   -> red (255,0,0)
    everything else = the render pass clear colour our selftest sets: (16,16,48)
Vulkan NDC: x right, y down on the framebuffer? No — Vulkan NDC has y pointing down, so a pixel row r
(framebuffer row 0 = top) maps to ndcY = 2*(r+0.5)/H - 1, and ndcX = 2*(c+0.5)/W - 1.

We compute the expected colour analytically for a 16x16 grid of cell centres, skip the cells whose centre
lies within EDGE_PX of a triangle edge (rasterisation of the boundary is implementation-defined), and require
max |channel error| <= 2 on every retained sample. Both classes must be present (a fully red or fully clear
image is not a pass).
"""
import sys
import numpy as np
from PIL import Image

W_EXPECT_HINT = "P6"

def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # header: P6\n<w> <h>\n<maxval>\n
    if not data.startswith(b"P6"):
        raise SystemExit("not a P6 PPM: %r" % data[:2])
    idx = 2
    fields = []
    while len(fields) < 3:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, maxv = fields
    assert maxv == 255, maxv
    buf = np.frombuffer(data[idx:idx + w * h * 3], dtype=np.uint8)
    if buf.size != w * h * 3:
        raise SystemExit("short pixel data: %d != %d" % (buf.size, w * h * 3))
    return buf.reshape(h, w, 3)

def inside(x, y, tri):
    def sign(ax, ay, bx, by, px, py):
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax)
    a, b, c = tri
    d1 = sign(a[0], a[1], b[0], b[1], x, y)
    d2 = sign(b[0], b[1], c[0], c[1], x, y)
    d3 = sign(c[0], c[1], a[0], a[1], x, y)
    return (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0)

def edge_dist(x, y, tri):
    """min distance from (x,y) to the triangle's three edge lines, in pixels"""
    best = 1e18
    for i in range(3):
        ax, ay = tri[i]
        bx, by = tri[(i + 1) % 3]
        num = abs((bx - ax) * (y - ay) - (by - ay) * (x - ax))
        den = ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
        best = min(best, num / den)
    return best

def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: m42a_ppm_check.py <in.ppm> <out.png> [clear_r,clear_g,clear_b]")
    path, png = sys.argv[1], sys.argv[2]
    clear = (16, 16, 48)
    if len(sys.argv) > 3:
        clear = tuple(int(v) for v in sys.argv[3].split(","))
    tri_col = (255, 0, 0)

    img = read_ppm(path)
    h, w, _ = img.shape
    tri = ((-0.9, -0.9), (1.6, -0.9), (-0.9, 1.6))
    # NDC -> framebuffer pixels; Vulkan NDC y points down, so row r maps to ndcY = 2*(r+0.5)/h - 1
    def to_px(p):
        return ((p[0] + 1.0) * 0.5 * w, (p[1] + 1.0) * 0.5 * h)
    tri_px = [to_px(p) for p in tri]

    EDGE_PX = 3.0
    grid = 16
    inside_n = outside_n = 0
    max_err = -1
    worst = None
    rows = []
    for gy in range(grid):
        line = []
        for gx in range(grid):
            x = (gx + 0.5) * w / grid - 0.5
            y = (gy + 0.5) * h / grid - 0.5
            if edge_dist(x, y, tri_px) < EDGE_PX:
                line.append(".")
                continue
            want = tri_col if inside(x, y, tri_px) else clear
            got = tuple(int(v) for v in img[int(round(y)), int(round(x))])
            err = max(abs(got[i] - want[i]) for i in range(3))
            if err > max_err:
                max_err = err
                worst = (gx, gy, got, want)
            if want == tri_col:
                inside_n += 1
                line.append("R" if err <= 2 else "r")
            else:
                outside_n += 1
                line.append("C" if err <= 2 else "c")
        rows.append("".join(line))

    # full-image statistics, independent of the grid
    red_px = int(np.all(np.abs(img.astype(int) - np.array(tri_col)) <= 2, axis=2).sum())
    clr_px = int(np.all(np.abs(img.astype(int) - np.array(clear)) <= 2, axis=2).sum())
    total = w * h
    other = total - red_px - clr_px

    Image.fromarray(img).save(png, "PNG", optimize=True)

    print("image          : %dx%d (%s)" % (w, h, path))
    print("grid 16x16     : R=triangle ok, r=triangle off, C=clear ok, c=clear off, .=edge skipped")
    for r in rows:
        print("  " + r)
    print("samples        : inside=%d outside=%d" % (inside_n, outside_n))
    print("max rgb error  : %d  (worst at grid %s: got %s want %s)" % (max_err, worst[:2] if worst else "-", worst[2] if worst else "-", worst[3] if worst else "-"))
    print("full-image     : red<=2:%d (%.2f%%)  clear<=2:%d (%.2f%%)  other:%d (%.3f%%)"
          % (red_px, 100.0 * red_px / total, clr_px, 100.0 * clr_px / total, other, 100.0 * other / total))
    print("png            : %s" % png)

    ok = (max_err >= 0 and max_err <= 2 and inside_n > 0 and outside_n > 0)
    print("VERDICT        : %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
