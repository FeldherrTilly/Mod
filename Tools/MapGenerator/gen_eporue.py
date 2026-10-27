"""Eporue (Europe backwards): flipped Europe map generator for WTP (draft stage: land, relief, climate, terrain, features -> ASCII + PNG).

Europe is turned by 180 degrees: the Atlantic lies in the east (Europe side of the game), Scandinavia in the south,
the Mediterranean (with a strip of North Africa) in the north. The real coastline is drawn in lon/lat and projected
onto the tile grid; climate is a free (warmer) variant. Python 3, standard library only.

    python Tools/MapGenerator/gen_eporue.py [seed] [output dir]
"""
import sys, os, math, random, heapq
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from noise import Noise, poly_dist, smoothstep, interp
from png import write_png

W, H = 107, 125
XOFF = 19                                 # tiles added on the left (compressed Asia gets room); the top got 15 rows as well
SEED = int(sys.argv[1]) if len(sys.argv) > 1 else 1
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out_eporue')
rng = random.Random(SEED)


class Shifted:
    """Noise sampled relative to the old left edge, so the terrain of the first draft stays where it was."""

    def __init__(self, n):
        self.n = n

    def fbm(self, x, y, *a, **k):
        return self.n.fbm(x - XOFF, y, *a, **k)


NZ = [Shifted(Noise(SEED * 1000 + i)) for i in range(24)]

N4 = [(1, 0), (-1, 0), (0, 1), (0, -1)]
N8 = N4 + [(1, 1), (1, -1), (-1, 1), (-1, -1)]


def I(x, y):
    return y * W + x


def inb(x, y):
    return 0 <= x < W and 0 <= y < H


def nbrs(x, y, d=N8):
    for dx, dy in d:
        nx, ny = x + dx, y + dy
        if 0 <= nx < W and 0 <= ny < H:
            yield nx, ny


ALL = [(x, y) for y in range(H) for x in range(W)]

# ---------------------------------------------------------------- projection (rotated by 180 degrees)
LON_MAX, LAT_MAX = 47.0, 71.5          # reference of the first draft (old tile grid: 1.1 tiles per degree of longitude, 2.2 per degree of latitude)
LAT_SHIFT = 4.0                        # free climate: everything counts as 4 degrees further north (a little warmer)
# west and central Europe get the most tiles per degree, Russia, Asia, Scandinavia and Africa are compressed
LON_KN = [(60.0, 4.5), (38.0, 25.7), (26.0, 35.3), (-10.0, 87.5), (-14.0, 91.5)]      # lon -> x
LAT_KN = [(71.5, 5.0), (58.0, 28.0), (40.0, 82.0), (34.0, 94.0), (22.7, 121.0)]       # lat -> y


def _pw(knots, v):
    for k in range(len(knots) - 1):
        if v >= knots[k + 1][0] or k == len(knots) - 2:
            (a, fa), (b, fb) = knots[k], knots[k + 1]
            return fa + (v - a) * (fb - fa) / (b - a)


def _pw_inv(knots, t):
    for k in range(len(knots) - 1):
        if t <= knots[k + 1][1] or k == len(knots) - 2:
            (a, fa), (b, fb) = knots[k], knots[k + 1]
            return a + (t - fa) * (b - a) / (fb - fa)


def to_xy(lon, lat):
    return (_pw(LON_KN, lon), _pw(LAT_KN, lat))


def from_xy(x, y):
    return (_pw_inv(LON_KN, x), _pw_inv(LAT_KN, y))


def real_lat(y):
    return _pw_inv(LAT_KN, y + 0.5)


def T2L(x, y):                         # tile coordinates of the first draft -> (lon, lat)
    return (LON_MAX - (x - XOFF) / 1.1, LAT_MAX - (y - 5.0) / 2.2)


def T2X(x, y):                         # tile coordinates of the first draft -> tile coordinates now
    return to_xy(*T2L(x, y))


def P(pts):
    return [to_xy(lo, la) for lo, la in pts]


# mainland: Spain, France, Britain's channel coast, Scandinavia, Russia (cut at the left edge), Balkans, Italy, Anatolia, Levant
MAINLAND = P([
    (-5.6, 36.0), (-6.3, 36.5), (-7.4, 37.2), (-9.0, 37.0), (-9.4, 38.5), (-8.8, 40.2), (-8.7, 41.8), (-9.3, 43.0), (-8.0, 43.7),
    (-5.5, 43.6), (-3.0, 43.4), (-1.8, 43.4), (-1.2, 44.5), (-1.2, 46.0), (-2.2, 47.2), (-4.4, 47.8), (-4.8, 48.4), (-3.0, 48.8),
    (-1.6, 48.6), (-1.5, 49.7), (0.6, 49.4), (2.0, 50.0), (2.4, 50.9), (3.5, 51.3), (4.2, 51.6), (4.7, 52.8), (5.2, 53.2),
    (7.0, 53.4), (8.5, 53.6), (8.7, 54.6), (8.1, 55.5), (8.2, 56.9), (8.6, 57.3), (10.0, 57.7), (10.6, 57.75),
    (11.9, 57.7), (11.2, 59.0), (10.6, 59.7), (9.0, 58.7), (8.0, 58.1), (7.0, 58.0), (5.7, 58.9), (5.2, 60.4), (5.0, 61.5),
    (5.0, 62.0), (6.5, 62.8), (8.0, 63.4), (9.5, 63.7), (10.8, 64.2), (12.5, 65.7), (13.8, 67.0), (14.4, 67.3), (15.5, 68.3),
    (17.0, 69.0), (18.9, 69.7), (21.0, 70.2), (24.0, 71.0), (25.8, 71.1), (28.5, 70.9), (31.0, 70.3), (33.0, 69.5),
    (36.0, 69.0), (38.0, 67.4), (41.0, 67.2), (46.0, 67.5), (51.0, 68.3), (55.0, 68.8), (58.2, 67.6), (55.0, 65.0), (56.5, 61.0), (58.4, 57.5), (55.8, 54.0), (57.8, 50.0), (58.4, 46.0), (56.0, 43.0),
    (57.8, 40.0), (58.4, 36.0), (56.5, 33.0), (58.0, 30.0), (57.4, 26.5), (57.0, 23.3), (53.0, 23.7), (49.0, 24.3), (45.0, 23.2), (41.5, 23.4), (40.0, 24.2),
    (38.2, 25.4), (36.6, 27.4), (35.4, 28.8), (34.9, 29.6), (34.3, 30.0), (34.3, 31.3), (34.9, 32.5),
    (35.5, 34.0), (35.8, 35.5), (36.2, 36.3), (34.6, 36.7), (32.5, 36.1), (30.7, 36.5), (29.0, 36.5), (28.0, 36.8),
    (27.2, 37.4), (27.2, 38.4), (26.5, 39.0), (26.2, 40.0), (24.5, 40.5), (23.8, 40.0), (23.5, 40.3), (22.9, 40.6),
    (22.7, 39.5), (23.2, 38.5), (24.0, 38.0), (23.0, 37.5), (22.8, 36.5), (22.5, 36.5), (21.7, 36.8), (21.2, 37.7),
    (21.4, 38.4), (20.5, 38.9), (20.0, 39.7), (19.4, 40.4), (19.5, 41.6), (18.5, 42.4), (17.0, 43.2), (15.2, 44.1),
    (14.5, 45.2), (13.7, 45.6), (13.5, 45.7), (12.3, 45.4), (12.4, 44.4), (13.6, 43.6), (14.5, 42.4), (16.0, 41.9),
    (16.2, 41.9), (17.2, 40.9), (18.5, 40.1), (17.0, 40.4), (16.5, 39.6), (17.1, 39.0), (16.5, 38.0), (15.7, 37.9),
    (15.7, 38.5), (16.1, 39.4), (15.7, 40.0), (14.8, 40.6), (14.0, 40.8), (13.0, 41.3), (12.3, 41.7), (11.0, 42.4),
    (10.5, 43.0), (10.3, 43.8), (9.0, 44.4), (7.6, 43.8), (6.8, 43.5), (6.0, 43.1), (4.8, 43.4), (4.5, 43.4), (3.2, 43.2),
    (3.2, 42.3), (3.0, 41.9), (2.1, 41.3), (0.8, 40.7), (-0.3, 39.5), (0.2, 38.8), (-0.5, 38.2), (-0.8, 37.6), (-2.0, 36.8),
    (-2.4, 36.7), (-4.4, 36.7), (-5.4, 36.1),
])
# North Africa: Atlas coast and the Sahara up to the map edge
AFRICA = P([
    (-12.0, 31.0), (-9.8, 31.5), (-9.2, 32.5), (-8.5, 33.4), (-6.8, 34.1), (-6.0, 35.1), (-4.0, 35.2), (-2.5, 35.3), (-1.0, 35.5),
    (0.5, 35.9), (1.0, 36.3), (3.0, 36.8), (5.0, 36.8), (7.0, 37.0), (9.0, 37.2), (10.2, 37.3), (10.9, 36.9), (10.6, 36.0),
    (10.3, 34.5), (10.1, 33.9), (11.1, 33.3), (12.5, 32.8), (13.5, 32.8), (15.0, 32.3), (16.6, 31.2), (18.5, 30.3), (19.8, 30.3),
    (20.1, 32.0), (22.0, 32.9), (22.5, 32.9), (23.5, 32.3), (25.1, 31.6), (27.0, 31.3), (29.0, 30.9), (30.0, 31.2), (31.0, 31.6),
    (32.2, 31.3), (34.0, 31.2), (34.3, 31.3), (44.0, 31.0), (44.0, 22.8), (35.0, 22.6), (30.0, 24.2), (24.0, 22.9), (19.0, 24.6), (14.0, 23.0), (9.0, 24.5), (4.0, 22.9), (-2.0, 24.8), (-8.0, 23.4),
    (-12.0, 24.0), (-12.8, 25.5), (-11.6, 27.0), (-12.4, 28.8), (-11.8, 30.2),
])
ISLANDS = {
    'Great Britain': P([(-5.7, 50.0), (-4.0, 50.3), (-1.5, 50.7), (0.2, 50.8), (0.8, 51.2), (1.0, 52.7), (0.0, 52.9), (-0.2, 53.6),
                        (-0.2, 54.3), (-1.5, 55.0), (-2.0, 55.8), (-3.0, 56.0), (-2.0, 57.5), (-3.5, 57.7), (-3.0, 58.6),
                        (-5.0, 58.6), (-5.8, 57.5), (-6.0, 56.5), (-5.0, 55.0), (-3.5, 54.9), (-3.0, 53.6), (-4.5, 53.3),
                        (-4.2, 52.5), (-5.2, 51.8), (-3.0, 51.5), (-4.5, 51.2)]),
    'Ireland': P([(-10.0, 51.7), (-8.0, 51.6), (-6.4, 52.2), (-6.2, 53.5), (-5.8, 54.5), (-6.2, 55.2), (-7.5, 55.3), (-8.5, 54.5),
                  (-10.0, 54.1), (-10.3, 52.5)]),
    'Zealand': P([(11.0, 55.2), (12.0, 55.1), (12.6, 55.6), (12.2, 56.1), (11.2, 56.0)]),
    'Funen': P([(9.8, 55.0), (10.8, 55.0), (10.9, 55.6), (10.0, 55.6)]),
    'Gotland': P([(18.0, 57.0), (19.0, 57.0), (19.4, 57.7), (18.8, 58.1), (18.2, 57.6)]),
    'Sicily': P([(12.4, 37.8), (13.5, 38.2), (15.2, 38.3), (15.3, 37.4), (15.1, 36.7), (14.0, 37.0), (12.5, 37.5)]),
    'Sardinia': P([(8.2, 39.0), (9.7, 39.0), (9.8, 41.0), (9.2, 41.2), (8.3, 40.8), (8.2, 40.0)]),
    'Corsica': P([(8.5, 41.5), (9.6, 41.5), (9.6, 42.8), (9.0, 43.1), (8.5, 42.6)]),
    'Crete': P([(23.5, 35.0), (25.5, 35.0), (26.3, 35.2), (26.0, 35.7), (24.0, 35.7), (23.5, 35.5)]),
    'Cyprus': P([(32.3, 34.7), (33.8, 34.6), (34.6, 35.2), (33.5, 35.5), (32.4, 35.2)]),
    'Mallorca': P([(2.4, 39.3), (3.4, 39.3), (3.5, 39.9), (2.5, 39.8)]),
    'Menorca': P([(3.8, 39.8), (4.3, 39.8), (4.3, 40.1), (3.8, 40.1)]),
    # fantasy: the Aegean as an island sea of settleable islands, plus a volcanic island
    'Lesbos': P([(25.5, 39.0), (26.5, 39.0), (26.6, 39.5), (25.6, 39.5)]),
    'Naxos': P([(24.6, 37.0), (25.6, 37.0), (25.8, 37.7), (24.8, 37.8)]),
    'Euboea': P([(23.2, 38.2), (24.3, 38.2), (24.6, 38.9), (23.6, 39.0)]),
    'Rhodes': P([(27.7, 36.0), (28.4, 36.0), (28.4, 36.6), (27.7, 36.5)]),
    'Santorini': P([(25.0, 36.2), (25.9, 36.2), (26.0, 36.7), (25.1, 36.7)]),
    'Canaries': P([(-18.2, 28.0), (-17.0, 28.0), (-16.8, 28.6), (-17.8, 28.8)]),
    'Fuerteventura': P([(-14.2, 28.2), (-13.5, 28.2), (-13.6, 28.9), (-14.2, 28.8)]),
}
for _name in ('Lesbos', 'Naxos', 'Euboea', 'Rhodes', 'Santorini', 'Mallorca', 'Cyprus', 'Corsica'):     # the Aegean as a real island sea: fantasy-sized islands
    _pts = ISLANDS[_name]
    _cx, _cy = sum(p[0] for p in _pts) / len(_pts), sum(p[1] for p in _pts) / len(_pts)
    _f = 1.2 if _name in ('Cyprus', 'Corsica', 'Mallorca') else 1.5
    ISLANDS[_name] = [(_cx + (px - _cx) * _f, _cy + (py - _cy) * _f) for px, py in _pts]
# seas inside the mainland polygon
SEAS = {
    'Baltic': P([(12.8, 55.6), (12.9, 55.5), (14.3, 55.5), (15.5, 56.1), (16.4, 56.6), (16.6, 57.5), (16.7, 58.5), (18.2, 59.3),
                 (18.7, 60.2), (17.2, 61.0), (17.3, 62.3), (18.5, 63.0), (20.3, 63.8), (21.5, 64.7), (22.5, 65.8), (25.3, 65.6),
                 (25.4, 65.0), (24.5, 64.9), (22.3, 63.9), (21.4, 63.0), (21.3, 61.6), (22.0, 60.4), (23.0, 59.9), (25.0, 60.2),
                 (28.0, 60.5), (30.1, 59.9), (28.0, 59.5), (25.5, 59.5), (23.4, 59.1), (24.4, 58.2), (24.3, 57.3), (23.5, 57.1),
                 (21.6, 57.5), (21.1, 56.9), (21.0, 55.4), (19.5, 54.5), (18.7, 54.8), (16.2, 54.3), (14.3, 53.9), (12.5, 54.2),
                 (11.2, 54.0), (10.9, 54.6), (11.5, 55.1), (12.2, 55.4)]),
    'Kattegat': P([(10.2, 57.7), (12.0, 57.6), (12.9, 56.2), (12.7, 55.5), (12.1, 55.3), (11.0, 55.4), (10.0, 56.0), (9.9, 56.9)]),
    'Caspian': P([(47.5, 46.8), (49.2, 46.6), (51.5, 45.5), (53.3, 44.2), (52.2, 42.0), (53.0, 40.2), (54.0, 38.6), (53.2, 37.0), (50.5, 36.8),
                  (49.0, 37.8), (49.2, 39.6), (47.8, 41.8), (47.3, 43.5), (46.8, 45.2)]),
    'Persian Gulf': P([(47.8, 29.8), (49.4, 30.0), (50.6, 28.8), (51.8, 27.2), (53.6, 26.0), (56.0, 25.8), (57.4, 24.4), (57.4, 21.0), (46.0, 21.0),
                       (46.0, 26.0), (47.0, 28.0)]),
    'Red Sea': P([(32.6, 29.9), (33.6, 28.6), (35.4, 26.4), (37.2, 23.8), (39.0, 21.0), (43.0, 21.0), (40.4, 22.9), (38.6, 25.0), (37.0, 27.2),
                  (35.5, 28.7), (34.4, 29.9)]),
    'Black Sea': P([(29.1, 41.3), (28.1, 41.9), (28.0, 43.2), (28.6, 44.2), (29.6, 45.2), (30.7, 46.4), (31.5, 46.6), (32.5, 46.0),
                    (33.0, 44.7), (34.0, 44.5), (35.0, 44.8), (36.5, 45.3), (35.8, 46.2), (37.5, 46.6), (38.8, 47.2), (39.2, 46.6),
                    (38.0, 46.0), (37.8, 44.9), (39.7, 43.6), (41.6, 42.0), (41.6, 41.6), (41.0, 41.0), (38.0, 40.9),
                    (36.3, 41.3), (35.1, 42.0), (33.5, 42.0), (31.5, 41.2)]),
}
EASTSEA = P([(33.0, 80.0), (33.0, 70.0), (35.0, 67.5), (36.0, 65.0), (34.5, 62.5), (33.5, 60.0), (32.0, 57.5), (32.6, 54.5), (31.8, 52.0),
             (32.5, 50.0), (34.5, 48.7), (37.0, 48.0), (40.0, 47.8), (44.0, 48.5), (48.0, 48.8), (53.0, 49.5), (58.0, 50.5), (80.0, 50.5), (80.0, 80.0)])
CHANNELS = [    # (polyline lon/lat, half width in tiles)
    ([(10.4, 57.9), (10.2, 57.3)], 0.9),                                       # Skagerrak into the Kattegat
    ([(11.2, 55.3), (10.8, 54.9), (10.6, 54.6)], 0.0),                         # no-op placeholder
    ([(29.1, 41.3), (29.0, 40.8), (27.5, 40.7), (26.3, 40.3), (25.6, 40.0), (25.0, 39.2), (24.7, 38.4)], 0.95),  # Bosporus, Marmara, Dardanelles
    ([(-5.4, 36.2), (-5.7, 35.5)], 1.2),                                       # Gibraltar
]

# ---------------------------------------------------------------- land mask
WARP = [NZ[0], NZ[1]]


def inside(poly, px, py):
    c = False
    n = len(poly)
    j = n - 1
    for k in range(n):
        xi, yi = poly[k]
        xj, yj = poly[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            c = not c
        j = k
    return c


CH = [([to_xy(lo, la) for lo, la in pts], hw) for pts, hw in CHANNELS]
EDGE = [Shifted(Noise(SEED * 1000 + 30 + i)) for i in range(3)]
land = [False] * (W * H)
for x, y in ALL:
    px = x + 0.5 + 1.5 * WARP[0].fbm(x, y, 4, 9) + 1.4 * WARP[0].fbm(x + 13, y, 3, 4)      # fractal coastline: bays, headlands, ragged straight stretches
    py = y + 0.5 + 1.5 * WARP[1].fbm(x, y, 4, 9) + 1.4 * WARP[1].fbm(x, y + 13, 3, 4)
    ok = inside(MAINLAND, px, py) or inside(AFRICA, px, py) or any(inside(p, px, py) for p in ISLANDS.values())
    if ok and any(inside(p, px, py) for p in SEAS.values()):
        ok = False
    if ok and inside(EASTSEA, px + 9.0 * EDGE[1].fbm(x, y, 3, 9) + 4.0 * EDGE[1].fbm(x + 40, y, 3, 4), py + 7.0 * EDGE[2].fbm(x, y, 3, 9) + 4.0 * EDGE[2].fbm(x, y + 40, 3, 4)):
        ok = False                                     # the eastern sea takes most of Russia and Ukraine
    if ok:
        for pts, hw in CH:
            if poly_dist(x + 0.5, y + 0.5, pts)[0] <= hw:
                ok = False
    if ok and ((from_xy(x + 0.5, y + 0.5)[0] - 47.0) / 24.5) ** 2 + ((from_xy(x + 0.5, y + 0.5)[1] - 71.5) / 14.5) ** 2 < 1 + 0.12 * NZ[19].fbm(x, y, 2, 5) + 0.22 * EDGE[0].fbm(x, y, 3, 4):
        ok = False                                     # round off the lower-left corner of "Russia"
    if ok:
        lon, lat = from_xy(x + 0.5, y + 0.5)
        if lon > 57.5 + 5.0 * EDGE[0].fbm(x, y, 3, 7) + 1.5 * EDGE[0].fbm(x, y, 2, 3):
            ok = False                                 # frayed eastern edge of Russia (map edge to Asia)
        if lat < 25.8 + 3.2 * EDGE[1].fbm(x, y, 3, 8) + 1.2 * EDGE[1].fbm(x, y, 2, 3):
            ok = False                                 # irregular southern edge of the Sahara
        if lon > 41.0 + 5.0 * EDGE[2].fbm(x, y, 3, 8) and lat < 31.0:
            ok = False                                 # irregular eastern edge of the Sahara
    if ok and x < 8 + 8 and y > 101 - 8 and math.hypot(x + 0.5 - 16.0, y + 0.5 - 93.0) > 8.0 + 1.5 * EDGE[2].fbm(x, y, 2, 3):
        ok = False                                     # round off the upper-left corner (Asia)
    land[I(x, y)] = ok
# ragged coast: bays and headlands on tile scale (not near the channels and straits)
rough = land[:]
for x, y in ALL:
    i = I(x, y)
    if any(poly_dist(x + 0.5, y + 0.5, pts)[0] <= hw + 2.0 for pts, hw in CH):
        continue
    ln = [land[I(nx, ny)] for nx, ny in nbrs(x, y, N4)]
    r = NZ[17].fbm(x, y, 2, 2.5)
    if land[i] and sum(ln) <= 3 and sum(ln) >= 2 and r > 0.12:
        rough[i] = False
    elif not land[i] and sum(ln) >= 1 and r < -0.18 and y > 0 and x > 4:
        rough[i] = True
land = rough
for _ in range(2):                     # no single-tile specks, no one-tile pits
    for x, y in ALL:
        i = I(x, y)
        n8 = sum(1 for nx, ny in nbrs(x, y) if land[I(nx, ny)])
        if land[i] and n8 == 0:
            land[i] = False
        elif not land[i] and sum(1 for nx, ny in nbrs(x, y, N4) if land[I(nx, ny)]) == 4:
            land[i] = True
ocean = [not v for v in land]



# ---------------------------------------------------------------- helpers
def bfs_dist(sources, passable):
    dist = [10 ** 6] * (W * H)
    q = deque()
    for i in sources:
        dist[i] = 0
        q.append(i)
    while q:
        i = q.popleft()
        for nx, ny in nbrs(i % W, i // W):
            j = I(nx, ny)
            if passable(j) and dist[j] > dist[i] + 1:
                dist[j] = dist[i] + 1
                q.append(j)
    return dist


def components(mask, d=N4):
    seen, comps = set(), []
    for x, y in ALL:
        i = I(x, y)
        if mask[i] and i not in seen:
            comp, dq = [(x, y)], deque([i])
            seen.add(i)
            while dq:
                j = dq.popleft()
                for nx, ny in nbrs(j % W, j // W, d):
                    k = I(nx, ny)
                    if mask[k] and k not in seen:
                        seen.add(k)
                        dq.append(k)
                        comp.append((nx, ny))
            comps.append(comp)
    return comps


def pick(options):
    r = rng.random() * sum(w for w, _ in options)
    for w, f in options:
        r -= w
        if r <= 0:
            return f
    return options[-1][1]


# ---------------------------------------------------------------- island sea in the eastern sea
# a volcanic island and several settleable islands of 8-14 tiles (at least two water tiles apart from each other and from the coast)
isle_rng = random.Random(SEED * 7 + 71)
ISLE_CELLS, ISLES = set(), []                 # all island plots, list of cell lists (the first is the volcanic island)
dl0 = bfs_dist([i for i in range(W * H) if land[i]], lambda j: True)
isle_cands = [I(x, y) for x, y in ALL if 8 <= x <= 29 and 15 <= y <= 55 and not land[I(x, y)] and dl0[I(x, y)] >= 6]
isle_rng.shuffle(isle_cands)
centres = []
for c in isle_cands:
    if len(ISLES) >= 8:
        break
    cx, cy = c % W, c // W
    if any(math.hypot(cx - ax, cy - ay) < 6.5 for ax, ay in centres):
        continue
    want = 12 if not ISLES else isle_rng.randint(9, 16)
    cells, front = [c], [c]
    while front and len(cells) < want:
        j = front.pop(isle_rng.randrange(len(front)))
        nb = list(nbrs(j % W, j // W, N4))
        isle_rng.shuffle(nb)
        for nx, ny in nb:
            q = I(nx, ny)
            if q in cells or len(cells) >= want or land[q] or q in ISLE_CELLS:
                continue
            if math.hypot(nx - cx, ny - cy) > 4.2 + 0.8 * NZ[7].fbm(nx, ny, 2, 3):
                continue
            if any(land[I(ax, ay)] or I(ax, ay) in ISLE_CELLS for ax in range(nx - 2, nx + 3) for ay in range(ny - 2, ny + 3) if inb(ax, ay)):
                continue
            cells.append(q)
            front.append(q)
    if len(cells) >= 7:
        ISLES.append(cells)
        centres.append((cx, cy))
        ISLE_CELLS.update(cells)
for q in ISLE_CELLS:
    land[q] = True
ocean = [not v for v in land]
VOLC_ISLE = ISLES[0] if ISLES else []
ISLE_NAMES = ['Saint Brendan Island', 'Hy-Brasil', 'Lyonesse', 'Thule', 'Antillia', 'Avalon', 'Mona', 'Ultima', 'Brittia']
VOLC_LONLAT = from_xy(sum(i % W for i in VOLC_ISLE) / float(len(VOLC_ISLE)) + 0.5, sum(i // W for i in VOLC_ISLE) / float(len(VOLC_ISLE)) + 0.5) if VOLC_ISLE else None
ISLE_BLOBS = [(from_xy(sum(i % W for i in cl) / float(len(cl)) + 0.5, sum(i // W for i in cl) / float(len(cl)) + 0.5) + (2.4 if k else 3.2,)) for k, cl in enumerate(ISLES)]
ISLE_BLOBS = [(lo, la, r) for (lo, la, r) in ISLE_BLOBS]

lake = [False] * (W * H)
large = [False] * (W * H)
plot_type = [3 if not land[i] else 2 for i in range(W * H)]      # 0 peak, 1 hills, 2 flat, 3 water

# ---------------------------------------------------------------- lakes
LAKES = [(31.3, 60.7, 1.7, 1.0), (35.4, 61.9, 1.0, 1.1), (27.7, 58.6, 0.8, 1.1), (13.2, 58.9, 1.0, 0.55), (14.5, 58.2, 0.5, 0.9),
         (16.5, 59.5, 0.9, 0.35), (6.5, 46.4, 0.7, 0.3), (9.4, 47.6, 0.6, 0.3), (17.8, 46.9, 1.0, 0.3), (28.2, 61.6, 1.2, 0.9),
         (10.8, 45.6, 0.4, 0.45), (8.6, 46.0, 0.45, 0.4), (27.4, 25.8, 1.9, 0.9), (12.6, 53.3, 0.8, 0.4), (21.6, 53.8, 1.0, 0.4), (14.0, 53.1, 0.5, 0.3),
         (-9.3, 53.4, 0.5, 0.3), (-8.0, 54.2, 0.4, 0.3), (14.4, 60.6, 0.6, 0.5), (16.7, 47.8, 0.3, 0.2), (24.5, 56.8, 0.5, 0.4), (26.5, 58.0, 0.4, 0.4), (11.0, 60.6, 0.5, 0.7)]
for lo, la, rx, ry in LAKES:
    for x, y in ALL:
        i = I(x, y)
        if not land[i]:
            continue
        lon, lat = from_xy(x + 0.5, y + 0.5)
        n = 1 + 0.35 * NZ[2].fbm(x, y, 2, 3)
        if ((lon - lo) / rx) ** 2 + ((lat - la) / ry) ** 2 < n:
            if not any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y)):
                lake[i], land[i], plot_type[i] = True, False, 3
# small steppe lakes in the wide Russian plains
for (cx, cy), rx, ry in ((T2X(12, 45), 1.2, 1.3), (T2X(21, 58), 1.4, 1.4), (T2X(13, 62), 1.1, 1.7), (T2X(23, 41), 1.3, 1.3), (T2X(14, 53), 0.9, 1.4)):
    for x, y in ALL:
        if land[I(x, y)] and ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 < 1 + 0.3 * NZ[2].fbm(x, y, 2, 3):
            lake[I(x, y)], land[I(x, y)], plot_type[I(x, y)] = True, False, 3
# glacial lake district: Finland and Karelia
for x, y in ALL:
    i = I(x, y)
    lon, lat = from_xy(x + 0.5, y + 0.5)
    if land[i] and 60.5 < lat < 65.5 and 23 < lon < 34 and NZ[4].fbm(x, y, 3, 4) > 0.30 and not any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y)):
        lake[i], land[i], plot_type[i] = True, False, 3

# ---------------------------------------------------------------- relief
RANGES = [   # polyline lon/lat, half width in tiles
    ('Alps', [(6.8, 44.1), (7.0, 45.8), (8.5, 46.4), (10.5, 46.5), (12.5, 47.0), (14.5, 47.2), (16.0, 47.6)], 2.4),
    ('Pyrenees', [(-1.8, 43.0), (0.0, 42.7), (1.5, 42.5), (3.0, 42.4)], 1.7),
    ('Cantabrian', [(-8.0, 43.0), (-5.0, 43.0), (-2.5, 42.9)], 1.4),
    ('Sierra Nevada', [(-5.0, 36.9), (-3.5, 37.1), (-2.2, 37.2)], 1.3),
    ('Central System', [(-6.2, 40.3), (-4.5, 40.6), (-3.5, 40.9)], 1.2),
    ('Apennines', [(8.6, 44.5), (9.8, 44.5), (11.5, 44.0), (13.2, 42.5), (14.5, 41.7), (15.8, 40.0), (16.2, 38.6)], 1.7),
    ('Dinaric', [(15.2, 45.6), (17.8, 44.2), (19.8, 43.0), (21.2, 41.6), (22.2, 40.0), (23.0, 38.8)], 1.6),
    ('Carpathians', [(17.5, 49.0), (19.5, 49.4), (22.5, 49.0), (24.5, 48.2), (25.5, 47.2), (25.8, 46.0), (24.5, 45.5), (22.5, 45.2)], 2.2),
    ('Balkan', [(22.5, 43.4), (25.0, 42.8), (27.5, 42.8)], 1.6),
    ('Caucasus', [(38.5, 44.2), (41.0, 43.3), (44.5, 42.6)], 2.4),
    ('Pontic', [(31.0, 41.2), (35.0, 41.3), (40.0, 40.8)], 1.5),
    ('Taurus', [(30.0, 37.2), (33.0, 37.1), (36.5, 37.6), (40.0, 38.0)], 2.2),
    ('Kjolen', [(9.8, 60.5), (11.6, 62.0), (13.6, 63.5), (15.4, 65.5), (17.8, 67.5), (20.2, 68.7), (22.8, 69.7)], 2.0),
    ('Highlands', [(-5.3, 57.2), (-4.0, 56.8), (-3.0, 56.7)], 1.4),
    ('Pennines', [(-2.3, 54.5), (-2.0, 53.2)], 0.9),
    ('Tell Atlas', [(-2.5, 35.0), (1.0, 36.0), (5.0, 36.4), (8.0, 36.5)], 1.4),
    ('High Atlas', [(-9.5, 31.2), (-6.5, 32.0), (-3.5, 32.7), (-1.5, 32.9), (0.0, 33.6), (1.5, 33.8), (3.0, 33.2), (4.5, 33.1),      # two tiles
                    (6.0, 33.8), (7.5, 34.0), (9.0, 33.4), (10.0, 33.2)], 1.9),                                                       # off the coast, bent
    ('Lebanon', [(35.6, 34.5), (36.2, 33.5)], 1.0),
    ('Sinai', [(33.7, 29.5), (34.2, 28.2)], 1.5),
    ('Urals', [(53.2, 66.0), (54.4, 61.0), (53.4, 55.0)], 1.6),
    ('Elburz', [(49.5, 37.0), (52.0, 36.6), (55.5, 36.8)], 1.7),
    ('Zagros', [(45.5, 36.5), (47.5, 33.5), (50.0, 30.8), (53.5, 28.0)], 2.1),
    ('Hindu Kush', [(56.5, 38.0), (57.0, 33.5)], 1.5),
    ('Hejaz', [(37.8, 25.5), (40.0, 23.4)], 1.3),
    ('Russian Heights', [(33.0, 57.5), (37.0, 56.0), (41.0, 55.2), (44.5, 55.8)], 1.3),     # invented chains for the wide plains
    ('Steppe Ridge', [(35.0, 50.5), (38.5, 49.5), (42.0, 50.2)], 1.2),
    ('Massif North', [(38.0, 61.0), (39.5, 60.3)], 1.4),
    ('Massif East', [(45.0, 60.5), (46.5, 59.8)], 1.3),
    ('Massif West', [(30.5, 53.5), (31.5, 52.5)], 1.2),
    ('Massif South', [(44.0, 48.0), (45.5, 47.2)], 1.3),
]
for _n, _pts, _w in (('Plain Chain West', [(9, 60), (13, 57), (17, 55)], 1.3), ('Plain Chain Mid', [(20, 48), (23, 46), (25, 43)], 1.2),
                     ('Plain Chain South', [(10, 40), (13, 44), (16, 47)], 1.2), ('Plain Massif A', [(12, 64), (14, 62)], 1.3),
                     ('Plain Massif B', [(22, 64), (23, 62)], 1.3), ('Plain Massif C', [(13, 50), (14, 52)], 1.2)):
    RANGES.append((_n, [T2L(px, py) for px, py in _pts], _w))
BLOBS = [   # centre lon/lat, radius in tiles: hill country
    (3.2, 45.0, 2.8), (8.0, 48.2, 1.8), (14.5, 50.4, 2.4), (5.5, 50.0, 1.6), (10.5, 51.8, 1.2), (-3.5, 52.5, 1.4), (-3.5, 50.5, 1.0),
    (33.0, 50.0, 2.2), (33.0, 57.5, 2.2), (27.0, 56.0, 1.6), (-8.0, 41.5, 1.8), (-1.0, 40.4, 2.0), (-5.5, 37.8, 1.6), (22.0, 41.8, 1.6),
    (16.5, 49.6, 1.8), (23.0, 47.5, 1.4), (6.0, 24.2, 3.0), (17.0, 23.8, 2.6), (30.0, 24.5, 2.0), (53.0, 51.0, 2.0), (14.5, 61.0, 1.8), (16.0, 57.3, 1.2), (-8.5, 53.0, 1.0), (-6.8, 54.8, 0.8),
]
BLOBS = BLOBS + ISLE_BLOBS + [(10.5, 52.6, 1.4), (12.8, 53.1, 1.2), (21.5, 53.5, 1.5), (24.5, 51.2, 1.3), (20.0, 46.6, 1.0), (2.0, 49.6, 1.3), (-0.5, 48.6, 1.2),
                              (4.0, 47.6, 1.3), (1.0, 45.5, 1.4), (26.5, 49.3, 1.5), (28.5, 52.0, 1.2), (24.0, 55.8, 1.3), (-2.0, 44.0, 1.0), (12.0, 49.2, 1.4),
                              (18.5, 51.5, 1.2), (13.5, 54.0, 1.0), (22.5, 44.2, 1.3), (8.0, 25.8, 1.8), (13.0, 25.5, 2.0), (21.0, 26.8, 1.6), (26.0, 27.5, 1.5),
                              (40.0, 24.5, 2.0), (48.0, 25.5, 1.8), (32.0, 36.5, 1.4), (37.0, 33.0, 1.6),
                              (6.0, 26.5, 2.5), (11.0, 28.0, 2.2), (21.0, 29.5, 2.0), (-2.0, 29.0, 2.0), (27.0, 29.0, 2.0), (32.0, 27.5, 2.0), (16.0, 26.0, 2.0), (-6.0, 28.0, 1.8)]
LOW_CHAINS = [   # (polyline lon/lat, half width in tiles, height): low, broken uplands in the plains and lowlands
    ([(7.0, 47.9), (7.1, 48.5), (7.3, 49.0)], 1.1, 0.62), ([(8.0, 47.6), (8.3, 48.4), (8.6, 49.0)], 1.0, 0.60),
    ([(5.8, 46.0), (6.4, 46.7), (7.2, 47.2), (8.0, 47.5)], 1.0, 0.64), ([(4.5, 49.9), (6.0, 50.2), (7.2, 50.4)], 1.1, 0.55),
    ([(9.8, 51.6), (10.8, 51.8), (12.0, 51.6)], 1.0, 0.60), ([(8.2, 52.1), (9.2, 52.0)], 0.8, 0.5),
    ([(12.3, 50.4), (14.0, 50.8), (16.0, 50.7), (17.5, 50.3)], 1.1, 0.62), ([(13.5, 49.3), (15.5, 49.1), (16.5, 49.6)], 1.0, 0.55),
    ([(19.5, 50.8), (21.0, 50.9)], 1.0, 0.55), ([(25.5, 53.8), (27.5, 53.6), (29.0, 53.0)], 1.0, 0.5),
    ([(27.0, 48.8), (29.0, 48.5), (30.5, 48.3)], 1.0, 0.5), ([(3.8, 47.0), (4.3, 47.4)], 0.9, 0.55),
    ([(-3.0, 48.2), (-1.2, 48.0), (0.0, 48.3)], 1.0, 0.5), ([(-4.2, 52.6), (-3.6, 52.2), (-3.8, 51.8)], 1.0, 0.62),
    ([(-9.5, 52.0), (-8.0, 52.3)], 0.9, 0.55), ([(-3.0, 41.5), (-1.5, 41.0), (-0.5, 40.4)], 1.1, 0.6),
    ([(-6.0, 39.4), (-4.5, 39.4)], 1.0, 0.5), ([(-6.5, 38.1), (-4.0, 38.3)], 1.0, 0.55), ([(2.0, 41.7), (2.4, 42.0)], 0.9, 0.55),
    ([(14.0, 26.0), (19.0, 27.0)], 1.2, 0.62), ([(34.0, 28.0), (36.0, 30.5)], 1.2, 0.62), ([(44.0, 26.0), (46.0, 29.0), (47.5, 31.0)], 1.3, 0.62),
    ([(21.5, 45.4), (23.0, 45.0), (24.5, 45.2)], 1.0, 0.55), ([(15.5, 47.3), (17.0, 47.0)], 0.9, 0.55),
    ([(-1.0, 27.0), (2.0, 28.4), (4.5, 27.6)], 1.2, 0.62), ([(8.0, 26.8), (12.0, 27.8), (16.0, 27.2)], 1.2, 0.6), ([(20.0, 27.5), (23.5, 28.5)], 1.2, 0.6),
    ([(-6.0, 25.8), (-4.0, 27.4)], 1.1, 0.6), ([(26.0, 26.2), (29.0, 27.5)], 1.1, 0.58), ([(36.0, 26.0), (39.0, 27.5), (42.0, 26.5)], 1.2, 0.6),
    # the North European Plain: moraine ridges and heaths
    ([(9.3, 52.9), (10.3, 53.2), (11.0, 52.9)], 1.0, 0.58), ([(12.0, 53.4), (13.3, 53.2), (14.8, 53.5), (16.3, 53.8), (17.8, 54.0)], 1.0, 0.6),
    ([(19.8, 53.5), (21.3, 53.8), (22.8, 53.9)], 1.0, 0.6), ([(24.0, 53.3), (25.5, 53.8), (27.0, 54.0), (28.5, 54.3)], 1.1, 0.62),
    ([(31.0, 54.4), (32.5, 54.9)], 1.0, 0.6), ([(12.2, 51.9), (13.3, 52.0)], 0.9, 0.55), ([(24.5, 50.9), (26.5, 50.7)], 1.0, 0.55),
]
MASSIFS = [(-6.0, 31.0, 2.0), (-1.5, 32.3, 2.0), (5.5, 32.0, 2.0), (10.0, 32.8, 2.0), (21.0, 31.2, 2.0), (-9.0, 29.6, 1.6), (-1.0, 30.0, 1.4), (6.0, 30.0, 1.4), (9.8, 31.0, 1.4), (22.0, 29.5, 1.4), (10.6, 51.7, 2.6), (13.2, 49.1, 2.4), (19.9, 49.2, 2.2), (5.5, 50.1, 2.2), (7.1, 48.3, 2.4), (2.8, 45.3, 3.2), (8.2, 48.0, 2.2),
           (16.2, 50.7, 2.2), (24.5, 41.8, 3.0), (21.2, 39.7, 2.6), (-5.2, 40.3, 2.2), (-3.8, 52.4, 2.0), (4.0, 47.0, 1.8), (20.9, 50.9, 1.8),
           (27.0, 54.0, 1.8), (28.0, 48.8, 2.0), (25.5, 56.8, 1.6), (5.5, 24.0, 3.5), (17.0, 23.5, 3.0), (9.0, 25.5, 2.2), (-1.5, 47.7, 1.8),
           (-6.8, 42.4, 2.0), (1.5, 43.0, 2.0), (12.0, 43.8, 1.8), (16.0, 40.5, 2.0), (18.0, 44.0, 2.4), (22.0, 47.0, 2.2), (25.5, 46.5, 2.2),
           (30.5, 49.0, 1.6), (0.5, 49.9, 1.4), (-8.0, 40.5, 1.8), (13.0, 56.2, 1.8), (22.0, 58.5, 1.6), (36.5, 37.0, 2.4), (43.5, 37.5, 2.4),
           (50.0, 33.0, 2.4), (55.0, 30.0, 2.4), (52.0, 27.0, 2.0), (3.0, 27.5, 2.4), (9.5, 28.0, 2.2), (14.0, 28.2, 2.4), (19.5, 28.5, 2.0),
           (24.0, 27.0, 2.2), (-3.0, 27.0, 2.0), (-8.0, 26.5, 2.0), (31.0, 25.5, 1.8), (38.0, 28.0, 2.0), (42.0, 26.0, 2.0), (47.0, 24.5, 1.8),
           (13.6, 53.3, 1.8), (21.0, 53.7, 1.7), (26.0, 53.8, 1.9), (32.0, 54.7, 1.7), (10.2, 53.1, 1.6)]     # a few summits in the North European Plain
MASSIFXY = [to_xy(lo, la) + (r,) for lo, la, r in MASSIFS]
LOWXY = [([to_xy(lo, la) for lo, la in pts], w, h) for pts, w, h in LOW_CHAINS]
PLATEAUS = [(34.5, 38.8, 4.5, 0.38), (-3.5, 40.0, 4.5, 0.30), (28.0, 61.0, 6.0, 0.18), (53.0, 47.0, 6.0, 0.12), (53.0, 33.0, 6.0, 0.30), (3.0, 25.0, 6.0, 0.20)]   # lon, lat, radius, elevation
ridge = [0.0] * (W * H)
elev = [0.0] * (W * H)
RXY = [(n, [to_xy(lo, la) for lo, la in pts], w * 0.8 * (1.1 if 36 < sum(q[1] for q in pts) / len(pts) < 58 and -10 < sum(q[0] for q in pts) / len(pts) < 26 else 1.0))
       for n, pts, w in RANGES]
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    jitter = 0.75 + 0.5 * (0.5 + 0.5 * NZ[5].fbm(x, y, 3, 5))
    v = 0.0
    qx, qy = x + 0.5 + 1.6 * NZ[5].fbm(x + 7, y, 3, 5), y + 0.5 + 1.6 * NZ[6].fbm(x, y + 7, 3, 5)      # wavy chains, no ruler-straight walls
    for n, pts, w in RXY:
        d, _ = poly_dist(qx, qy, pts)
        v = max(v, 1 - d / (w * jitter))
    for pts, w, h in LOWXY:
        d, _ = poly_dist(qx, qy, pts)
        v = max(v, h * (1 - d / (w * jitter)))
    for lo, la, r in BLOBS:
        bx, by = to_xy(lo, la)
        v = max(v, 0.55 * (1 - math.hypot(x + 0.5 - bx, y + 0.5 - by) / r))
    for bx, by, r in MASSIFXY:
        v = max(v, 1.0 * (1 - math.hypot(x + 0.5 - bx, y + 0.5 - by) / (r * (0.85 + 0.3 * (0.5 + 0.5 * NZ[18].fbm(x, y, 2, 3))))))
    ridge[i] = max(0.0, v)
    plat = 0.0
    for lo, la, r, e in PLATEAUS:
        bx, by = to_xy(lo, la)
        plat = max(plat, e * smoothstep(1.0, 0.3, math.hypot(x + 0.5 - bx, y + 0.5 - by) / r))
    elev[i] = min(1.0, max(plat, 0.04 + 0.1 * (0.5 + 0.5 * NZ[6].fbm(x, y, 3, 8)) + 0.9 * ridge[i]))
rrng = random.Random(SEED * 7 + 3)
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    val = ridge[i] + 0.22 * NZ[3].fbm(x, y, 3, 4)
    if val > 0.62:
        plot_type[i] = 0
    elif val > 0.42 and rrng.random() < 0.88:
        plot_type[i] = 1
    elif val > 0.20 and rrng.random() < 0.6:
        plot_type[i] = 1
# an extra peak here and there next to a chain, so the chains are not uniform walls
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] == 1 and ridge[i] > 0.4 and rrng.random() < 0.10 and any(plot_type[I(nx, ny)] == 0 for nx, ny in nbrs(x, y)):
        plot_type[i] = 0
dc0 = bfs_dist([i for i in range(W * H) if ocean[i]], lambda j: True)
for x, y in ALL:                       # no wide wall of peaks: thin out the thick parts of the chains
    i = I(x, y)
    if land[i] and plot_type[i] == 0:
        n8 = sum(1 for nx, ny in nbrs(x, y) if plot_type[I(nx, ny)] == 0)
        if (n8 >= 5 and rrng.random() < 0.8) or (n8 >= 4 and rrng.random() < 0.55) or (n8 == 3 and rrng.random() < 0.25):
            plot_type[i] = 1 if rrng.random() < 0.45 else 2
    lon, la = from_xy(x + 0.5, y + 0.5)
    if land[i] and plot_type[i] == 0 and 13.0 <= lon <= 24.0 and 38.0 <= la <= 46.5 and dc0[i] <= 3:
        plot_type[i] = 1               # the Dinaric Alps keep away from the sea
# lone hills in the open lowlands (resources that need hills), spaced at least three tiles apart
hill_spots = [I(x, y) for x, y in ALL if land[I(x, y)] and plot_type[I(x, y)] == 2 and ridge[I(x, y)] < 0.2]
rrng.shuffle(hill_spots)
LONE = []
for i in hill_spots:
    if len(LONE) >= 170:
        break
    x, y = i % W, i // W
    if all(math.hypot(x - j % W, y - j // W) >= 3 for j in LONE) and not any(plot_type[I(nx, ny)] != 2 for nx, ny in nbrs(x, y) if land[I(nx, ny)]):
        LONE.append(i)
        plot_type[i] = 1

# scattered knolls and single peaks in the flat lands: the mountain density of the real chains is lower than the fantasy maps'
kn_rng = random.Random(SEED * 7 + 89)
kn_spots = [I(x, y) for x, y in ALL if land[I(x, y)] and plot_type[I(x, y)] == 2 and ridge[I(x, y)] < 0.15 and dc0[I(x, y)] >= 2]
kn_rng.shuffle(kn_spots)
KNOLLS = []
for i in kn_spots:
    if len(KNOLLS) >= 70:
        break
    x, y = i % W, i // W
    if plot_type[i] != 2 or any(max(abs(x - j % W), abs(y - j // W)) < 6 for j in KNOLLS):
        continue
    if any(plot_type[I(nx, ny)] == 0 for nx in range(x - 3, x + 4) for ny in range(y - 3, y + 4) if inb(nx, ny)):
        continue
    KNOLLS.append(i)
    r = kn_rng.random()
    plot_type[i] = 0 if r < 0.55 else 1
    for nx, ny in nbrs(x, y, N4):
        j = I(nx, ny)
        if land[j] and plot_type[j] == 2 and kn_rng.random() < (0.55 if r < 0.55 else 0.35):
            plot_type[j] = 1
    if r < 0.2:                        # an occasional twin peak
        dx, dy = kn_rng.choice(((1, 1), (-1, 1), (1, -1), (-1, -1)))
        if inb(x + dx, y + dy) and land[I(x + dx, y + dy)] and plot_type[I(x + dx, y + dy)] == 2:
            plot_type[I(x + dx, y + dy)] = 0
HILL_PATCHES = []
for i in kn_spots:
    if len(HILL_PATCHES) >= 40:
        break
    x, y = i % W, i // W
    if plot_type[i] != 2 or any(max(abs(x - j % W), abs(y - j // W)) < 4 for j in HILL_PATCHES + KNOLLS):
        continue
    HILL_PATCHES.append(i)
    for dx, dy in ((0, 0), (1, 0), (0, 1), (-1, 0), (0, -1), (1, 1)):
        j = I(x + dx, y + dy) if inb(x + dx, y + dy) else None
        if j is not None and land[j] and plot_type[j] == 2 and kn_rng.random() < 0.55:
            plot_type[j] = 1

# hidden valley: a ring of peaks around a green plain, with a pass on the side facing the Atlantic
HV = to_xy(24.5, 46.3)
HIDDEN = set()
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    d = math.hypot(x + 0.5 - HV[0], y + 0.5 - HV[1])
    ang = math.degrees(math.atan2(y + 0.5 - HV[1], x + 0.5 - HV[0]))
    if d < 2.4:
        plot_type[i] = 2
        HIDDEN.add(i)
    elif d < 3.5 + 0.4 * NZ[20].fbm(x, y, 2, 3):
        plot_type[i] = 1 if abs(ang) < 30 else 0
        HIDDEN.add(i)

# ---------------------------------------------------------------- large rivers
RIVERS = {
    'Danube': [(10.4, 48.0), (11.6, 48.6), (12.6, 48.9), (13.5, 48.6), (16.4, 48.2), (17.2, 47.9), (19.0, 47.5), (19.1, 46.0), (20.2, 44.8),
               (22.4, 44.6), (24.5, 43.8), (26.5, 44.0), (28.0, 44.6), (29.5, 45.2)],
    'Rhine': [(8.6, 46.6), (8.2, 47.4), (7.2, 47.8), (7.0, 48.9), (7.6, 50.0), (6.6, 50.9), (6.2, 51.6), (4.6, 51.9), (4.0, 52.0)],
    'Rhone': [(6.1, 46.2), (4.9, 45.8), (4.8, 44.5), (4.7, 43.6), (4.5, 43.2)],
    'Loire': [(4.0, 45.0), (3.6, 46.5), (2.0, 47.5), (0.5, 47.3), (-1.5, 47.2), (-2.3, 47.2)],
    'Elbe': [(15.5, 50.5), (14.0, 50.5), (13.5, 51.0), (11.5, 52.5), (10.0, 53.4), (8.7, 53.9)],
    'Vistula': [(19.0, 49.8), (21.0, 50.0), (21.0, 52.0), (18.8, 53.4), (18.9, 54.5)],
    'Dnieper': [(30.0, 55.0), (29.8, 53.0), (29.5, 50.8), (31.0, 49.0), (32.0, 47.6), (31.6, 46.5)],
    'Euphrates': [(41.0, 38.5), (41.0, 36.0), (43.5, 34.0), (46.5, 31.0), (48.0, 29.8)],
    'Nile': [(29.3, 26.2), (30.3, 28.0), (31.0, 29.8), (31.0, 31.7)],           # springs from a lake in the Sahara
    'Draa': [(-6.0, 31.0), (-7.5, 29.8), (-9.3, 28.8), (-11.0, 28.4), (-14.0, 28.0)],           # rivers from the Atlas through the Sahara, most end in a lake
    'Saoura': [(-1.5, 32.3), (-1.7, 30.2), (-1.0, 28.2), (0.0, 26.8)],
    'Igharghar': [(5.5, 32.0), (5.9, 30.0), (6.8, 28.4), (7.8, 27.0), (8.4, 24.8), (8.6, 22.0)],
    'Mya': [(10.0, 32.8), (9.6, 30.5), (10.8, 28.6), (11.6, 27.0)],
    'Zousfana': [(21.0, 31.2), (21.5, 29.3), (22.8, 27.6), (23.5, 25.0), (23.8, 22.0)],
}
RIVER_PATHS = {}
river_rng = random.Random(SEED * 7 + 5)
for name, pts in RIVERS.items():
    xy = [to_xy(lo, la) for lo, la in pts]
    dense = []
    for (ax, ay), (bx, by) in zip(xy, xy[1:]):
        n = max(1, int(math.hypot(bx - ax, by - ay) / 4))
        for k in range(n):
            t = k / float(n)
            nx_, ny_ = -(by - ay), bx - ax
            ln = math.hypot(nx_, ny_) or 1.0
            off = (river_rng.uniform(-1.4, 1.4) + 2.6 * NZ[8].fbm(ax + (bx - ax) * t, ay + (by - ay) * t + 40 * len(RIVER_PATHS), 2, 7)) if k else 0.0
            dense.append((ax + (bx - ax) * t + nx_ / ln * off, ay + (by - ay) * t + ny_ / ln * off))
    dense.append(xy[-1])
    cells, last = [], None
    for px, py in dense:
        c = (int(px), int(py))
        if last is None:
            cells.append(c)
        else:
            cx, cy = last
            while (cx, cy) != c:
                if cx != c[0] and (cy == c[1] or abs(c[0] - cx) >= abs(c[1] - cy) * river_rng.uniform(0.6, 1.4)):
                    cx += 1 if c[0] > cx else -1
                else:
                    cy += 1 if c[1] > cy else -1
                cells.append((cx, cy))
        last = c
    path = []
    for cx, cy in cells:
        if not inb(cx, cy):
            continue
        i = I(cx, cy)
        if ocean[i]:
            if path:
                break
            continue
        path.append(i)
    RIVER_PATHS[name] = path
    for i in path:
        large[i], land[i], plot_type[i] = True, False, 3
        lake[i] = False


def safe_to_remove(c):
    """Removing c must not cut the river: its river neighbours stay connected (N4) and river mouths stay."""
    x, y = c % W, c // W
    nb = [I(nx, ny) for nx, ny in nbrs(x, y, N4) if large[I(nx, ny)]]
    if len(nb) < 2 or any(ocean[I(nx, ny)] or lake[I(nx, ny)] for nx, ny in nbrs(x, y, N4)):
        return False
    seen, dq = {nb[0]}, deque([nb[0]])
    while dq:
        q = dq.popleft()
        for nx, ny in nbrs(q % W, q // W, N4):
            r = I(nx, ny)
            if r != c and large[r] and r not in seen and max(abs(nx - x), abs(ny - y)) <= 4:
                seen.add(r)
                dq.append(r)
    return all(n in seen for n in nb)


def thin_rivers():
    """No blocks of large river plots: no 2x2, at most 5 in 3x3, at most 8 in 4x4 - and every river plot keeps its straight (N4) links."""
    removed = 0
    while True:
        changed = False
        for size, limit in ((2, 3), (3, 5), (4, 8)):
            for y0 in range(H - size + 1):
                for x0 in range(W - size + 1):
                    cells = [I(x0 + dx, y0 + dy) for dx in range(size) for dy in range(size) if large[I(x0 + dx, y0 + dy)]]
                    while len(cells) > limit:
                        n4 = lambda c: sum(1 for nx, ny in nbrs(c % W, c // W, N4) if large[I(nx, ny)])
                        cands = sorted([c for c in cells if safe_to_remove(c)], key=lambda c: (-n4(c), river_rng.random()))
                        if not cands:
                            break
                        c = cands[0]
                        large[c], land[c], plot_type[c] = False, True, 2
                        cells.remove(c)
                        removed += 1
                        changed = True
        if not changed:
            return removed


RIVERS_THINNED = thin_rivers()
ENDO = []                              # the Sahara rivers from the Atlas end in a small lake in the desert
for name in ('Saoura', 'Mya'):
    pc = [i for i in RIVER_PATHS.get(name, []) if large[i]]
    if len(pc) >= 5:
        e = pc[-1]
        large[e], lake[e] = False, True
        ENDO.append((e % W, e // W))
for name in RIVER_PATHS:
    RIVER_PATHS[name] = [i for i in RIVER_PATHS[name] if large[i]]
# fords every 8 to 14 tiles so that land units can cross (peaks and large rivers are barriers)
FORD = set()
for name, path in RIVER_PATHS.items():
    k = river_rng.randint(4, 8)
    while k < len(path) - 3:
        i = path[k]
        x, y = i % W, i // W
        if sum(1 for nx, ny in nbrs(x, y, N4) if land[I(nx, ny)]) >= 2:
            FORD.add(i)
        k += river_rng.randint(8, 14)


# ---------------------------------------------------------------- connectivity (peaks and large rivers are barriers for land units)
def solid(i):
    return not ocean[i] and not lake[i]


def passable(i):
    return (land[i] and plot_type[i] != 0) or i in FORD


PASSES = []


def ensure_connectivity():
    while True:
        comps = sorted(components([passable(i) for i in range(W * H)], N8), key=len, reverse=True)
        comp_of = {}
        for k, c in enumerate(comps):
            for x, y in c:
                comp_of[I(x, y)] = k
        changed = False
        for k, c in enumerate(comps[1:], 1):
            if len(c) < 3:
                continue
            dist, prev, pq, found = {}, {}, [], None
            for x, y in c:
                dist[I(x, y)] = 0
                heapq.heappush(pq, (0, I(x, y)))
            while pq:
                d, i = heapq.heappop(pq)
                if d > dist[i]:
                    continue
                if comp_of.get(i, k) < k:
                    found = i
                    break
                for nx, ny in nbrs(i % W, i // W):
                    j = I(nx, ny)
                    if not solid(j) and not large[j]:
                        continue
                    step = 1 if passable(j) else (3 if large[j] else 4)
                    if d + step < dist.get(j, 1e9):
                        dist[j], prev[j] = d + step, i
                        heapq.heappush(pq, (d + step, j))
            if found is None or dist[found] > 16:
                continue
            i, carved = found, 0
            while i in prev:
                if plot_type[i] == 0 and land[i]:
                    plot_type[i] = 1
                    PASSES.append(i)
                    carved += 1
                elif large[i] and i not in FORD:
                    FORD.add(i)
                    PASSES.append(i)
                    carved += 1
                i = prev[i]
            if carved:
                changed = True
                break
        if not changed:
            break


ensure_connectivity()


def near_large(x, y):
    return any(large[I(nx, ny)] or lake[I(nx, ny)] for nx, ny in nbrs(x, y))


dcoast = bfs_dist([i for i in range(W * H) if ocean[i]], lambda j: True)
# the Norwegian coast faces the Atlantic: keep a settleable strip (no peaks within three tiles of the water)
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] == 0 and dcoast[i] <= 2 and from_xy(x + 0.5, y + 0.5)[1] > 57.0 and from_xy(x + 0.5, y + 0.5)[0] < 14.0:
        plot_type[i] = 1

# ---------------------------------------------------------------- climate
T = [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    eff = max(0.0, real_lat(y) - LAT_SHIFT)
    t = 1 - (eff / 80.0) ** 1.3
    t -= 0.22 * ridge[i] + (0.03 if plot_type[i] == 1 else 0)
    if real_lat(y) > 44:
        t -= 0.06 * (1 - (47.0 - from_xy(x + 0.5, y + 0.5)[0]) * 0.022)                  # continental cold towards the old Russia
    t += 0.03 * NZ[10].fbm(x, y, 3, 10)
    T[i] = t

RAIN_REAL = [(24, 0.07), (28, 0.10), (32, 0.26), (35, 0.38), (38, 0.44), (42, 0.55), (46, 0.74), (52, 0.82), (58, 0.80), (64, 0.62), (74, 0.5)]


def march(y, xs):
    out = {}
    m, prev = 1.0, None
    for x in xs:
        i = I(x, y)
        if ocean[i]:
            m, prev = min(1.0, m + 0.5), 0.0
            continue
        if lake[i] or large[i]:
            m = min(1.0, m + 0.12)
            continue
        e = elev[i]
        up = max(0.0, e - (prev if prev is not None else e))
        r = min(m * 0.9, m * (0.045 + 1.6 * up + 0.3 * ridge[i]))
        out[x] = r
        m -= r * 0.65
        prev = e
    return out


P1 = [0.0] * (W * H)
for y in range(H):
    from_w = march(y, range(W))
    from_e = march(y, range(W - 1, -1, -1))              # the Atlantic lies in the east: prevailing winds blow from there
    for x in range(W):
        i = I(x, y)
        if land[i]:
            P1[i] = from_e.get(x, 0) + 0.35 * from_w.get(x, 0)
for _ in range(4):
    nP = P1[:]
    for x, y in ALL:
        i = I(x, y)
        if not land[i]:
            continue
        s, n = P1[i] * 2, 2
        for nx, ny in nbrs(x, y):
            j = I(nx, ny)
            if land[j]:
                s += P1[j]
                n += 1
        nP[i] = s / n
    P1 = nP
cost = [1e9] * (W * H)
pq = []
for i in range(W * H):
    if ocean[i]:
        cost[i] = 0.0
        heapq.heappush(pq, (0.0, i))
while pq:
    c, i = heapq.heappop(pq)
    if c > cost[i]:
        continue
    for nx, ny in nbrs(i % W, i // W, N4):
        j = I(nx, ny)
        step = 1 + 6 * ridge[j] + (2 if plot_type[j] == 0 else 0)
        if c + step < cost[j]:
            cost[j] = c + step
            heapq.heappush(pq, (c + step, j))
M = [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    p1 = min(1.0, P1[i] / 0.14)
    p2 = math.exp(-cost[i] / 14.0)
    wet = 0.12 if near_large(x, y) else 0
    M[i] = max(0.0, interp(RAIN_REAL, real_lat(y)) * (0.55 * p1 + 0.6 * p2) + wet + 0.06 * NZ[11].fbm(x, y, 3, 12))

# ---------------------------------------------------------------- biomes
terrain = [''] * (W * H)
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    t = T[i] + 0.05 * NZ[12].fbm(x, y, 3, 6)
    m = M[i] + 0.14 * NZ[13].fbm(x, y, 3, 5)
    if t < 0.10:
        b = 'SNOW'
    elif t < 0.20:
        b = 'TUNDRA'
    elif t < 0.33:
        b = 'TAIGA' if m > 0.30 else 'TUNDRA'
    elif t < 0.50:
        if m < 0.17:
            b = 'ROCK_STEPPES'
        elif m < 0.27:
            b = 'ROCK_STEPPES' if NZ[14].fbm(x, y, 2, 4) > 0.1 else 'PLAINS_FERTILE'
        elif m < 0.48:
            b = 'PLAINS_FERTILE'
        else:
            b = 'TAIGA' if t < 0.37 else 'GRASS'
    elif t < 0.66:
        if m < 0.13:
            b = 'DESERT'
        elif m < 0.25:
            b = 'ROCK_STEPPES' if elev[i] > 0.3 else 'SHRUBLAND'
        elif m < 0.40:
            b = 'PLAINS'
        elif m < 0.50:
            b = 'PLAINS_FERTILE' if t < 0.57 else 'PLAINS'
        else:
            b = 'GRASS'
    elif t < 0.80:
        if m < 0.14:
            b = 'DESERT'
        elif m < 0.26:
            b = 'SHRUBLAND'
        elif m < 0.42:
            b = 'PLAINS'
        elif m < 0.58:
            b = 'GRASS' if NZ[14].fbm(x, y, 2, 5) > 0.2 else 'SAVANNAH'
        else:
            b = 'GRASS' if t < 0.74 or NZ[14].fbm(x, y, 2, 5) > 0.15 else 'SAVANNAH'
    else:
        if m < 0.14:
            b = 'DESERT'
        elif m < 0.27:
            b = 'SHRUBLAND'
        else:
            b = 'SAVANNAH'
    if b == 'GRASS':                   # patches of fertile plains and prairie break up the green
        pn = NZ[14].fbm(x + 31, y, 3, 5)
        if pn < -0.22:
            b = 'PLAINS'
        elif pn < -0.06:
            b = 'PLAINS_FERTILE'
    elif b in ('PLAINS', 'PLAINS_FERTILE') and plot_type[i] == 1 and elev[i] > 0.3 and NZ[15].fbm(x, y, 2, 3) > 0.0 and t > 0.3:
        b = 'ROCK_STEPPES'
    if plot_type[i] == 2 and m > 0.76 and elev[i] < 0.3 and b not in ('SNOW', 'DESERT'):
        lowwet = dcoast[i] <= 2 or near_large(x, y)
        if lowwet and NZ[15].fbm(x, y, 2, 3) > 0.05:
            b = 'MARSH' if t >= 0.58 else ('WETLAND' if t >= 0.2 else b)
    if plot_type[i] == 0:
        if t < 0.3:
            b = 'SNOW' if t < 0.2 else 'TUNDRA'
        elif b in ('MARSH', 'WETLAND'):
            b = 'GRASS' if t < 0.66 else 'SAVANNAH'
    terrain[i] = b

# river deltas: marsh in the warm, wetland in the cold; Nile and Danube spread wider
DELTA = set()
for name, path in RIVER_PATHS.items():
    if not path:
        continue
    m = path[-1]
    mx, my = m % W, m // W
    r = {'Nile': 4.2, 'Danube': 3.6, 'Rhone': 2.9, 'Po': 3.0, 'Rhine': 2.8, 'Dnieper': 2.6}.get(name, 2.2)
    for x, y in ALL:
        i = I(x, y)
        if land[i] and plot_type[i] == 2 and math.hypot(x - mx, (y - my) * 0.9) <= r * (0.8 + 0.4 * (0.5 + 0.5 * NZ[7].fbm(x, y, 2, 3))):
            if T[i] >= 0.2 and dcoast[i] <= 3:
                terrain[i] = 'MARSH' if T[i] >= 0.55 else 'WETLAND'
                DELTA.add(i)
VALLEY_GREEN = set()
green_rng = random.Random(SEED * 7 + 97)        # now and then a desert plot stays on the river bank
for i in [j for nm in ('Nile', 'Draa', 'Saoura', 'Igharghar', 'Mya', 'Zousfana') for j in RIVER_PATHS.get(nm, [])]:      # green ribbon through the desert
    for nx, ny in nbrs(i % W, i // W):
        j = I(nx, ny)
        if land[j] and plot_type[j] == 2 and j not in DELTA and terrain[j] in ('DESERT', 'ROCK_STEPPES', 'SHRUBLAND') and green_rng.random() > 0.2:
            terrain[j] = 'SAVANNAH' if T[j] >= 0.8 else 'PLAINS'
            VALLEY_GREEN.add(j)

# Russia: salt basin (salt lake, bare rim, salt pans later), hidden valley floor
SB = T2X(32, 56)
BASIN = set()
for x, y in ALL:
    i = I(x, y)
    if not land[i] or large[i]:
        continue
    d = math.hypot(x + 0.5 - SB[0], y + 0.5 - SB[1])
    n = 0.6 * NZ[21].fbm(x, y, 2, 3)
    if d < 1.6 + n and not any(large[I(nx, ny)] for nx, ny in nbrs(x, y)):
        lake[i], land[i], plot_type[i] = True, False, 3
    elif d < 4.0 + n:
        BASIN.add(i)
for i in BASIN:
    if land[i] and plot_type[i] != 0:
        d = math.hypot(i % W + 0.5 - SB[0], i // W + 0.5 - SB[1])
        terrain[i] = 'DESERT' if d < 2.6 and NZ[22].fbm(i % W, i // W, 2, 3) > -0.1 else ('ROCK_STEPPES' if d < 3.4 else 'SHRUBLAND')
        plot_type[i] = 2 if d < 2.6 else plot_type[i]
for i in HIDDEN:
    if land[i] and plot_type[i] != 0:
        terrain[i] = 'GRASS' if T[i] < 0.66 else 'PLAINS_FERTILE'

# the Nile springs from a lake that greens the desert around it
LG = to_xy(27.4, 25.8)
LAKE_GREEN = set()
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] != 0 and terrain[i] in ('DESERT', 'ROCK_STEPPES', 'SHRUBLAND'):
        for (gx, gy), gr in [(LG, 6.0)] + [((ex + 0.5, ey + 0.5), 3.8) for ex, ey in ENDO]:
            d = math.hypot(x + 0.5 - gx, y + 0.5 - gy) * 6.0 / gr
            if d < 6.0 + 0.8 * NZ[23].fbm(x, y, 2, 3):
                terrain[i] = 'SAVANNAH' if d < 3.4 else ('PLAINS' if d < 4.6 else 'SHRUBLAND')
                LAKE_GREEN.add(i)
                break

# oases in the Sahara
orng = random.Random(SEED * 7 + 13)
OASES, OASIS_RING = [], set()
cands = [(x, y) for x, y in ALL if land[I(x, y)] and terrain[I(x, y)] == 'DESERT' and plot_type[I(x, y)] == 2 and dcoast[I(x, y)] > 4
         and real_lat(y) < 33 and not near_large(x, y) and all(land[I(nx, ny)] and terrain[I(nx, ny)] in ('DESERT', 'SHRUBLAND', 'ROCK_STEPPES') for nx, ny in nbrs(x, y))
         and math.hypot(x + 0.5 - LG[0], y + 0.5 - LG[1]) > 9]
orng.shuffle(cands)
for x, y in cands:
    if len(OASES) >= 6:
        break
    if all(math.hypot(x - ox, y - oy) >= 11 for ox, oy in OASES):
        OASES.append((x, y))
        i = I(x, y)
        lake[i], land[i], plot_type[i] = True, False, 3
        for nx, ny in nbrs(x, y):
            j = I(nx, ny)
            if plot_type[j] == 2:
                terrain[j] = 'SHRUBLAND' if orng.random() < 0.5 else 'PLAINS'
                OASIS_RING.add(j)

# ---------------------------------------------------------------- set pieces: canyon, mesas, gorge
def dry_terrain(i):
    return 'DESERT' if T[i] >= 0.5 else 'ROCK_STEPPES'


def green_terrain(i):
    return 'TAIGA' if T[i] < 0.4 else ('GRASS' if T[i] < 0.7 else 'SAVANNAH')


def window(cx, cy, r):
    return [(x, y) for x in range(cx - r, cx + r + 1) for y in range(cy - r, cy + r + 1) if inb(x, y)]


sp_rng = random.Random(SEED * 7 + 83)
RIVER_LISTS = [(name, [i for i in path if large[i]]) for name, path in RIVER_PATHS.items()]
# canyon: the driest stretch of a large river, cut between rock walls
CANYON_WALL, CANYON_NAME = set(), None
best = None
for name, path in RIVER_LISTS:
    for k in range(0, max(1, len(path) - 5)):
        seg = path[k:k + 10]
        if len(seg) < 6:
            continue
        ps = [M[I(nx, ny)] for i in seg for nx, ny in nbrs(i % W, i // W) if land[I(nx, ny)]]
        if ps:
            sc = sum(ps) / len(ps)
            if best is None or sc < best[0]:
                best = (sc, seg, name)
canyon = best[1] if best and best[0] < 0.35 else []
if canyon:
    CANYON_NAME = best[2]
    for x, y in ALL:
        i = I(x, y)
        if not land[i]:
            continue
        d, k = min((max(abs(x - c % W), abs(y - c // W)), n) for n, c in enumerate(canyon))
        if plot_type[i] == 0 and d > 2:
            continue
        if d == 1 and i not in DELTA:
            plot_type[i] = 2
            terrain[i] = 'SAVANNAH' if T[i] >= 0.8 else 'PLAINS'
            VALLEY_GREEN.add(i)
        elif d <= 4:
            # irregular wall: solid inner ridge, random bulges further out, passages stay open
            bulge = NZ[21].fbm(x * 1.7, y * 1.7, 2, 2) + 0.25 * (sp_rng.random() - 0.5)
            gap = k % 5 == 2
            if d == 2:
                pt = 0 if not gap and sp_rng.random() < 0.85 else 1
            elif d == 3:
                pt = 0 if not gap and bulge > 0.05 else (1 if sp_rng.random() < 0.55 else None)
            else:
                pt = 0 if not gap and bulge > 0.4 else (1 if sp_rng.random() < 0.25 else None)
            if pt is None:
                continue
            terrain[i] = dry_terrain(i)
            plot_type[i] = pt
            CANYON_WALL.add(i)
            VALLEY_GREEN.discard(i)

# mesas: free-standing rock towers and buttes in the largest deserts
MESA, MESA_BASE = set(), set()
deserts = sorted(components([land[i] and terrain[i] == 'DESERT' and plot_type[i] in (1, 2) and i not in CANYON_WALL
                             for i in range(W * H)]), key=len, reverse=True)
mesa_centres = []
for comp in deserts[:3]:
    if len(comp) < 12:
        continue
    pool = [(x, y) for x, y in comp if dcoast[I(x, y)] > 2 and plot_type[I(x, y)] == 2 and I(x, y) not in BASIN and not near_large(x, y)]
    sp_rng.shuffle(pool)
    want = max(2, min(6, len(comp) // 40))
    got = 0
    for x, y in pool:
        if got >= want:
            break
        if any(max(abs(x - mx), abs(y - my)) < 6 for mx, my in mesa_centres):
            continue
        mesa_centres.append((x, y))
        got += 1
        r = sp_rng.random()
        cells = [(x, y)] + ([] if r < 0.3 else sp_rng.sample([(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)], 1 if r < 0.7 else 2))
        for n, (cx, cy) in enumerate(cells):
            j = I(cx, cy)
            if inb(cx, cy) and land[j] and not large[j]:
                plot_type[j] = 0 if (n == 0 and r < 0.7) else 1
                terrain[j] = 'DESERT'
                MESA.add(j)
        for cx, cy in cells:
            for nx, ny in nbrs(cx, cy):
                j = I(nx, ny)
                if land[j] and j not in MESA and terrain[j] == 'DESERT':
                    MESA_BASE.add(j)

# gorge: a mountain ridge crossing a large river, which breaks through in a narrow wooded cleft
gorge, GORGE, GORGE_FLOOR, GORGE_NAME = None, set(), set(), None
canyon_set = set(canyon)
best = None
for name, path in RIVER_LISTS:
    for k in range(4, len(path) - 3):
        c = path[k]
        cx, cy = c % W, c // W
        if any(p in canyon_set for p in path[max(0, k - 5):k + 6]) or any(math.hypot(cx - d % W, cy - d // W) < 3 for d in DELTA):
            continue
        if any(math.hypot(cx - j % W, cy - j // W) < 8 for j in BASIN | HIDDEN):
            continue
        ax, ay = path[k + 1] % W - path[k - 1] % W, path[k + 1] // W - path[k - 1] // W
        n = math.hypot(ax, ay) or 1.0
        ax, ay = ax / n, ay / n
        sides, wet = [0, 0], []
        for x, y in window(cx, cy, 7):
            j = I(x, y)
            s = (x - cx) * -ay + (y - cy) * ax
            if abs((x - cx) * ax + (y - cy) * ay) <= 3 and 2 <= abs(s) <= 7 and land[j] and plot_type[j] == 0:
                sides[s > 0] += 1
            if land[j] and max(abs(x - cx), abs(y - cy)) <= 2:
                wet.append(M[j])
            if lake[j] and max(abs(x - cx), abs(y - cy)) <= 4:
                wet.append(-1.0)
        if not wet or T[c] < 0.3:
            continue
        sc = 2 * min(sides) + 0.5 * max(sides) + 6 * sum(wet) / len(wet)
        if best is None or sc > best[0]:
            best = (sc, name, path, k, ax, ay)
if best:
    _, GORGE_NAME, path, gk, ax, ay = best
    gorge = path[gk]
    cx, cy = gorge % W, gorge // W
    seg = path[max(0, gk - 5):gk + 6]
    for x, y in window(cx, cy, 8):
        j = I(x, y)
        if not land[j] or j in CANYON_WALL or j in HIDDEN or j in DELTA or j in BASIN:
            continue
        a = (x - cx) * ax + (y - cy) * ay
        s = (x - cx) * -ay + (y - cy) * ax
        d = min(max(abs(x - p % W), abs(y - p // W)) for p in seg)
        th = 1.2 + 0.8 * (0.5 + 0.5 * NZ[21].fbm(x * 1.5 + 11, y * 1.5, 2, 2))   # irregular ridge thickness
        if abs(s) > 6.5 + 1.5 * NZ[5].fbm(x, y, 2, 2) or abs(a) > th + 1.2:
            continue
        if d == 1 and abs(a) <= th + 1.2:
            plot_type[j] = 2
            terrain[j] = green_terrain(j)
            GORGE_FLOOR.add(j)
        elif abs(a) <= th:
            plot_type[j] = 0 if sp_rng.random() < 0.85 else 1
            GORGE.add(j)
        elif sp_rng.random() < 0.3:
            plot_type[j] = 0 if sp_rng.random() < 0.4 else 1
            GORGE.add(j)
        if j in GORGE and plot_type[j] == 1 and terrain[j] in ('DESERT', 'SNOW'):
            terrain[j] = 'ROCK_STEPPES' if terrain[j] == 'DESERT' else 'TUNDRA'

ensure_connectivity()                  # canyon walls and the gorge ridge may have cut the land again

# ---------------------------------------------------------------- clay hills (only hills on marsh, wetland or tundra give clay)
clay_rng = random.Random(SEED * 7 + 19)
for x, y in ALL:
    i = I(x, y)
    if land[i] and terrain[i] in ('MARSH', 'WETLAND') and plot_type[i] == 2 and clay_rng.random() < 0.13 \
            and sum(1 for nx, ny in nbrs(x, y) if plot_type[I(nx, ny)] == 1) <= 1:
        plot_type[i] = 1
CLAY_R = 7


def is_clay(i):
    return land[i] and plot_type[i] == 1 and terrain[i] in ('MARSH', 'WETLAND', 'TUNDRA')


def needs_clay(i):
    return land[i] and plot_type[i] != 0 and terrain[i] not in ('DESERT', 'SNOW')


CLAY_SCATTER = []
while True:
    cdist = bfs_dist([i for i in range(W * H) if is_clay(i)], lambda j: land[j])
    bad = [i for i in range(W * H) if needs_clay(i) and cdist[i] > CLAY_R]
    if not bad:
        break
    badset = set(bad)

    def reach(k, depth=CLAY_R):                          # land tiles within `depth` steps (water and lakes block)
        seen, dq = {k: 0}, deque([k])
        while dq:
            c = dq.popleft()
            if seen[c] >= depth:
                continue
            for nx, ny in nbrs(c % W, c // W):
                q = I(nx, ny)
                if land[q] and q not in seen:
                    seen[q] = seen[c] + 1
                    dq.append(q)
        return seen

    seed_cell = clay_rng.choice(bad)
    eligible = [q for q in reach(seed_cell) if plot_type[q] == 2 and terrain[q] not in ('DESERT', 'SNOW') and not near_large(q % W, q // W)] or [seed_cell]
    cands = clay_rng.sample(eligible, min(60, len(eligible)))
    j = max(cands, key=lambda k: sum(1 for q in reach(k) if q in badset) + 4 * M[k] + 2 * clay_rng.random())
    terrain[j] = 'WETLAND' if T[j] < 0.55 else 'MARSH'
    plot_type[j] = 1
    CLAY_SCATTER.append(j)


# ---------------------------------------------------------------- features
def coastal_land(x, y):
    return any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y))


FEATURE_TERRAINS = {
    'FOREST': {'GRASS', 'SAVANNAH', 'PLAINS', 'PLAINS_FERTILE'},
    'FOREST_EVERGREEN': {'GRASS', 'PLAINS', 'PLAINS_FERTILE', 'TAIGA'},
    'DENSE_FOREST': {'GRASS', 'PLAINS_FERTILE'},
    'FOREST_TUNDRA': {'TUNDRA', 'TAIGA'},
    'ANCIENT_FOREST': {'TUNDRA', 'TAIGA'},
    'LIGHT_FOREST': {'GRASS', 'WETLAND', 'PLAINS', 'SHRUBLAND', 'PLAINS_FERTILE'},
    'JUNGLE': {'MARSH', 'SAVANNAH'},
    'TROPICAL_GROVES': {'MARSH', 'SAVANNAH'},
    'MANGROVE': {'MARSH'},
}
feature = [None] * (W * H)
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] == 0:
        continue
    b, t, m = terrain[i], T[i], M[i]
    flat = plot_type[i] == 2
    river = near_large(x, y)
    patch = 0.5 + 0.5 * NZ[17].fbm(x, y, 3, 3)
    score = 0.55 * patch + 0.45 * rng.random()
    f = None
    if b in ('GRASS', 'PLAINS_FERTILE', 'PLAINS', 'TAIGA', 'SAVANNAH', 'TUNDRA'):
        base = {'TAIGA': 0.85, 'GRASS': 0.76, 'PLAINS_FERTILE': 0.75, 'PLAINS': 0.68, 'SAVANNAH': 0.66, 'TUNDRA': 0.68}[b]
        density = min(0.95, base * (0.6 + 0.6 * smoothstep(0.15, 0.75, m)))
        if b == 'TUNDRA':
            density *= smoothstep(0.06, 0.18, t)
        if score < density:
            if b == 'TAIGA':
                f = pick([(6, 'FOREST_EVERGREEN'), (2.5 if m > 0.55 else 1, 'ANCIENT_FOREST'), (2.5 if t < 0.27 else 0.5, 'FOREST_TUNDRA')])
            elif b == 'TUNDRA':
                f = pick([(3, 'FOREST_TUNDRA'), (1, 'ANCIENT_FOREST')])
            elif b == 'SAVANNAH':
                if t > 0.8 and m > 0.55:
                    f = pick([(5, 'JUNGLE'), (2, 'TROPICAL_GROVES'), (1, 'FOREST')])
                else:
                    f = pick([(2, 'TROPICAL_GROVES'), (2, 'FOREST'), (1 if t > 0.78 else 0, 'JUNGLE')])
            else:
                cool = smoothstep(0.5, 0.36, t)
                opts = [(3 * (1 - cool) + 0.5, 'FOREST'), (3 * cool + 0.4, 'FOREST_EVERGREEN')]
                if b in ('GRASS', 'PLAINS_FERTILE') and m > 0.65 and not coastal_land(x, y):
                    opts.append((1.2, 'DENSE_FOREST'))
                if b == 'PLAINS':
                    opts.append((1.2, 'LIGHT_FOREST'))
                elif b == 'PLAINS_FERTILE':
                    opts.append((0.4, 'LIGHT_FOREST'))
                f = pick(opts)
        elif river and flat and b in ('GRASS', 'PLAINS', 'PLAINS_FERTILE', 'SAVANNAH') and rng.random() < 0.25:
            f = 'LUSHGRASS'
        elif river and not flat and b in ('GRASS', 'PLAINS', 'PLAINS_FERTILE', 'SAVANNAH') and rng.random() < 0.3:
            f = 'MEADOW'
        elif b in ('TUNDRA', 'TAIGA') and rng.random() < 0.12:
            f = 'LICHEN' if flat and rng.random() < 0.5 else 'MOSS'
    elif b == 'MARSH':
        r = rng.random()
        if coastal_land(x, y) and r < 0.55:
            f = 'MANGROVE'
        elif t > 0.78 and score < 0.55:
            f = 'JUNGLE'
        elif r < 0.25:
            f = 'TROPICAL_GROVES'
    elif b == 'WETLAND':
        r = rng.random()
        if river and flat and r < 0.35:
            f = pick([(2, 'SWAMP'), (2, 'BOG'), (1, 'WILD_FLOWERS')])
        elif r < 0.5:
            f = pick([(2, 'LIGHT_FOREST'), (1, 'THICKET')])
    elif b == 'SHRUBLAND':
        r = rng.random()
        if r < 0.3:
            f = 'SHRUB'
        elif r < 0.4 and flat:
            f = 'CACTUS_TREES'
        elif r < 0.58:
            f = 'LIGHT_FOREST'
    elif b == 'DESERT':
        r = rng.random()
        if river and flat and r < 0.6:
            f = 'FLOOD_PLAINS'
        elif r < 0.08 and flat:
            f = 'CACTUS_TREES'
        elif r < 0.13 and not coastal_land(x, y) and any(plot_type[I(nx, ny)] in (0, 1) for nx, ny in nbrs(x, y)):
            f = 'ROCK_FORMATIONS_DESERT'
    elif b == 'ROCK_STEPPES':
        r = rng.random()
        if river and flat and r < 0.4:
            f = 'FLOOD_PLAINS'
        elif r < 0.12 and flat:
            f = 'LICHEN'
        elif r < 0.2:
            f = 'MOSS'
        elif r < 0.32 and not coastal_land(x, y) and any(plot_type[I(nx, ny)] in (0, 1) for nx, ny in nbrs(x, y)):
            f = 'ROCK_FORMATIONS_TEMPERATE'
    elif b == 'SNOW':
        if rng.random() < 0.03 and not coastal_land(x, y):
            f = 'ROCK_FORMATIONS_POLAR'
    if f in FEATURE_TERRAINS and b not in FEATURE_TERRAINS[f]:
        f = None
    feature[i] = f
for i in FORD:
    feature[i] = 'RIVER_FORD'
for i in HIDDEN:
    if land[i] and plot_type[i] != 0:
        x, y = i % W, i // W
        r = rng.random()
        if near_large(x, y) and r < 0.45:
            feature[i] = 'LUSHGRASS' if plot_type[i] == 2 else 'MEADOW'
        elif r < 0.6:
            feature[i] = pick([(3, 'FOREST'), (1, 'LIGHT_FOREST')])
        else:
            feature[i] = None
for i in GORGE_FLOOR:
    r = rng.random()
    if plot_type[i] == 2 and r < 0.6:
        feature[i] = 'FOREST_EVERGREEN' if terrain[i] == 'TAIGA' else pick([(3, 'FOREST'), (1, 'LIGHT_FOREST')])
for i in OASIS_RING:
    r = rng.random()
    feature[i] = ('SHRUB' if r < 0.4 else ('LIGHT_FOREST' if r < 0.6 else None)) if terrain[i] == 'SHRUBLAND' else ('LIGHT_FOREST' if r < 0.4 else None)
for i in LAKE_GREEN:
    r = rng.random()
    if terrain[i] == 'SAVANNAH':
        feature[i] = 'TROPICAL_GROVES' if r < 0.45 else ('LUSHGRASS' if r < 0.55 and plot_type[i] == 2 else None)
    elif terrain[i] == 'PLAINS':
        feature[i] = 'LIGHT_FOREST' if r < 0.35 else ('LUSHGRASS' if r < 0.5 and plot_type[i] == 2 else None)
    else:
        feature[i] = 'SHRUB' if r < 0.4 else ('LIGHT_FOREST' if r < 0.55 else None)
for i in range(W * H):                # set pieces that were reshaped later may no longer carry their green features
    if land[i] and terrain[i] == 'DESERT' and feature[i] in ('SHRUB', 'LIGHT_FOREST', 'FOREST', 'LUSHGRASS', 'MEADOW', 'WILD_FLOWERS', 'THICKET', 'TROPICAL_GROVES'):
        feature[i] = None
    elif land[i] and feature[i] in FEATURE_TERRAINS and terrain[i] not in FEATURE_TERRAINS[feature[i]]:
        feature[i] = None
    elif land[i] and feature[i] and feature[i].startswith('ROCK_FORMATIONS') and coastal_land(i % W, i // W):
        feature[i] = None
SALT_PANS = []
salt = [I(x, y) for x, y in ALL if land[I(x, y)] and plot_type[I(x, y)] == 2 and terrain[I(x, y)] in ('DESERT', 'ROCK_STEPPES') and feature[I(x, y)] is None
        and I(x, y) in BASIN and not near_large(x, y)]
rng.shuffle(salt)
for i in salt:
    if len(SALT_PANS) < 3 and all(math.hypot(i % W - j % W, i // W - j // W) >= 2.5 for j in SALT_PANS):
        SALT_PANS.append(i)
        feature[i] = 'SALT_PAN'

# volcanoes: Etna, Vesuvius, a volcanic Aegean island and the Teide (FEATURE_VOLCANO sits on flat land, hills around the cone)
VOLCANOES = []
for lo, la in ((15.0, 37.75), (14.4, 40.9), (25.5, 36.45), (-16.6, 28.3), (2.9, 45.8), (44.3, 39.7)) + ((VOLC_LONLAT,) if VOLC_LONLAT else ()):
    bx, by = to_xy(lo, la)
    cands = sorted([I(x, y) for x, y in ALL if land[I(x, y)] and plot_type[I(x, y)] in (1, 2) and not near_large(x, y)],
                   key=lambda k: math.hypot(k % W + 0.5 - bx, k // W + 0.5 - by))
    if cands and math.hypot(cands[0] % W + 0.5 - bx, cands[0] // W + 0.5 - by) < 3:
        i = cands[0]
        VOLCANOES.append(i)
        plot_type[i] = 2
        feature[i] = 'VOLCANO'
        if terrain[i] in ('SNOW', 'TUNDRA', 'MARSH', 'WETLAND'):
            terrain[i] = 'PLAINS'
        for nx, ny in nbrs(i % W, i // W, N4):
            j = I(nx, ny)
            if land[j] and plot_type[j] == 2 and rng.random() < 0.5:
                plot_type[j] = 1
                if feature[j] in ('FOREST', 'LIGHT_FOREST', 'FOREST_EVERGREEN', 'TROPICAL_GROVES', 'JUNGLE', 'DENSE_FOREST'):
                    pass

# ---------------------------------------------------------------- output (draft: ASCII)
TCH = {'GRASS': 'g', 'PLAINS_FERTILE': 'p', 'PLAINS': 'r', 'SAVANNAH': 'v', 'SHRUBLAND': 's', 'DESERT': 'd', 'ROCK_STEPPES': 'k',
       'MARSH': 'm', 'WETLAND': 'w', 'TAIGA': 't', 'TUNDRA': 'u', 'SNOW': 'x'}
FCH = {'FOREST': 'F', 'LIGHT_FOREST': 'F', 'THICKET': 'F', 'FOREST_EVERGREEN': 'E', 'ANCIENT_FOREST': 'E', 'FOREST_TUNDRA': 'E',
       'DENSE_FOREST': 'H', 'JUNGLE': 'J', 'TROPICAL_GROVES': 'J', 'MANGROVE': 'J', 'SWAMP': '%', 'BOG': '%', 'LUSHGRASS': 'l',
       'MEADOW': 'l', 'WILD_FLOWERS': 'l', 'FLOOD_PLAINS': 'f', 'ROCK_FORMATIONS_DESERT': 'o',
       'ROCK_FORMATIONS_TEMPERATE': 'o', 'ROCK_FORMATIONS_POLAR': 'o', 'SHRUB': ',', 'CACTUS_TREES': ',', 'BARREL_CACTUS': ',',
       'LICHEN': ',', 'MOSS': ','}


NORTH_SEA = (-4.5, 12.0, 50.8)        # lon from, lon to, lat from (to the map edge): North Sea and the sea off Norway


def europe_zone():
    """Atlantic Europe plots: open sea at least 8 tiles from land (5 in the North Sea), joined to the east edge.

    The zone comes up to 8 tiles off every open Atlantic coast (5 off the North Sea coasts), but does not leak
    through narrow straits into the Mediterranean, the Baltic or round the North Cape to the west."""
    def min_dist(i):
        lon, la = from_xy(i % W + 0.5, i // W + 0.5)
        return 5 if NORTH_SEA[0] <= lon <= NORTH_SEA[1] and la >= NORTH_SEA[2] else 8

    def open_sea(i):
        return ocean[i] and i % W >= 48 and dland[i] >= min_dist(i)
    zone = [False] * (W * H)
    dq = deque(I(W - 1, y) for y in range(H) if open_sea(I(W - 1, y)))
    for i in dq:
        zone[i] = True
    while dq:
        i = dq.popleft()
        for nx, ny in nbrs(i % W, i // W, N4):
            j = I(nx, ny)
            if not zone[j] and open_sea(j):
                zone[j] = True
                dq.append(j)
    # bays and gulfs: the zone reaches in until the land of every open coast is at most 8 tiles from it; in a bay the
    # zone takes the middle of the bay (the sea at least k tiles from land, k as large as possible). Distances run through
    # water at least 3 tiles from land, so straits like Gibraltar and the Skagerrak stay closed.
    deep = [ocean[i] and dland[i] >= 3 for i in range(W * H)]
    coast = [i for i in range(W * H) if ocean[i] and dland[i] == 1 and i % W >= 48]
    while True:
        de = bfs_dist([i for i in range(W * H) if zone[i]], lambda j: deep[j])
        worst = None
        for c in coast:
            cx, cy = c % W, c // W
            d = min([de[I(x, y)] + max(abs(x - cx), abs(y - cy)) for x in range(cx - 2, cx + 3) for y in range(cy - 2, cy + 3)
                     if inb(x, y) and deep[I(x, y)]] or [10 ** 6])
            if 7 < d <= 16 and (worst is None or d > worst[0]):
                worst = (d, cx, cy)
        if worst is None:
            return zone
        d, cx, cy = worst
        for k in range(7, 2, -1):
            near = set(i for i in range(W * H) if ocean[i] and dland[i] >= k and not zone[i]
                       and max(abs(i % W - cx), abs(i // W - cy)) <= 12)
            grown = set(i for i in near if any(zone[I(nx, ny)] for nx, ny in nbrs(i % W, i // W, N4)))
            dq = deque(grown)
            while dq:
                i = dq.popleft()
                for nx, ny in nbrs(i % W, i // W, N4):
                    j = I(nx, ny)
                    if j in near and j not in grown:
                        grown.add(j)
                        dq.append(j)
            if k == 3 or any(max(abs(i % W - cx), abs(i // W - cy)) <= 7 for i in grown):
                break
        for i in grown:
            zone[i] = True
        if not any(max(abs(i % W - cx), abs(i // W - cy)) <= 7 for i in grown):
            coast = [c for c in coast if (c % W, c // W) != (cx, cy)]     # out of reach even at k = 3


dland = bfs_dist([i for i in range(W * H) if not ocean[i]], lambda j: True)
EUROPE = europe_zone()
PACIFIC = [ocean[i] and (i % W < 4 or (i % W < 18 and dland[i] >= 6)) for i in range(W * H)]      # the four added columns on the left, and wide open sea beyond


# ---------------------------------------------------------------- finishing: small rivers, hardwood, coastal water, reefs, signs
# edge rivers (small rivers on plot borders): the plot corners drain along the terrain towards the sea, lakes and large rivers
CW, CH = W + 1, H + 1


def C(i, j):
    return j * CW + i


def corner_plots(i, j):
    for px, py in ((i - 1, j - 1), (i, j - 1), (i - 1, j), (i, j)):
        if inb(px, py):
            yield px, py


cel = [0.0] * (CW * CH)
outlet = [False] * (CW * CH)
for j in range(CH):
    for i in range(CW):
        ps = list(corner_plots(i, j))
        if len(ps) < 4 or any(not land[I(px, py)] for px, py in ps):
            outlet[C(i, j)] = True
        cel[C(i, j)] = sum(elev[I(px, py)] if land[I(px, py)] else 0 for px, py in ps) / len(ps) + 0.15 * NZ[19].fbm(i, j, 3, 4) + 0.04 * NZ[20].fbm(i, j, 2, 2)
cdown = [-1] * (CW * CH)
cseen = [False] * (CW * CH)
corder = []
pq = []
for k in range(CW * CH):
    if outlet[k]:
        cseen[k] = True
        heapq.heappush(pq, (cel[k], NZ[22].fbm(k % CW, k // CW, 3, 2.5), k))
while pq:
    e, _, k = heapq.heappop(pq)
    corder.append(k)
    i, j = k % CW, k // CW
    for di, dj in N4:
        ni, nj = i + di, j + dj
        if 0 <= ni < CW and 0 <= nj < CH:
            q = C(ni, nj)
            if not cseen[q]:
                cseen[q] = True
                cdown[q] = k
                heapq.heappush(pq, (max(cel[q], e), NZ[22].fbm(ni, nj, 3, 2.5), q))
cacc = [0.0] * (CW * CH)
for k in corder:
    if not outlet[k]:
        i, j = k % CW, k // CW
        cacc[k] = sum(M[I(px, py)] for px, py in corner_plots(i, j)) / 4.0
for k in reversed(corder):
    if cdown[k] >= 0:
        cacc[cdown[k]] += cacc[k]

RIVER_T = 3.2
wofr = {}  # plot -> NS direction (river on east edge)
nofr = {}  # plot -> WE direction (river on south edge)
for k in range(CW * CH):
    if outlet[k] or cdown[k] < 0 or cacc[k] < RIVER_T:
        continue
    i, j = k % CW, k // CW
    d = cdown[k]
    di, dj = d % CW - i, d // CW - j
    if di == 0:  # vertical edge between (i,j) and (i,j+dj): east edge of plot (i-1, min(j, j+dj))
        py = min(j, j + dj)
        wofr[I(i - 1, py)] = 0 if dj > 0 else 2
    else:        # horizontal edge: south edge of plot (min(i, i+di), j)
        px = min(i, i + di)
        nofr[I(px, j)] = 1 if di > 0 else 3
# the volcanic island gets a stream running from the cone down to the sea:
# the longest chain of plot edges (land on both sides) from a corner of the cone to the shore
vol_here = [v for v in VOLCANOES if v in set(VOLC_ISLE)]
if vol_here:
    vx, vy = vol_here[0] % W, vol_here[0] // W

    def edge_plots_c(a, b):
        ai, aj, bi, bj = a % CW, a // CW, b % CW, b // CW
        if ai == bi:
            return [(ai - 1, min(aj, bj)), (ai, min(aj, bj))]
        return [(min(ai, bi), aj - 1), (min(ai, bi), aj)]

    def edge_ok(a, b):
        return all(inb(px, py) and land[I(px, py)] for px, py in edge_plots_c(a, b))

    best_path = []

    def walk(path):
        global best_path
        k = path[-1]
        if len(path) > 1 and any(ocean[I(px, py)] for px, py in corner_plots(k % CW, k // CW)) and len(path) > len(best_path):
            best_path = path[:]
        if len(path) > 5:
            return
        for di, dj in N4:
            i, j = k % CW + di, k // CW + dj
            away = math.hypot(i - vx - 0.5, j - vy - 0.5) > math.hypot(k % CW - vx - 0.5, k // CW - vy - 0.5)   # always downhill from the cone
            if 0 <= i < CW and 0 <= j < CH and away and C(i, j) not in path and edge_ok(k, C(i, j)):
                walk(path + [C(i, j)])

    for i, j in ((vx, vy), (vx + 1, vy), (vx, vy + 1), (vx + 1, vy + 1)):
        walk([C(i, j)])
    for k, d in zip(best_path, best_path[1:]):
        i, j = k % CW, k // CW
        di, dj = d % CW - i, d // CW - j
        if di == 0:
            wofr[I(i - 1, min(j, j + dj))] = 0 if dj > 0 else 2
        else:
            nofr[I(min(i, i + di), j)] = 1 if di > 0 else 3
# edge rivers may only run between two land plots
for i in list(wofr):
    x, y = i % W, i // W
    if not land[i] or x + 1 >= W or not land[I(x + 1, y)]:
        del wofr[i]
for i in list(nofr):
    x, y = i % W, i // W
    if not land[i] or y == 0 or not land[I(x, y - 1)]:
        del nofr[i]
# no small rivers across dry land
DRY = ('DESERT', 'ROCK_STEPPES')
for i in list(wofr):
    if terrain[i] in DRY or terrain[i + 1] in DRY:
        del wofr[i]
for i in list(nofr):
    if terrain[i] in DRY or terrain[i - W] in DRY:
        del nofr[i]

def has_river(x, y):
    i = I(x, y)
    return i in wofr or i in nofr or (x > 0 and I(x - 1, y) in wofr) or (y < H - 1 and I(x, y + 1) in nofr)


# features that need a river on the plot itself (large river plots do not count)
for x, y in ALL:
    i = I(x, y)
    if feature[i] in ('FLOOD_PLAINS', 'LUSHGRASS', 'MEADOW', 'WILD_FLOWERS', 'BOG', 'SWAMP') and not has_river(x, y):
        feature[i] = None
    elif feature[i] == 'SHRUB' and terrain[i] == 'MARSH':
        feature[i] = None

# the North European Plain gets a few more features: lush grass and bogs along the small rivers, thickets and groves elsewhere
pl_rng = random.Random(SEED * 7 + 97)
pl_pick = lambda a, b, wa: a if pl_rng.random() < wa else b      # own random stream: the rest of the map stays as it was
for x, y in ALL:
    i = I(x, y)
    lon, la = from_xy(x + 0.5, y + 0.5)
    if not land[i] or plot_type[i] == 0 or feature[i] is not None or not (4.0 <= lon <= 34.0 and 50.0 <= la <= 57.5) or pl_rng.random() > 0.55:
        continue
    b, flat, riv = terrain[i], plot_type[i] == 2, has_river(x, y)
    if riv and flat and b in ('GRASS', 'PLAINS', 'PLAINS_FERTILE'):
        feature[i] = 'LUSHGRASS'
    elif riv and flat and b == 'WETLAND':
        feature[i] = pl_pick('BOG', 'WILD_FLOWERS', 0.67)
    elif b == 'TAIGA':
        feature[i] = pl_pick('MOSS', 'THICKET', 0.67)
    elif b in ('GRASS', 'PLAINS', 'PLAINS_FERTILE', 'WETLAND'):
        feature[i] = pl_pick('THICKET', 'LIGHT_FOREST', 0.4)

# natural harbours and coves, barrier islands with a shallow lagoon (carved late so that they keep clear of the river mouths)
HARBOUR_WATER, SHALLOW_WATER = set(), set()
hb_rng = random.Random(SEED * 7 + 23)
river_cells = [i for i in range(W * H) if large[i]]


def carve_water(cells):
    for cx, cy in cells:
        j = I(cx, cy)
        land[j], ocean[j], plot_type[j], feature[j], terrain[j] = False, True, 3, None, ''
        wofr.pop(j, None)
        nofr.pop(j, None)
        if cx > 0:
            wofr.pop(I(cx - 1, cy), None)
        if cy < H - 1:
            nofr.pop(I(cx, cy + 1), None)


def open_coast(x, y):
    i = I(x, y)
    if not land[i] or plot_type[i] == 0 or i in ISLE_CELLS or terrain[i] in ('DESERT', 'SNOW') or x < 6 or x >= W - 5:
        return None
    sea = [(dx, dy) for dx, dy in N4 if ocean[I(x + dx, y + dy)]]
    if len(sea) != 1:
        return None
    near_sea = sum(1 for dx in range(-2, 3) for dy in range(-2, 3) if inb(x + dx, y + dy) and ocean[I(x + dx, y + dy)])
    if 7 <= near_sea <= 11:                           # straight coast, no headland or bay yet
        return (x, y, -sea[0][0], -sea[0][1])
    return None


hb_cands = [c for c in (open_coast(x, y) for x, y in ALL) if c]
hb_rng.shuffle(hb_cands)
harbours = []
for x, y, nx_, ny_ in hb_cands:
    if len(harbours) >= 3:
        break
    if any(math.hypot(x - hx, y - hy) < 16 for hx, hy, _, _ in harbours):
        continue
    bx, by = x + 3 * nx_, y + 3 * ny_
    if any(math.hypot(bx - r % W, by - r // W) < 6 for r in river_cells):
        continue
    basin = [(bx, by), (bx - nx_, by - ny_)] + [(bx + dx, by + dy) for dx, dy in N8
                                                if (dx, dy) != (-nx_, -ny_) and (abs(dx) + abs(dy) == 1 or hb_rng.random() < 0.45)]
    water_cells = [(x, y), (x + nx_, y + ny_)] + basin
    # enclosed by at least two land tiles on every side except the entrance; no peaks, lakes or rivers in the bay
    ok = all(inb(cx, cy) and land[I(cx, cy)] and plot_type[I(cx, cy)] != 0 for cx, cy in water_cells)
    for cx, cy in basin:
        for dx in range(-2, 3):
            for dy in range(-2, 3):
                px, py = cx + dx, cy + dy
                if (px - x) * nx_ + (py - y) * ny_ > 0 and (not inb(px, py) or not land[I(px, py)]):
                    ok = False
    if not ok:
        continue
    harbours.append((x, y, nx_, ny_))
    carve_water(water_cells)
    HARBOUR_WATER.update(I(cx, cy) for cx, cy in water_cells)
    for hx, hy in ((x - ny_, y + nx_), (x + ny_, y - nx_)):   # rocky headlands guard the entrance
        j = I(hx, hy)
        if land[j] and plot_type[j] == 2 and hb_rng.random() < 0.6:
            plot_type[j] = 1
# small coves: 3-5 water tiles cut into the coast, a natural anchorage for a coastal town
coves = []
for x, y, nx_, ny_ in hb_cands:
    if len(coves) >= 16:
        break
    if not land[I(x, y)] or any(math.hypot(x - hx, y - hy) < 9 for hx, hy, _, _ in harbours) or any(math.hypot(x - cx, y - cy) < 7 for cx, cy, _, _ in coves):
        continue
    if any(math.hypot(x - r % W, y - r // W) < 3 for r in river_cells):
        continue
    water_cells = [(x, y), (x + nx_, y + ny_)]          # a short random walk inland, no fixed shape
    want = hb_rng.randint(3, 5)
    for _ in range(30):
        if len(water_cells) >= want:
            break
        bx, by = hb_rng.choice(water_cells[1:])
        dx, dy = hb_rng.choice(N4)
        if dx * nx_ + dy * ny_ < 0 or (bx + dx, by + dy) in water_cells:
            continue
        if (bx + dx - x) * nx_ + (by + dy - y) * ny_ < 1:      # stay behind the coastline
            continue
        water_cells.append((bx + dx, by + dy))
    ok = all(inb(cx, cy) and land[I(cx, cy)] and plot_type[I(cx, cy)] != 0 for cx, cy in water_cells)
    for cx, cy in water_cells:
        for qx, qy in nbrs(cx, cy):
            if (qx, qy) in water_cells or (qx - x) * nx_ + (qy - y) * ny_ <= 0:
                continue
            if not land[I(qx, qy)]:
                ok = False
    if not ok:
        continue
    coves.append((x, y, nx_, ny_))
    mode = hb_rng.random()                       # shallow cove, shallow inner part, or deep anchorage
    carve_water(water_cells)
    for n, (cx, cy) in enumerate(water_cells):
        j = I(cx, cy)
        if mode < 0.4 or (mode < 0.7 and n >= 2):
            SHALLOW_WATER.add(j)
        else:
            HARBOUR_WATER.add(j)

# barrier islands (Frisian and Danish west coast): a broken chain two tiles off the coast with a shallow lagoon behind it
dl1 = bfs_dist([i for i in range(W * H) if land[i]], lambda j: True)
BARRIER = set()
for x, y in ALL:
    i = I(x, y)
    lon, la = from_xy(x + 0.5, y + 0.5)
    if ocean[i] and dl1[i] == 2 and 4.3 <= lon <= 9.0 and 53.0 <= la <= 56.2 and NZ[9].fbm(x, y, 2, 3) > -0.02 \
            and not any(land[I(nx, ny)] for nx, ny in nbrs(x, y)) and i not in ISLE_CELLS:
        BARRIER.add(i)
for i in BARRIER:
    land[i], ocean[i], plot_type[i], terrain[i], feature[i] = True, False, 2, 'PLAINS', None
for i in BARRIER:
    for nx, ny in nbrs(i % W, i // W):
        j = I(nx, ny)
        if ocean[j] and dl1[j] == 1:
            SHALLOW_WATER.add(j)
dland = bfs_dist([i for i in range(W * H) if not ocean[i]], lambda j: True)
EUROPE = europe_zone()
for i in range(W * H):                 # above the Sahara the zone reaches further to the centre
    if ocean[i] and dland[i] >= 3 and i % W >= 68 and i // W >= H - 7:
        EUROPE[i] = True
for comp in components(ocean, N8):     # sea regions below 50 plots would turn into lakes when the game loads the map
    if len(comp) < 50 and not any(EUROPE[I(x, y)] or PACIFIC[I(x, y)] for x, y in comp):
        for x, y in comp:
            i = I(x, y)
            lake[i], ocean[i], feature[i] = True, False, None
            HARBOUR_WATER.discard(i)
            SHALLOW_WATER.discard(i)
for x, y in ALL:                       # features that may not touch the coast, after the new bays and islands
    i = I(x, y)
    if land[i] and feature[i] == 'DENSE_FOREST' and coastal_land(x, y):
        feature[i] = 'FOREST'
    elif land[i] and feature[i] and feature[i].startswith('ROCK_FORMATIONS') and coastal_land(x, y):
        feature[i] = None
    elif land[i] and plot_type[i] != 2 and feature[i] in ('CACTUS_TREES', 'FLOOD_PLAINS', 'LUSHGRASS', 'MEADOW', 'WILD_FLOWERS', 'BOG', 'SWAMP', 'SALT_PAN'):
        feature[i] = None

# hardwood: clusters of dense old-growth forest in the temperate and boreal woods
SET_PIECES = DELTA | VALLEY_GREEN | LAKE_GREEN | HIDDEN | BASIN | CANYON_WALL | MESA | MESA_BASE | GORGE | GORGE_FLOOR | set(CLAY_SCATTER) | set(SALT_PANS) | set(VOLCANOES) | set(i for i in range(W * H) if is_clay(i))
hw_rng = random.Random(SEED * 7 + 47)


def hardwood_kind(j):
    x, y = j % W, j // W
    if not land[j] or plot_type[j] == 0 or j in SET_PIECES:
        return None
    if terrain[j] in ('TAIGA', 'TUNDRA'):
        return 'ANCIENT_FOREST'
    if terrain[j] in ('GRASS', 'PLAINS_FERTILE') and not coastal_land(x, y):
        return 'DENSE_FOREST'
    return None


WOODED = ('FOREST', 'FOREST_EVERGREEN', 'DENSE_FOREST', 'ANCIENT_FOREST', 'FOREST_TUNDRA')
seeds = [i for i in range(W * H) if feature[i] in WOODED and hardwood_kind(i)]
hw_rng.shuffle(seeds)
HARDWOOD, hw_centres = set(), []
for c in seeds:
    if len(hw_centres) >= 24:
        break
    if any(max(abs(c % W - h % W), abs(c // W - h // W)) < 7 for h in hw_centres):
        continue
    hw_centres.append(c)
    want, cluster, dq = hw_rng.randint(5, 9), [c], deque([c])
    while dq and len(cluster) < want:
        j = dq.popleft()
        nb = list(nbrs(j % W, j // W, N4))
        hw_rng.shuffle(nb)
        for nx, ny in nb:
            q = I(nx, ny)
            if q not in cluster and feature[q] in WOODED and hardwood_kind(q) and len(cluster) < want:
                cluster.append(q)
                dq.append(q)
    for j in cluster:
        feature[j] = hardwood_kind(j)
        HARDWOOD.add(j)

# more features: meteor crater (Nordlinger Ries), tar pits, ruins of ancient cities, large summit models on the peaks
xf_rng = random.Random(SEED * 7 + 101)
IMPROVEMENT = {}


def flat_site(lon, la, rad=4, wet=False):
    cx, cy = to_xy(lon, la)
    best = None
    for x, y in ALL:
        i = I(x, y)
        if not land[i] or plot_type[i] != 2 or large[i] or i in SET_PIECES or feature[i] in ('VOLCANO', 'CRATER', 'TAR_PIT', 'SALT_PAN'):
            continue
        d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
        if d > rad or (wet and (coastal_land(x, y) or has_river(x, y) or any(not land[I(nx, ny)] or plot_type[I(nx, ny)] == 0 for nx, ny in nbrs(x, y)))):
            continue
        if best is None or d < best[0]:
            best = (d, i)
    return best[1] if best else None


CRATER_AT = flat_site(10.6, 48.9, 5, True)
if CRATER_AT is not None:
    terrain[CRATER_AT], feature[CRATER_AT] = 'WETLAND', 'CRATER'
    for nx, ny in nbrs(CRATER_AT % W, CRATER_AT // W):
        j = I(nx, ny)
        if feature[j] is None and plot_type[j] == 2 and xf_rng.random() < 0.6:
            feature[j] = 'FOREST' if terrain[j] in FEATURE_TERRAINS['FOREST'] else None
TAR_PITS = []
tar_cands = [I(x, y) for x, y in ALL if land[I(x, y)] and plot_type[I(x, y)] == 2 and terrain[I(x, y)] in ('MARSH', 'WETLAND') and feature[I(x, y)] is None
             and not coastal_land(x, y) and not has_river(x, y) and I(x, y) not in SET_PIECES and not any(lake[I(nx, ny)] or large[I(nx, ny)] for nx, ny in nbrs(x, y))]
xf_rng.shuffle(tar_cands)
for i in tar_cands:
    if len(TAR_PITS) >= 4:
        break
    if all(math.hypot(i % W - j % W, i // W - j // W) >= 14 for j in TAR_PITS):
        TAR_PITS.append(i)
        feature[i] = 'TAR_PIT'
RUIN_SITES = []
for lo, la, nm in ((10.3, 36.85, 'Ruins of Carthage'), (14.3, 32.6, 'Ruins of Leptis Magna'), (26.2, 39.95, 'Ruins of Troy'), (38.3, 34.55, 'Ruins of Palmyra'),
                   (35.4, 30.4, 'Ruins of Petra'), (13.0, 41.8, 'Ruins of Ostia'), (23.7, 37.9, 'Ruins of Eleusis'), (-6.2, 37.3, 'Ruins of Italica')):
    i = flat_site(lo, la, 5)
    if i is not None and feature[i] in (None, 'SHRUB', 'LIGHT_FOREST') and all(math.hypot(i % W - j % W, i // W - j // W) >= 4 for j, _ in RUIN_SITES):
        RUIN_SITES.append((i, nm))
        IMPROVEMENT[i] = 'IMPROVEMENT_CITY_RUINS'
        feature[i] = None
# large summit models on peaks (same rules as CvMapGenerator::addImpassableMountains)
MTN_TERRAINS = {'GRASS', 'WETLAND', 'SAVANNAH', 'PLAINS', 'SHRUBLAND', 'PLAINS_FERTILE', 'TUNDRA', 'TAIGA', 'SNOW'}
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] != 0 or terrain[i] not in MTN_TERRAINS or coastal_land(x, y) or feature[i]:
        continue
    if any((feature[I(nx, ny)] or '').startswith('REMARKABLE') for nx, ny in nbrs(x, y)) and xf_rng.random() >= 0.05:
        continue
    n = sum(1 for nx, ny in nbrs(x, y) if plot_type[I(nx, ny)] == 0)
    chance = {0: 5, 1: 10, 2: 20}.get(n, 30 if n <= 4 else (45 if n <= 6 else 55))
    if xf_rng.random() * 100 < chance:
        r = xf_rng.random()            # 1: gold, 2: silver, 3: coal - gold and silver summits are rare
        feature[i] = 'REMARKABLE_MOUNTAIN%d' % (1 if r < 0.07 else (2 if r < 0.14 else 3))

# coastal water: deep coast along the shore, shallow coast in the sheltered or flat seas (North Sea, Baltic, Adriatic, Persian Gulf, ...)
SHALLOW_BOXES = [(-4.0, 9.5, 51.0, 58.0), (-6.5, 2.5, 49.0, 51.2), (-6.5, -2.5, 52.0, 55.5), (11.0, 19.5, 42.5, 46.0), (33.0, 41.0, 44.5, 47.5),
                 (47.0, 57.0, 24.0, 30.5), (31.0, 37.5, 22.0, 30.5), (8.0, 12.0, 52.5, 57.5), (23.0, 27.5, 38.0, 41.0)]
SHALLOW_SEAS = [SEAS['Baltic'], SEAS['Kattegat'], SEAS['Persian Gulf'], SEAS['Red Sea']]


def shallow_zone(x, y):
    lon, la = from_xy(x + 0.5, y + 0.5)
    if any(a <= lon <= b and c <= la <= d for a, b, c, d in SHALLOW_BOXES):
        return True
    return any(inside(p, x + 0.5, y + 0.5) for p in SHALLOW_SEAS)


# a lake may not touch the sea (the game would merge them): such lake plots become wet land
for x, y in ALL:
    i = I(x, y)
    if lake[i] and any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y)):
        lake[i], land[i], plot_type[i], feature[i] = False, True, 2, None
        nb = [terrain[I(nx, ny)] for nx, ny in nbrs(x, y) if land[I(nx, ny)] and terrain[I(nx, ny)]]
        terrain[i] = max(sorted(set(nb)), key=nb.count) if nb else 'WETLAND'
disle = bfs_dist(list(ISLE_CELLS), lambda j: True) if ISLE_CELLS else [10 ** 6] * (W * H)
WATER_TERRAIN = [''] * (W * H)
for x, y in ALL:
    i = I(x, y)
    if large[i]:
        WATER_TERRAIN[i] = terrain[i] = 'LARGE_RIVERS'
    elif lake[i]:
        WATER_TERRAIN[i] = terrain[i] = 'LAKE'
    elif i in SHALLOW_WATER:
        WATER_TERRAIN[i] = terrain[i] = 'SHALLOW_COAST'
    elif i in HARBOUR_WATER:
        WATER_TERRAIN[i] = terrain[i] = 'COAST'
    elif ocean[i] and disle[i] <= 2 and NZ[16].fbm(x, y, 2, 4) > -0.1:
        WATER_TERRAIN[i] = terrain[i] = 'SHALLOW_COAST'              # the island sea is shallow around its islands
    elif ocean[i]:
        d = dland[i]
        near = [(nx, ny) for nx in range(x - 3, x + 4) for ny in range(y - 3, y + 4) if inb(nx, ny)]
        shelter = sum(1 for nx, ny in near if not ocean[I(nx, ny)]) / float(len(near))
        n = NZ[16].fbm(x, y, 2, 4)
        if any(large[I(nx, ny)] or lake[I(nx, ny)] for nx, ny in nbrs(x, y)):
            d = 1
        if d == 1:
            t = 'SHALLOW_COAST' if (shallow_zone(x, y) and n > -0.25) or (shelter > 0.5 and n > 0.0) else 'COAST'
        elif d == 2 and shallow_zone(x, y) and n > 0.05:
            t = 'SHALLOW_COAST'
        elif d == 2 and ((shallow_zone(x, y) and n > -0.2) or (shelter > 0.45 and n > 0.15)):
            t = 'COAST'
        else:
            t = 'OCEAN'
        WATER_TERRAIN[i] = terrain[i] = t
for i in range(W * H):
    if EUROPE[i] or PACIFIC[i]:
        WATER_TERRAIN[i] = terrain[i] = 'OCEAN'

# deep-water lanes: shallow coast may not cut off deep water (large ships cannot enter it)
deep = lambda i: ocean[i] and terrain[i] in ('OCEAN', 'COAST')
lanes, isolated = 0, set()
while True:
    comps = sorted([c for c in components([deep(i) for i in range(W * H)], N8) if I(*c[0]) not in isolated], key=len)
    if len(comps) <= 1:
        break
    src = [I(x, y) for x, y in comps[0]]
    target = set(I(x, y) for x, y in comps[-1])
    dist, prev, pq, found = {i: 0 for i in src}, {}, [(0, i) for i in src], None
    while pq:
        c, i = heapq.heappop(pq)
        if i in target:
            found = i
            break
        if c > dist[i]:
            continue
        for nx, ny in nbrs(i % W, i // W):
            j = I(nx, ny)
            if not ocean[j]:
                continue
            step = 0.01 if deep(j) else 1
            if c + step < dist.get(j, 1e9):
                dist[j], prev[j] = c + step, i
                heapq.heappush(pq, (c + step, j))
    if found is None:
        isolated.update(I(x, y) for x, y in comps[0])
        continue
    i = found
    while i in prev:
        if not deep(i):
            WATER_TERRAIN[i] = terrain[i] = 'COAST'
            lanes += 1
        i = prev[i]

# open-ocean lanes: ships that cannot enter coastal water need a chain of TERRAIN_OCEAN plots from every big sea to the Atlantic
# (Gibraltar for the Mediterranean, the Bosporus for the Black Sea)
oc_lanes, oc_skip = 0, set()
while True:
    comps = components([ocean[i] and terrain[i] == 'OCEAN' for i in range(W * H)], N4)
    main = set(I(x, y) for c in comps if any(EUROPE[I(x, y)] for x, y in c) for x, y in c)
    todo = [c for c in comps if len(c) >= 25 and I(*c[0]) not in main and I(*c[0]) not in oc_skip]
    if not todo:
        break
    c = max(todo, key=len)
    src = [I(x, y) for x, y in c]
    dist, prev, pq, found = {i: 0 for i in src}, {}, [(0, i) for i in src], None
    while pq:
        d, i = heapq.heappop(pq)
        if i in main:
            found = i
            break
        if d > dist[i]:
            continue
        for nx, ny in nbrs(i % W, i // W, N4):
            j = I(nx, ny)
            if not ocean[j]:
                continue
            step = {'OCEAN': 0.01, 'COAST': 1.0}.get(terrain[j], 1.5)
            if d + step < dist.get(j, 1e9):
                dist[j], prev[j] = d + step, i
                heapq.heappush(pq, (d + step, j))
    if found is None:
        oc_skip.add(I(*c[0]))
        continue
    i = found
    while i in prev:
        if terrain[i] != 'OCEAN':
            WATER_TERRAIN[i] = terrain[i] = 'OCEAN'
            feature[i] = None
            oc_lanes += 1
        i = prev[i]

# water features: kelp and seagrass, a few rock reefs, coral in the warm south, pearl reefs, vents in the open ocean
wrng = random.Random(SEED * 7 + 59)
for x, y in ALL:
    i = I(x, y)
    if not ocean[i] or EUROPE[i] or PACIFIC[i] or i in HARBOUR_WATER:
        continue
    tr, la, r = terrain[i], from_xy(x + 0.5, y + 0.5)[1], wrng.random()
    if tr == 'OCEAN':
        if r < 0.015:
            feature[i] = 'VENT_FAV'
        elif r < 0.03:
            feature[i] = 'VENT_DEF'
        elif r < 0.045 and la < 33 and dland[i] <= 7:
            feature[i] = 'REEF'
    else:
        if r < 0.05:
            feature[i] = 'KELP' if la > 45 else 'SEAGRASS'
        elif r < 0.075 or (r < 0.11 and la < 33):
            feature[i] = 'STONE_REEF'
vdist = bfs_dist(list(VOLC_ISLE), lambda j: True) if VOLC_ISLE else [10 ** 6] * (W * H)
for x, y in ALL:                       # reef ring around the volcanic island
    i = I(x, y)
    if not ocean[i] or vdist[i] > 3 or EUROPE[i] or PACIFIC[i]:
        continue
    r = wrng.random()
    if terrain[i] in ('COAST', 'SHALLOW_COAST'):
        WATER_TERRAIN[i] = terrain[i] = 'SHALLOW_COAST'
        feature[i] = 'STONE_REEF' if r < 0.2 else ('SEAGRASS' if r < 0.35 else None)
    elif terrain[i] == 'OCEAN' and r < 0.45:
        feature[i] = 'REEF'
PEARLS = []
cands = [(x, y) for x, y in ALL if terrain[I(x, y)] in ('SHALLOW_COAST', 'COAST') and ocean[I(x, y)] and feature[I(x, y)] in (None, 'SEAGRASS', 'STONE_REEF')
         and from_xy(x + 0.5, y + 0.5)[1] < 34 and not (EUROPE[I(x, y)] or PACIFIC[I(x, y)])]
wrng.shuffle(cands)
cands.sort(key=lambda p: terrain[I(*p)] != 'SHALLOW_COAST')
for x, y in cands:
    if len(PEARLS) >= 5:
        break
    if all(math.hypot(x - px, y - py) >= 10 for px, py in PEARLS):
        PEARLS.append((x, y))
        feature[I(x, y)] = 'PEARL_REEF'

# landmark names: map signs with the real names (visible to all players; ASCII captions, no commas or '=')
SIGNS = []


def add_sign(cells, caption, gap=5):
    for c in cells:
        x, y = (c % W, c // W) if isinstance(c, int) else c
        if inb(x, y) and all(max(abs(x - sx), abs(y - sy)) >= gap for sx, sy, _ in SIGNS):
            SIGNS.append((x, y, caption))
            return True
    return False


def by_dist(cells, cx, cy):
    return sorted(cells, key=lambda c: math.hypot(c % W + 0.5 - cx, c // W + 0.5 - cy))


def water_near(lon, la):
    cx, cy = to_xy(lon, la)
    cells = [i for i in range(W * H) if ocean[i] and not (EUROPE[i] or PACIFIC[i]) and math.hypot(i % W + 0.5 - cx, i // W + 0.5 - cy) < 5]
    return sorted(cells, key=lambda i: (-min(dland[i], 3), math.hypot(i % W + 0.5 - cx, i // W + 0.5 - cy)))


def land_near(lon, la, rad=4):
    cx, cy = to_xy(lon, la)
    return by_dist([i for i in range(W * H) if land[i] and math.hypot(i % W + 0.5 - cx, i // W + 0.5 - cy) < rad], cx, cy)


# set pieces first, they own their spot
if CRATER_AT is not None:
    add_sign([CRATER_AT], 'Nordlinger Ries', 4)
for i, nm in RUIN_SITES:
    add_sign([i], nm, 4)
if canyon:
    add_sign(canyon[len(canyon) // 2:] + canyon, CANYON_NAME + ' Canyon', 4)
if gorge is not None:
    add_sign([gorge] + [j for j in GORGE_FLOOR], GORGE_NAME + ' Gorge', 4)
if mesa_centres:
    add_sign([I(x, y) for x, y in mesa_centres], 'Painted Mesas', 4)
VOLCANO_NAMES = [((15.0, 37.75), 'Mount Etna'), ((14.4, 40.9), 'Mount Vesuvius'), ((25.5, 36.45), 'Santorini'), ((-16.6, 28.3), 'Teide'), ((2.9, 45.8), 'Chaine des Puys'), ((44.3, 39.7), 'Mount Ararat')] + ([(VOLC_LONLAT, ISLE_NAMES[0])] if VOLC_LONLAT else [])
for i in VOLCANOES:
    lo, la = from_xy(i % W + 0.5, i // W + 0.5)
    add_sign([i], min(VOLCANO_NAMES, key=lambda v: math.hypot(v[0][0] - lo, v[0][1] - la))[1], 3)
# mountain ranges: the highest peak near the middle of the chain
RANGE_CAPTIONS = {'Alps': 'Alps', 'Pyrenees': 'Pyrenees', 'Cantabrian': 'Cantabrian Mountains', 'Sierra Nevada': 'Sierra Nevada',
                  'Central System': 'Sistema Central', 'Apennines': 'Apennines', 'Dinaric': 'Dinaric Alps', 'Carpathians': 'Carpathians',
                  'Balkan': 'Balkan Mountains', 'Caucasus': 'Caucasus', 'Pontic': 'Pontic Mountains', 'Taurus': 'Taurus Mountains',
                  'Kjolen': 'Kjolen Mountains', 'Highlands': 'Scottish Highlands', 'Pennines': 'Pennines', 'Tell Atlas': 'Tell Atlas',
                  'High Atlas': 'High Atlas', 'Lebanon': 'Lebanon Mountains', 'Sinai': 'Sinai Mountains', 'Urals': 'Ural Mountains',
                  'Elburz': 'Elburz Mountains', 'Zagros': 'Zagros Mountains', 'Hindu Kush': 'Hindu Kush', 'Hejaz': 'Hejaz Mountains'}
for name, pts, w in RXY:
    if name not in RANGE_CAPTIONS:
        continue
    pk = [i for i in range(W * H) if plot_type[i] == 0 and land[i] and poly_dist(i % W + 0.5, i // W + 0.5, pts)[0] < 2 * w + 0.5]
    if pk:
        mx, my = pts[len(pts) // 2]
        add_sign(by_dist(pk, mx, my), RANGE_CAPTIONS[name])
# large rivers: the name sits upstream of the mouth
for name, path in RIVER_PATHS.items():
    cells = [i for i in path if large[i]]
    if len(cells) >= 6:
        k = len(cells) * 2 // 3
        add_sign(cells[k:] + cells[:k][::-1], name + ' River')
# lakes
LAKE_NAMES = ['Lake Ladoga', 'Lake Onega', 'Lake Peipus', 'Lake Vanern', 'Lake Vattern', 'Lake Malaren', 'Lake Geneva', 'Lake Constance', 'Lake Balaton',
              'Lake Saimaa', 'Lake Garda', 'Lake Maggiore']
for (lo, la, rx, ry), name in zip(LAKES, LAKE_NAMES):
    cx, cy = to_xy(lo, la)
    cells = by_dist([i for i in range(W * H) if lake[i] and math.hypot(i % W + 0.5 - cx, i // W + 0.5 - cy) < 4], cx, cy)
    add_sign(cells, name, 3)
# islands
for name, poly in ISLANDS.items():
    cells = [i for i in range(W * H) if land[i] and inside(poly, i % W + 0.5, i // W + 0.5)]
    if len(cells) >= 7:
        cx = sum(i % W + 0.5 for i in cells) / len(cells)
        cy = sum(i // W + 0.5 for i in cells) / len(cells)
        add_sign(by_dist(cells, cx, cy), name, 3)
# islands of the eastern sea
for k, cl in enumerate(ISLES):
    cx = sum(i % W + 0.5 for i in cl) / len(cl)
    cy = sum(i // W + 0.5 for i in cl) / len(cl)
    add_sign(by_dist(cl, cx, cy), ISLE_NAMES[k], 3)
if BARRIER:
    bl = sorted(BARRIER, key=lambda i: i % W)
    add_sign(bl[len(bl) // 2:] + bl, 'Frisian Islands', 3)
# seas
for lo, la, name in ((15.0, 35.0, 'Mediterranean Sea'), (12.0, 40.0, 'Tyrrhenian Sea'), (15.5, 43.2, 'Adriatic Sea'), (25.0, 38.5, 'Aegean Sea'),
                     (18.5, 37.3, 'Ionian Sea'), (34.0, 43.0, 'Black Sea'), (19.0, 58.5, 'Baltic Sea'), (20.0, 62.0, 'Gulf of Bothnia'),
                     (3.0, 56.0, 'North Sea'), (-1.0, 49.8, 'English Channel'), (-5.0, 45.5, 'Bay of Biscay'), (50.0, 42.0, 'Caspian Sea'),
                     (51.0, 27.5, 'Persian Gulf'), (37.0, 25.0, 'Red Sea'), (3.0, 64.0, 'Norwegian Sea'), (-4.5, 53.5, 'Irish Sea')):
    add_sign(water_near(lo, la), name, 6)
# regions
for lo, la, name in ((12.0, 62.0, 'Scandinavia'), (-4.0, 40.0, 'Iberian Peninsula'), (33.0, 39.0, 'Anatolia'), (20.0, 47.0, 'Hungarian Plain'),
                     (10.0, 56.0, 'Jutland'), (-3.0, 48.2, 'Brittany'), (22.0, 37.7, 'Peloponnese'), (34.0, 45.2, 'Crimea'),
                     (8.0, 27.0, 'Sahara'), (47.0, 36.0, 'Mesopotamia'), (30.0, 54.0, 'Russian Plain'), (13.0, 42.8, 'Italian Peninsula'),
                     (30.5, 29.2, 'Nile Delta'), (24.0, 62.0, 'Finland'), (-1.0, 38.0, 'Andalusia')):
    add_sign(land_near(lo, la), name, 7)



# ---------------------------------------------------------------- Mediterranean tropics (late, own random stream)
# The warm shores of the Mediterranean become a jungle belt (savannah or marsh with jungle and tropical groves, mangroves
# on wet coasts), behind it lies a belt of savannah. Peaks, deserts far from the sea, set pieces and special features stay.
def river_side(x, y):
    return I(x, y) in wofr or I(x, y) in nofr or (x > 0 and I(x - 1, y) in wofr) or (y + 1 < H and I(x, y + 1) in nofr)


trng = random.Random(SEED * 7 + 97)
TROP_NZ = Shifted(Noise(SEED * 1000 + 600))
MED_WATER = []
for x, y in ALL:
    i = I(x, y)
    lon, la = from_xy(x + 0.5, y + 0.5)
    if ocean[i] and -5.6 < lon < 36.5 and 30.0 < la < 46.0 and not inside(SEAS['Black Sea'], x + 0.5, y + 0.5) \
            and not (lon > 26.5 and la > 40.2):
        MED_WATER.append(i)
dmed = bfs_dist(MED_WATER, lambda j: land[j])
TROP_KEEP = ('VOLCANO', 'CRATER', 'TAR_PIT', 'RIVER_FORD', 'FLOOD_PLAINS', 'OASIS', 'SALT_PAN', 'BOG', 'SWAMP')
JUNGLE, SAVANNA_BELT = set(), set()
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] == 0 or i in SET_PIECES or i in IMPROVEMENT or i in HARDWOOD:
        continue
    f = feature[i] or ''
    if f in TROP_KEEP or f.startswith('ROCK_FORMATIONS') or f.startswith('REMARKABLE'):
        continue
    la = real_lat(y)
    d = dmed[i] + 1.8 * TROP_NZ.fbm(x, y, 3, 6) + max(0.0, la - 41.0) * 0.8      # ragged belts, thinner to the north
    if terrain[i] in ('SNOW', 'TUNDRA', 'TAIGA') or la > 45.5:
        continue
    if terrain[i] == 'DESERT' and dmed[i] > 2:
        continue                       # the Sahara stays desert
    if d <= 2.2:
        wet = plot_type[i] == 2 and (river_side(x, y) or any(ocean[I(nx, ny)] or lake[I(nx, ny)] for nx, ny in nbrs(x, y))) and trng.random() < 0.45
        terrain[i] = 'MARSH' if wet else 'SAVANNAH'
        coast = any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y))
        r = trng.random()
        if terrain[i] == 'MARSH' and coast and r < 0.35:
            feature[i] = 'MANGROVE'
        else:
            feature[i] = 'JUNGLE' if r < 0.6 else ('TROPICAL_GROVES' if r < 0.85 else None)
        JUNGLE.add(i)
    elif d <= 5.5:
        terrain[i] = 'SAVANNAH'
        r = trng.random()
        feature[i] = 'TROPICAL_GROVES' if r < 0.2 else ('LUSHGRASS' if r < 0.3 and plot_type[i] == 2 and river_side(x, y) else None)
        SAVANNA_BELT.add(i)


# ---------------------------------------------------------------- resources: placed by the generator instead of the game
# The rules of the game (CvPlot::canHaveBonus, CvMapGenerator::canPlaceBonusAt) are read from CIV4BonusInfos.xml, and the
# amount per resource follows the game's formula. The spread is not even: every resource has its own large-scale noise
# (rich and poor provinces), there is a shared richness noise, resources come in small groups, and the real deposits of
# Europe (coal in the Ruhr, silver in the Harz, iron at Kiruna, wine in Bordeaux, ...) draw their resource. The climate
# latitude of a plot is its real latitude (the game's latitude runs the wrong way on this flipped map).
BONUS_XML = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Assets', 'XML', 'Terrain', 'CIV4BonusInfos.xml')


def read_bonus_infos(path):
    import re
    text = open(path, encoding='utf-8').read()
    infos = []
    for b in re.findall(r'<BonusInfo>(.*?)</BonusInfo>', text, re.S):
        def val(tag, default=0):
            m = re.search(r'<%s>(-?\d+)<' % tag, b)
            return int(m.group(1)) if m else default

        def bools(tag):
            m = re.search(r'<%s>(.*?)</%s>' % (tag, tag), b, re.S)
            if not m:
                return set()
            return set(k.split('_', 1)[1] for k, v in re.findall(r'<(?:Terrain|Feature)Type>(\w+)</(?:Terrain|Feature)Type>\s*<b\w+>(\d)<', m.group(1)) if v == '1')
        rands = re.search(r'<Rands>(.*?)</Rands>', b, re.S)
        infos.append(dict(
            type=re.search(r'<Type>BONUS_(\w+)<', b).group(1), order=val('iPlacementOrder'), const=val('iConstAppearance'),
            rands=[int(v) for v in re.findall(r'>(\d+)<', rands.group(1))] if rands else [], tiles_per=val('iTilesPer'),
            min_area=val('iMinAreaSize', -1), min_lat=val('iMinLatitude'), max_lat=val('iMaxLatitude', 90), unique=val('iUnique'),
            hills=val('bHills') == 1, flat=val('bFlatlands') == 1, peaks=val('bPeaks') == 1, no_river=val('bNoRiverSide') == 1,
            river_only=val('bRiverSideOnly') == 1, coastal=val('bCoastalLandOnly') == 1, whale=val('bWhalingboatWorkable') == 1,
            south=val('bOnlySouthernHemisphere') == 1,
            T=bools('TerrainBooleans'), F=bools('FeatureBooleans'), FT=bools('FeatureTerrainBooleans')))
    return infos


# real deposits: resource -> (lon, lat, radius in tiles, extra weight in the centre)
DEPOSITS = {
    'COAL': [(7.2, 51.5, 4, 8), (18.8, 50.3, 4, 8), (-3.5, 51.7, 3, 8), (-1.6, 55.0, 3, 6), (38.0, 48.0, 4, 8), (6.9, 49.3, 3, 5),
             (4.4, 50.4, 3, 6), (-5.8, 43.3, 3, 5), (-4.0, 55.9, 3, 5)],
    'IRON': [(20.2, 67.8, 4, 8), (15.0, 60.0, 4, 8), (6.0, 49.2, 3, 6), (14.9, 47.5, 3, 6), (-2.9, 43.3, 3, 6), (33.4, 47.9, 4, 6),
             (10.3, 42.8, 2, 5), (8.0, 50.9, 3, 5), (59.5, 57.0, 5, 6)],
    'SILVER': [(10.5, 51.8, 3, 10), (13.0, 50.6, 3, 10), (15.3, 49.9, 3, 8), (11.7, 47.3, 3, 8), (-5.5, 38.2, 4, 8), (9.6, 59.7, 3, 8)],
    'GOLD': [(23.0, 46.3, 3, 10), (18.9, 48.7, 2, 8), (-6.8, 42.5, 3, 8), (13.0, 47.1, 2, 6)],
    'GEMS': [(13.8, 50.5, 3, 8), (7.3, 49.7, 2, 8), (58.8, 36.2, 3, 8)],
    'SALT': [(20.0, 50.0, 3, 8), (13.6, 47.6, 3, 8), (10.4, 53.2, 2, 6), (-2.5, 53.2, 2, 6), (12.0, 51.5, 2, 6), (35.5, 31.5, 2, 6),
             (-3.5, 24.0, 5, 4)],
    'STONE': [(10.1, 44.1, 2, 6), (-2.4, 50.5, 2, 4), (23.9, 38.1, 2, 4), (27.6, 40.6, 2, 4)],
    'GRAPES': [(-0.6, 44.8, 3, 10), (4.8, 47.0, 3, 10), (4.0, 49.0, 2, 8), (7.3, 50.0, 3, 8), (11.2, 43.5, 3, 8), (-2.5, 42.5, 2, 8),
               (-7.5, 41.2, 2, 8), (21.4, 48.1, 2, 8), (-6.1, 36.7, 2, 6), (4.8, 44.5, 2, 6), (44.0, 42.0, 3, 6)],
    'OLIVES': [(-4.5, 37.6, 4, 10), (16.5, 41.0, 3, 8), (22.0, 37.5, 3, 8), (10.0, 35.5, 3, 8), (24.9, 35.2, 2, 6), (35.5, 33.0, 3, 8),
               (5.5, 43.5, 2, 6)],
    'HORSES': [(-5.5, 37.5, 3, 5), (19.5, 47.0, 4, 6), (35.0, 48.0, 6, 6), (45.0, 27.0, 6, 6), (4.5, 43.5, 2, 4)],
    'SHEEP': [(-1.5, 52.5, 4, 6), (-4.0, 56.5, 3, 6), (-4.0, 40.5, 5, 6)],
    'CATTLE': [(5.5, 53.0, 3, 6), (-0.5, 49.0, 3, 6), (-8.0, 53.0, 4, 6), (8.2, 46.8, 3, 6), (9.0, 56.0, 3, 6)],
    'FLAX': [(3.5, 51.0, 3, 8), (-7.0, 54.5, 3, 6), (29.5, 57.8, 4, 8)],
    'HEMP': [(36.0, 54.0, 6, 6), (11.5, 44.8, 3, 5)],
    'TOBACCO': [(22.5, 41.0, 3, 8), (36.0, 41.0, 3, 6)],
    'COTTON': [(31.0, 30.0, 4, 10), (35.0, 37.0, 3, 6), (37.0, 36.5, 3, 5)],
    'SUGAR': [(-4.0, 36.8, 2, 5), (14.0, 37.5, 2, 5), (31.0, 27.0, 3, 5)],
    'RICE': [(9.5, 45.2, 2, 6), (-0.3, 39.4, 2, 6), (31.0, 31.0, 2, 6)],
    'FUR': [(31.0, 58.5, 5, 6), (24.0, 67.0, 5, 5), (58.0, 60.0, 5, 6)],
    'FISH': [(2.0, 54.6, 4, 6), (14.0, 68.0, 3, 8), (-5.0, 47.5, 3, 4), (14.0, 55.0, 4, 4)],
    'WHALE': [(5.0, 64.0, 5, 6), (-4.0, 45.0, 4, 4)],
    'HUMPBACK_WHALE': [(5.0, 64.0, 5, 6), (-4.0, 45.0, 4, 4)],
    'PEARLS': [(51.0, 26.0, 4, 10), (37.5, 23.5, 4, 6)],
    'PEARLS_2': [(51.0, 26.0, 4, 10), (37.5, 23.5, 4, 6)],
}
GROUP_CHANCE = {'COAL': 0.6, 'IRON': 0.6, 'SILVER': 0.5, 'GOLD': 0.4, 'GEMS': 0.4, 'SALT': 0.5, 'STONE': 0.5, 'GRAPES': 0.6,
                'OLIVES': 0.6, 'FISH': 0.5, 'HIGH_SEA_FISH': 0.5, 'SHRIMP': 0.4, 'CRAB': 0.4}

RICHNESS = 6.0                         # contrast of the shared rich/poor land noise (noise sd ~0.28)
LAND_AMOUNT = 0.8                      # share of the game's amount on land: room for rich and empty stretches
PRECIOUS = {'GOLD': 0.3, 'SILVER': 0.3, 'GEMS': 0.4}     # share of the game's amount: few, mostly at the real deposits
NEIGHBOUR_SHARE = 0.10                 # now and then (share of the placements) a resource right next to another one
brng = random.Random(SEED * 7 + 71)
BONUS_INFOS = [b for b in read_bonus_infos(BONUS_XML) if not b['south']]      # Europe has no southern hemisphere
BONUS_INFOS.sort(key=lambda b: b['order'])                                      # stable: XML order within one placement order
BONUS = {}
area_size = [0] * (W * H)
for comp in components([land[i] for i in range(W * H)], N8) + components([not land[i] for i in range(W * H)], N8):
    for x, y in comp:
        area_size[I(x, y)] = len(comp)
NO_BONUS_FEATURES = ('VOLCANO', 'CRATER', 'TAR_PIT', 'RIVER_FORD')


def can_have(b, i):
    x, y = i % W, i // W
    t, f = terrain[i], feature[i]
    if not t or i in IMPROVEMENT or f in NO_BONUS_FEATURES or EUROPE[i] or PACIFIC[i]:
        return False
    if plot_type[i] == 0 and not b['peaks']:
        return False
    if f:
        if f not in b['F'] or t not in b['FT']:
            return False
    elif t not in b['T']:
        return False
    if (plot_type[i] == 1 and not b['hills']) or (plot_type[i] == 2 and not b['flat']):
        return False
    if land[i] and ((b['no_river'] and river_side(x, y)) or (b['river_only'] and not river_side(x, y))):
        return False
    if b['coastal'] and not (land[i] and any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y))):
        return False
    if area_size[i] < b['min_area']:
        return False
    if not b['min_lat'] <= real_lat(y) <= b['max_lat']:
        return False
    if b['whale'] and any(terrain[I(nx, ny)] in ('COAST', 'SHALLOW_COAST') for nx, ny in nbrs(x, y)):
        return False
    return True


def can_place(b, i, mixed=False, same=False):
    """mixed: another resource may lie next to it; same: the same resource may lie closer than its unique range."""
    x, y = i % W, i // W
    if i in BONUS or (not mixed and any(BONUS.get(I(nx, ny), b['type']) != b['type'] for nx, ny in nbrs(x, y))):
        return False
    r = 0 if same else b['unique']
    return not any(BONUS.get(I(nx, ny)) == b['type'] for nx in range(x - r, x + r + 1) for ny in range(y - r, y + r + 1)
                   if inb(nx, ny) and (nx, ny) != (x, y))


def amount(b, ncands):
    count = (b['const'] + sum(brng.randrange(r) for r in b['rands'] if r > 0)) * (ncands // b['tiles_per']) // 100
    return int(round(count * PRECIOUS.get(b['type'], LAND_AMOUNT if land[CANDS[b['type']][0]] else 1.0)))


CANDS = dict((b['type'], [i for i in range(W * H) if can_have(b, i)]) for b in BONUS_INFOS)
BONUS_INFOS = [b for b in BONUS_INFOS if CANDS[b['type']] and b['tiles_per'] > 0]
COUNTS = dict((b['type'], amount(b, len(CANDS[b['type']]))) for b in BONUS_INFOS)
# the real deposits first (precious metals before the rest), so that iron does not take the Harz before silver
by_type = dict((b['type'], b) for b in BONUS_INFOS)
for t in sorted(DEPOSITS, key=lambda t: (t not in PRECIOUS, t)):
    if t not in by_type:
        continue
    b = by_type[t]
    for lo, la, r, m in DEPOSITS[t]:
        dx, dy = to_xy(lo, la)
        near = sorted((math.hypot(i % W + 0.5 - dx, i // W + 0.5 - dy) + brng.random() + (3 if plot_type[i] == 0 else 0), i) for i in CANDS[t]
                      if math.hypot(i % W + 0.5 - dx, i // W + 0.5 - dy) < r + 1 and can_place(b, i))
        if near:
            BONUS[near[0][1]] = t
            COUNTS[t] -= 1

RICH = Shifted(Noise(SEED * 1000 + 300))
for k, b in enumerate(BONUS_INFOS):
    cands, count = CANDS[b['type']], COUNTS[b['type']]
    if count <= 0:
        continue
    prov = Shifted(Noise(SEED * 1000 + 400 + k))
    deps = [to_xy(lo, la) + (r, m) for lo, la, r, m in DEPOSITS.get(b['type'], [])]
    weight = {}
    for i in cands:
        x, y = i % W, i // W
        w = math.exp(4.0 * prov.fbm(x, y, 3, 14) + (RICHNESS * RICH.fbm(x, y, 3, 10) if land[i] else 0.0))
        for dx, dy, r, m in deps:
            d = math.hypot(x + 0.5 - dx, y + 0.5 - dy)
            if d < r:
                w *= 1 + m * (1 - d / r)
        if plot_type[i] == 0 and b['type'] in PRECIOUS:
            w *= 0.1                   # gold, silver and gems mostly in the hills, seldom on peaks
        weight[i] = w
    group = GROUP_CHANCE.get(b['type'].rstrip('_0123456789'), 0.3)
    loose = int(count * NEIGHBOUR_SHARE + brng.random())      # placements that may touch another resource
    while count > 0 and weight:
        keys = list(weight)
        i = brng.choices(keys, [weight[j] for j in keys])[0]
        del weight[i]
        if not can_place(b, i):
            if loose <= 0 or not can_place(b, i, mixed=True):
                continue
            loose -= 1
        BONUS[i] = b['type']
        count -= 1
        x, y = i % W, i // W
        r = b['unique'] + 2
        near = [j for j in weight if max(abs(j % W - x), abs(j // W - y)) <= r]
        brng.shuffle(near)
        for j in near:                 # a small group of the same resource around it, now and then side by side
            if count <= 0 or brng.random() >= group:
                break
            ok = can_place(b, j)
            if not ok and loose > 0 and can_place(b, j, mixed=True, same=True):
                ok, loose = True, loose - 1
            if ok:
                BONUS[j] = b['type']
                count -= 1
                del weight[j]


# ---------------------------------------------------------------- natives: every tribe gets a home that fits its way of life
# The map is a scenario: all players are set (8 colonial powers off their own coasts, 31 tribes, the kings), so the game
# does not place the natives by its north/south rule (which would put the Inuit into the Sahara on this flipped map).
# Each tribe has a home region; its land grows from there along the terrain (mountains and sea are costly), and the
# villages are set in it by the quality of the site and the tribe's way of life, unevenly and clustered.
NATIVE_KINDS = {   # preferred terrain, preferred plot types, settled (bigger, closer villages) or not
    'arctic': ({'TUNDRA': 3, 'SNOW': 1, 'TAIGA': 2}, {1: 1, 2: 1}, False),
    'boreal': ({'TAIGA': 3, 'TUNDRA': 1, 'WETLAND': 2, 'GRASS': 1}, {1: 1, 2: 1}, False),
    'forest': ({'GRASS': 3, 'PLAINS_FERTILE': 2, 'WETLAND': 1, 'TAIGA': 1, 'PLAINS': 1}, {1: 1, 2: 1}, False),
    'plains': ({'PLAINS': 3, 'PLAINS_FERTILE': 3, 'GRASS': 2, 'ROCK_STEPPES': 1, 'SHRUBLAND': 1}, {2: 2, 1: 0.5}, False),
    'steppe': ({'PLAINS': 3, 'ROCK_STEPPES': 2, 'SHRUBLAND': 2, 'PLAINS_FERTILE': 1}, {2: 2, 1: 0.5}, False),
    'desert': ({'DESERT': 2, 'ROCK_STEPPES': 3, 'SHRUBLAND': 2, 'PLAINS': 1}, {2: 1, 1: 1}, False),
    'highland': ({'PLAINS': 2, 'SHRUBLAND': 2, 'GRASS': 2, 'PLAINS_FERTILE': 2, 'ROCK_STEPPES': 1}, {1: 3, 2: 1}, True),
    'mountain': ({'GRASS': 2, 'TAIGA': 2, 'PLAINS': 2, 'TUNDRA': 1, 'PLAINS_FERTILE': 1}, {1: 3, 2: 1}, False),
    'river': ({'GRASS': 2, 'PLAINS_FERTILE': 3, 'MARSH': 2, 'WETLAND': 2, 'SAVANNAH': 2}, {2: 2, 1: 0.5}, True),
    'wetland': ({'WETLAND': 3, 'MARSH': 3, 'GRASS': 2, 'TAIGA': 1}, {2: 2, 1: 0.5}, False),
    'coast': ({'GRASS': 2, 'PLAINS': 2, 'SHRUBLAND': 2, 'PLAINS_FERTILE': 2, 'SAVANNAH': 2}, {2: 1, 1: 1}, True),
    'island': ({'GRASS': 2, 'PLAINS': 2, 'SHRUBLAND': 2, 'SAVANNAH': 2, 'MARSH': 1}, {2: 1, 1: 1}, True),
}
TRIBES = [   # civilization, home (lon, lat), way of life, villages
    ('INUIT', (22.0, 69.0), 'arctic', 5), ('ALGONQUIAN', (30.0, 62.5), 'boreal', 6), ('HURON', (15.0, 60.5), 'boreal', 6),
    ('IROQUOIS', (22.0, 54.0), 'forest', 8), ('CHEROKEE', (12.0, 49.0), 'forest', 8), ('NEZ_PERCE', (9.0, 46.5), 'mountain', 5),
    ('BLACKFOOT', (33.0, 48.0), 'plains', 6), ('SIOUX', (20.0, 47.0), 'plains', 7), ('COMANCHE', (44.0, 48.0), 'steppe', 6),
    ('CROW', (-3.5, 38.0), 'plains', 6), ('SHOSHONE', (-5.0, 41.5), 'steppe', 7), ('APACHE', (3.0, 30.5), 'desert', 6),
    ('NAVAJO', (40.0, 30.0), 'desert', 6), ('AZTEC', (33.0, 39.0), 'highland', 9), ('MIXTEC', (46.0, 34.0), 'river', 7),
    ('ZAPOTEC', (44.0, 40.5), 'mountain', 6), ('TARASCANS', (21.0, 43.0), 'highland', 7), ('MAYA', (12.5, 42.5), 'coast', 8),
    ('GUAYMI', (22.0, 39.0), 'coast', 6), ('ARAWAK', (9.0, 40.5), 'island', 4), ('CARIBS', None, 'island', 6),
    ('INCA', (-6.0, 31.5), 'highland', 9), ('MUISCA', (14.0, 31.0), 'highland', 7), ('MAPUCHE', (-1.5, 52.5), 'forest', 7),
    ('TEHUELCHE', (-8.0, 53.0), 'steppe', 5), ('TUPI', (0.0, 47.5), 'coast', 7), ('GUARANI', (1.0, 44.5), 'river', 7),
    ('SHUAR', (27.0, 52.0), 'wetland', 6), ('OMAGUA', (26.0, 44.5), 'river', 7), ('MACHIPARO', (7.0, 52.5), 'wetland', 6),
    ('TOROMONA', (31.0, 29.0), 'river', 6),
]
nrng = random.Random(SEED * 7 + 83)
NATIVE_OK = [land[i] and plot_type[i] in (1, 2) and terrain[i] not in ('SNOW',) and i not in BONUS and i not in IMPROVEMENT
             and feature[i] not in NO_BONUS_FEATURES and area_size[i] >= 6 for i in range(W * H)]
SITE_NZ = Shifted(Noise(SEED * 1000 + 500))


def site_value(i, kind):
    """Quality of a village site for a tribe: its terrain, the land around, rivers and coast, plus noise for uneven spread."""
    terr, plots, _ = NATIVE_KINDS[kind]
    x, y = i % W, i // W
    v = terr.get(terrain[i], 0.3) * plots.get(plot_type[i], 0.3)
    ring = [I(nx, ny) for nx, ny in nbrs(x, y)]
    v += 0.25 * sum(terr.get(terrain[j], 0.2) for j in ring if land[j] and plot_type[j] != 0)
    v -= 0.6 * sum(1 for j in ring if land[j] and plot_type[j] == 0)
    nwater = sum(1 for j in ring if ocean[j])
    if nwater > 4:
        return 0.0
    if river_side(x, y) or any(large[j] or lake[j] for j in ring):
        v += 2.5 if kind in ('river', 'wetland') else 1.2
    if nwater:
        v += 2.0 if kind in ('coast', 'island', 'arctic') else 0.4
    v *= math.exp(1.2 * SITE_NZ.fbm(x, y, 3, 6))
    return max(v, 0.0)


def snap_home(lon, la):
    hx, hy = to_xy(lon, la)
    best = None
    for i in range(W * H):
        if NATIVE_OK[i] and area_size[i] >= 20 and i not in homes:
            d = math.hypot(i % W + 0.5 - hx, i // W + 0.5 - hy)
            if best is None or d < best[0]:
                best = (d, i)
    return best[1]


VILLAGES = {}                          # plot -> tribe index
TRIBE_LAND = [-1] * (W * H)
homes = []
for t, (civ, home, kind, n) in enumerate(TRIBES):
    if home is None:                   # the Caribs live on the islands of the eastern sea
        cells = [i for cl in ISLES for i in cl if NATIVE_OK[i]]
        home = from_xy(sum(i % W for i in cells) / float(len(cells)) + 0.5, sum(i // W for i in cells) / float(len(cells)) + 0.5)
    homes.append(snap_home(*home))
# tribal land: grows from all homes at once, across land; mountains, large rivers and short sea crossings are costly
cost = {}
pq = []
for t, h in enumerate(homes):
    cost[h] = 0.0
    TRIBE_LAND[h] = t
    heapq.heappush(pq, (0.0, h, t))
while pq:
    c, i, t = heapq.heappop(pq)
    if c > cost.get(i, 1e9) or c > 20:
        continue
    for nx, ny in nbrs(i % W, i // W):
        j = I(nx, ny)
        if land[j]:
            step = {0: 3.5, 1: 1.4}.get(plot_type[j], 1.0)
        elif large[j] or lake[j]:
            step = 2.0
        elif ocean[j] and dland[j] <= 2 and not EUROPE[j]:
            step = 2.5 if TRIBES[t][2] == 'island' else 4.0
        else:
            continue
        if c + step < cost.get(j, 1e9):
            cost[j] = c + step
            TRIBE_LAND[j] = t
            heapq.heappush(pq, (c + step, j, t))
for t, (civ, home, kind, n) in enumerate(TRIBES):
    settled = NATIVE_KINDS[kind][2]
    gap = 3 if settled else 4         # Chebyshev distance between villages of one tribe
    own = [i for i in range(W * H) if TRIBE_LAND[i] == t and NATIVE_OK[i]]
    vals = dict((i, site_value(i, kind)) for i in own)
    order = [homes[t]]
    placed = []
    tries = 0
    while len(placed) < n and tries < 400:
        tries += 1
        if order:
            i = order.pop()
        else:
            free = [j for j in own if vals[j] > 0 and j not in VILLAGES]
            if not free:
                break
            i = nrng.choices(free, [vals[j] ** 2 for j in free])[0]
        x, y = i % W, i // W
        if any(max(abs(x - v % W), abs(y - v // W)) < (gap if VILLAGES[v] == t else 3) for v in VILLAGES):
            continue
        if placed and min(max(abs(x - p % W), abs(y - p // W)) for p in placed) > 10:
            continue                   # stay together: within 10 tiles of another village of the tribe
        VILLAGES[i] = t
        placed.append(i)
CAPITALS = dict((t, homes[t]) for t in range(len(TRIBES)) if homes[t] in VILLAGES)
TRIBE_CONTACTS = [set() for _ in TRIBES]
for i in range(W * H):
    t = TRIBE_LAND[i]
    if t >= 0:
        for nx, ny in nbrs(i % W, i // W):
            u = TRIBE_LAND[I(nx, ny)]
            if u >= 0 and u != t:
                TRIBE_CONTACTS[t].add(u)
                TRIBE_CONTACTS[u].add(t)
# the colonial powers start in the Europe zone, spread evenly over the whole height of the east side, in the order of
# their homes from south to north: Portugal (top, off Africa), Spain, France, England, Netherlands, Denmark, Sweden,
# Russia (bottom, off Norway). Each start is the Europe plot nearest to land in its row band.
COLONIAL_ORDER = [3, 2, 0, 1, 4, 5, 6, 7]     # player numbers of eporue_players.txt, top to bottom
COLONIAL_STARTS = [None] * len(COLONIAL_ORDER)
for k, p in enumerate(COLONIAL_ORDER):
    ty = (H - 1) - 6 - k * (H - 13) / (len(COLONIAL_ORDER) - 1.0)
    best = min((abs(i // W - ty) * 3 + dland[i], i) for i in range(W * H)
               if EUROPE[i] and i % W >= W // 2 and dland[i] >= 4 and abs(i // W - ty) <= 4)
    COLONIAL_STARTS[p] = best[1]


WGLYPH = {'OCEAN': '.', 'COAST': '-', 'SHALLOW_COAST': '_'}


def ascii_terrain(x, y):
    i = I(x, y)
    if large[i]:
        return '=' if i in FORD else '~'
    if lake[i]:
        return 'L'
    if ocean[i]:
        return ':' if EUROPE[i] else (';' if PACIFIC[i] else WGLYPH[WATER_TERRAIN[i]])
    if plot_type[i] == 0:
        return '^'
    c = TCH[terrain[i]]
    return c.upper() if plot_type[i] == 1 else c


def ascii_features(x, y):
    i = I(x, y)
    if large[i]:
        return '=' if i in FORD else '~'
    if lake[i]:
        return 'L'
    if ocean[i]:
        return ':' if EUROPE[i] else (';' if PACIFIC[i] else WGLYPH[WATER_TERRAIN[i]])
    if plot_type[i] == 0:
        return '^'
    f = feature[i]
    return FCH.get(f, '?') if f else ('n' if plot_type[i] == 1 else ' ')


def ascii_map(fn):
    rows = ['     ' + ''.join(str(x // 10) if x % 10 == 0 else ' ' for x in range(W)), '     ' + ''.join(str(x % 10) for x in range(W))]
    for y in range(H - 1, -1, -1):
        rows.append('%3d  %s' % (y, ''.join(fn(x, y) for x in range(W))))
    return '\n'.join(rows)


def write_preview(path):
    COL = {
        'OCEAN': (28, 58, 110), 'COAST': (45, 95, 160), 'SHALLOW_COAST': (70, 140, 185), 'LAKE': (60, 120, 200),
        'LARGE_RIVERS': (40, 110, 230), 'SNOW': (240, 240, 245), 'TUNDRA': (170, 175, 160), 'TAIGA': (110, 140, 115),
        'GRASS': (90, 170, 70), 'PLAINS_FERTILE': (165, 190, 90), 'PLAINS': (200, 185, 110), 'SAVANNAH': (175, 175, 60),
        'SHRUBLAND': (200, 160, 100), 'DESERT': (235, 215, 150), 'ROCK_STEPPES': (165, 145, 120),
        'MARSH': (80, 130, 100), 'WETLAND': (100, 140, 140),
    }
    S = 8
    img = [[(0, 0, 0)] * (W * S) for _ in range(H * S)]
    for x, y in ALL:
        i = I(x, y)
        if large[i]:
            c = COL['LARGE_RIVERS']
        elif lake[i]:
            c = COL['LAKE']
        elif ocean[i]:
            c = (22, 45, 90) if EUROPE[i] or PACIFIC[i] else COL[WATER_TERRAIN[i]]
        else:
            c = COL[terrain[i]]
        f = feature[i] or ''
        for sy in range(S):
            for sx in range(S):
                col = c
                if land[i] and plot_type[i] == 1 and (sx + sy) % 4 == 0:
                    col = tuple(max(0, v - 45) for v in c)
                if land[i] and plot_type[i] == 0:
                    col = (95, 85, 80) if abs(sx - S // 2) <= sy // 2 else c
                if f == 'VOLCANO' and abs(sx - S // 2) <= sy // 2:
                    col = (200, 40, 20)
                if ('FOREST' in f or f in ('JUNGLE', 'TROPICAL_GROVES', 'MANGROVE')) and (sx % 3 == 1 and sy % 3 == 1):
                    col = (20, 80, 30) if f in ('DENSE_FOREST', 'JUNGLE') else (30, 100, 40)
                if f == 'RIVER_FORD' and sy in (3, 4):
                    col = (170, 140, 90)
                if f in ('REEF', 'STONE_REEF', 'PEARL_REEF') and 2 <= sx <= 5 and 2 <= sy <= 5:
                    col = (230, 150, 120) if f != 'PEARL_REEF' else (250, 250, 250)
                if f in ('DENSE_FOREST', 'ANCIENT_FOREST') and (sx % 3 == 1 and sy % 3 == 1) and i in HARDWOOD:
                    col = (10, 40, 15)
                img[(H - 1 - y) * S + sy][x * S + sx] = col
    for x, y, _ in SIGNS:
        for sy in range(2):
            for sx in range(2):
                img[(H - 1 - y) * S + sy][x * S + sx] = (255, 255, 255)
    for i in BONUS:                    # resources: a yellow dot at the lower right
        x, y = i % W, i // W
        for sy in (5, 6):
            for sx in (5, 6):
                img[(H - 1 - y) * S + sy][x * S + sx] = (255, 215, 0)
    for i, t in VILLAGES.items():      # native villages: a square in the colour of the tribe, capitals with a white rim
        x, y = i % W, i // W
        col = TRIBE_COLOURS[t % len(TRIBE_COLOURS)]
        for sy in range(1, 7):
            for sx in range(1, 7):
                edge = sy in (1, 6) or sx in (1, 6)
                img[(H - 1 - y) * S + sy][x * S + sx] = (255, 255, 255) if edge and CAPITALS.get(t) == i else ((0, 0, 0) if edge else col)
    for i in COLONIAL_STARTS:
        x, y = i % W, i // W
        for sy in range(1, 7):
            for sx in range(1, 7):
                if sx == sy or sx == 7 - sy:
                    img[(H - 1 - y) * S + sy][x * S + sx] = (255, 255, 255)
    RC = (40, 110, 230)
    for i in wofr:
        x, y = i % W, i // W
        for sy in range(S):
            img[(H - 1 - y) * S + sy][x * S + S - 1] = RC
            if x + 1 < W:
                img[(H - 1 - y) * S + sy][(x + 1) * S] = RC
    for i in nofr:
        x, y = i % W, i // W
        for sx in range(S):
            img[(H - 1 - y) * S + S - 1][x * S + sx] = RC
            if y - 1 >= 0:
                img[(H - 1 - (y - 1)) * S][x * S + sx] = RC
    write_png(path, W * S, H * S, img)


TRIBE_COLOURS = [(220, 40, 40), (40, 40, 220), (230, 130, 20), (150, 40, 180), (20, 170, 170), (240, 240, 60), (120, 70, 30),
                 (240, 120, 200), (90, 200, 60), (60, 60, 60)]
TOP_LAT, BOTTOM_LAT = 70, -30
WB_NAME = 'Eporue Massive'
# game settings of a scenario (the options of the test games on this map)
GAME_HEADER = '''Era=ERA_DISCOVERY
Speed=GAMESPEED_TAC_NORMAL
Calendar=CALENDAR_DEFAULT
Option=GAMEOPTION_TWO_PLOT_CITY_RADIUS
Option=GAMEOPTION_NEW_MOVEMENT_COST
Option=GAMEOPTION_NO_MORE_VARIABLES_HIDDEN
Option=GAMEOPTION_REDUCED_CITY_DISTANCE
Option=GAMEOPTION_GOODIES_ALWAYS_DISPLAY_COLOURED_CIRCLE
Option=GAMEOPTION_RANDOM_FOUNDING_FATHERS
Victory=VICTORY_REVOLUTION
Victory=VICTORY_DOMINATION
GameTurn=0
MaxTurns=0
MaxCityElimination=0
NumAdvancedStartPoints=720
TargetScore=0
StartYear=1492
Description=TXT_KEY_WORLDBUILDER_EPORUE_DESC
ModPath=
WBNorthAmericanNative=1
WBSouthAmericanNative=1
WBCentralAmericanNative=1'''
COLONIAL_PLAYERS, KING_TEAMS, NONE_TEAMS = range(8), range(39, 47), (47, 48)


def read_players():
    """Players of the scenario (eporue_players.txt: the 49 players of 'The New World Gigantic - Historic'): civ type -> block."""
    import re
    text = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'eporue_players.txt')).read()
    return re.findall(r'BeginPlayer\n.*?EndPlayer\n', text, re.S)


def write_players(f):
    import re
    blocks = read_players()
    pid = dict((re.search(r'CivType=CIVILIZATION_(\w+)', b).group(1), k) for k, b in enumerate(blocks) if 'CivType=CIVILIZATION_' in b)
    tribe_pid = [pid[civ] for civ, _, _, _ in TRIBES]
    contacts = dict((p, set()) for p in range(len(blocks)))
    for p in COLONIAL_PLAYERS:
        contacts[p] |= set(KING_TEAMS) | set(NONE_TEAMS)
    for k in KING_TEAMS:
        contacts[k] |= set(COLONIAL_PLAYERS) | {47}
    contacts[47] |= set(COLONIAL_PLAYERS) | set(KING_TEAMS) | {48}
    contacts[48] |= set(COLONIAL_PLAYERS) | {47}
    for t, others in enumerate(TRIBE_CONTACTS):
        contacts[tribe_pid[t]] |= set(tribe_pid[u] for u in others)
    for p in range(len(blocks)):
        f.write('BeginTeam\n\tTeamID=%d\n' % p)
        for q in sorted(contacts[p]):
            f.write('\tContactWithTeam=%d\n' % q)
        if p in COLONIAL_PLAYERS:
            f.write('\tAtWar=48\n')
        f.write('\tRevealMap=0\nEndTeam\n')
    for p, b in enumerate(blocks):
        start = None
        if p in COLONIAL_PLAYERS:
            start, gold = COLONIAL_STARTS[p], 100
        elif p in tribe_pid:
            start, gold = CAPITALS.get(tribe_pid.index(p)), 70
        if start is not None:
            b = re.sub(r'StartingGold=\d+', 'StartingGold=%d' % gold, b)
            b = b.replace('\tStartingEra=', '\tStartingX=%d, StartingY=%d\n\tStartingEra=' % (start % W, start // W))
        f.write(b)
    return tribe_pid


def write_wbsave(path):
    with open(path, 'w', newline='\r\n') as f:
        f.write('Version=11\nBeginGame\n\t%s\nEndGame\n' % GAME_HEADER.replace('\n', '\n\t'))
        tribe_pid = write_players(f)
        f.write('BeginMap\n\tgrid width=%d\n\tgrid height=%d\n\ttop latitude=%d\n\tbottom latitude=%d\n' % (W, H, TOP_LAT, BOTTOM_LAT))
        f.write('\twrap X=0\n\twrap Y=0\n\tworld size=WORLDSIZE_MASSIVE\n\tclimate=CLIMATE_TEMPERATE\n\tsealevel=SEALEVEL_MEDIUM\n')
        f.write('\tnum plots written=%d\n\tnum signs written=%d\n' % (W * H, len(SIGNS)))
        f.write('\tRandomize Features=false\n\tRandomize Resources=false\n\tRandomize Goodies=true\nEndMap\n\n### Plot Info ###\n')
        for x in range(W):
            for y in range(H):
                i = I(x, y)
                f.write('BeginPlot\n\tx=%d,y=%d\n' % (x, y))
                if i in wofr:
                    f.write('\tRiverNSDirection=%d\n\tisWOfRiver\n' % wofr[i])
                if i in nofr:
                    f.write('\tRiverWEDirection=%d\n\tisNOfRiver\n' % nofr[i])
                if EUROPE[i]:
                    f.write('\tEuropeType=EUROPE_EAST\n')
                elif PACIFIC[i]:
                    f.write('\tEuropeType=EUROPE_WEST\n')
                if i in BONUS:
                    f.write('\tBonusType=BONUS_%s\n' % BONUS[i])
                if i in IMPROVEMENT:
                    f.write('\tImprovementType=%s\n' % IMPROVEMENT[i])
                if feature[i]:
                    f.write('\tFeatureType=FEATURE_%s, FeatureVariety=0\n' % feature[i])
                f.write('\tTerrainType=TERRAIN_%s\n\tPlotType=%d\n' % (terrain[i], plot_type[i]))
                if i in VILLAGES:      # a native village with its braves (the game skips its own native set-up in a scenario)
                    t = VILLAGES[i]
                    capital = CAPITALS.get(t) == i
                    pop = (3 if NATIVE_KINDS[TRIBES[t][2]][2] else 2) + capital
                    for _ in range(2 + capital):
                        f.write('\tBeginUnit\n\t\tUnitType=UNIT_NATIVE, UnitOwner=%d\n\t\tProfession=PROFESSION_BRAVE\n\t\tDamage=0\n'
                                '\t\tLevel=1, Experience=0\n\t\tFacingDirection=4\n\t\tUnitAIType=UNITAI_DEFENSIVE\n\tEndUnit\n' % tribe_pid[t])
                    f.write('\tBeginCity\n\t\tCityOwner=%d\n\t\tCityPopulation=%d\n%s\tEndCity\n' % (tribe_pid[t], pop, '\t\tCitizenType=UNIT_NATIVE\n' * pop))
                f.write('EndPlot\n')
        f.write('\n### Sign Info ###\n')
        for x, y, caption in SIGNS:
            f.write('BeginSign\n\tplotX=%d\n\tplotY=%d\n\tplayerType=-1\n\tcaption=%s\nEndSign\n' % (x, y, caption))


if __name__ == '__main__':
    from collections import Counter
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'ascii_terrain.txt'), 'w') as f:
        f.write(ascii_map(ascii_terrain) + '\n')
    with open(os.path.join(OUT, 'ascii_features.txt'), 'w') as f:
        f.write(ascii_map(ascii_features) + '\n')
    write_preview(os.path.join(OUT, 'preview.png'))
    write_wbsave(os.path.join(OUT, WB_NAME + '.ColonizationWBSave'))
    print('edge rivers', len(wofr) + len(nofr), 'hardwood clusters', len(hw_centres), len(HARDWOOD), 'signs', len(SIGNS), 'lanes', lanes, 'europe', sum(EUROPE), 'pacific', sum(PACIFIC))
    print('water', sorted(Counter(WATER_TERRAIN[i] for i in range(W * H) if ocean[i]).items()))
    print('signs', [(c, x, y) for x, y, c in SIGNS])
    nl = sum(land)
    print('river plots without a straight neighbour:', sum(1 for i in range(W * H) if large[i] and not any(large[I(nx, ny)] or ocean[I(nx, ny)] or lake[I(nx, ny)] for nx, ny in nbrs(i % W, i // W, N4))))
    print('rivers thinned', RIVERS_THINNED, 'oases', OASES, 'salt pans', len(SALT_PANS))
    print('land %d (%.0f%%)' % (nl, 100.0 * nl / (W * H)), 'large river plots', sum(large), 'lakes', sum(lake), 'clay scatter', len(CLAY_SCATTER), 'lone hills', len(LONE))
    print(sorted(Counter(terrain[i] for i in range(W * H) if land[i]).items(), key=lambda kv: -kv[1]))
    print(sorted(Counter(feature[i] for i in range(W * H) if feature[i]).items(), key=lambda kv: -kv[1]))
    print('peaks', sum(1 for i in range(W * H) if land[i] and plot_type[i] == 0), 'hills', sum(1 for i in range(W * H) if land[i] and plot_type[i] == 1))
    print('resources', len(BONUS), sorted(Counter(BONUS.values()).items(), key=lambda kv: -kv[1]))
    print('villages', len(VILLAGES), [(TRIBES[t][0], sum(1 for v in VILLAGES.values() if v == t), from_xy(homes[t] % W, homes[t] // W)) for t in range(len(TRIBES))])
    print('colonial starts', [(i % W, i // W) for i in COLONIAL_STARTS])
