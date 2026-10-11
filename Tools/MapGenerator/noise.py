import math, random


class Noise:
    """Seeded 2D value noise with fBm."""

    def __init__(self, seed):
        r = random.Random(seed)
        self.p = [r.random() * 2 - 1 for _ in range(8192)]

    def _v(self, ix, iy):
        h = (ix * 73856093) ^ (iy * 19349663) ^ ((ix * iy) * 83492791)
        return self.p[h & 8191]

    def value(self, x, y):
        ix, iy = math.floor(x), math.floor(y)
        fx, fy = x - ix, y - iy
        sx = fx * fx * (3 - 2 * fx)
        sy = fy * fy * (3 - 2 * fy)
        a = self._v(ix, iy)
        b = self._v(ix + 1, iy)
        c = self._v(ix, iy + 1)
        d = self._v(ix + 1, iy + 1)
        return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy

    def fbm(self, x, y, octaves=4, scale=16.0, persistence=0.5):
        amp, freq, tot, norm = 1.0, 1.0 / scale, 0.0, 0.0
        for o in range(octaves):
            tot += amp * self.value(x * freq + o * 17.3, y * freq - o * 9.1)
            norm += amp
            amp *= persistence
            freq *= 2.0
        return tot / norm

    def ridged(self, x, y, octaves=3, scale=8.0):
        return 1.0 - abs(self.fbm(x, y, octaves, scale))


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    qx, qy = ax + t * dx, ay + t * dy
    return math.hypot(px - qx, py - qy), t


def poly_dist(px, py, pts):
    """Distance to polyline and arc-length position (in tiles) of the closest point."""
    best, best_s, s = 1e9, 0.0, 0.0
    for i in range(len(pts) - 1):
        (ax, ay), (bx, by) = pts[i], pts[i + 1]
        seg = math.hypot(bx - ax, by - ay)
        d, t = seg_dist(px, py, ax, ay, bx, by)
        if d < best:
            best, best_s = d, s + t * seg
        s += seg
    return best, best_s


def poly_len(pts):
    return sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]) for i in range(len(pts) - 1))


def smoothstep(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def interp(table, x):
    """Piecewise linear lookup in [(x, y), ...] sorted by x."""
    if x <= table[0][0]:
        return table[0][1]
    for (x0, y0), (x1, y1) in zip(table, table[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return table[-1][1]
