"""Eporue (Europe backwards): flipped Europe map generator for WTP (draft stage: land, relief, climate, terrain, features -> ASCII + PNG).

Europe is turned by 180 degrees: the Atlantic lies in the east (Europe side of the game), Scandinavia in the south,
the Mediterranean (with a strip of North Africa) in the north. The real coastline is drawn in lon/lat and projected
onto the tile grid; climate is a free (warmer) variant. Python 3, standard library only.

    python Tools/MapGenerator/gen_europe.py [seed] [output dir]
"""
import sys, os, math, random, heapq
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from noise import Noise, poly_dist, smoothstep, interp
from png import write_png

W, H = 80, 100
SEED = int(sys.argv[1]) if len(sys.argv) > 1 else 1
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out_eporue')
rng = random.Random(SEED)
NZ = [Noise(SEED * 1000 + i) for i in range(24)]

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
SX, SY, Y0 = 1.1, 2.2, 5.0            # tiles per degree of longitude / latitude, bottom margin
LON_MAX, LAT_MAX = 47.0, 71.5          # lon 47 E = left edge, lat 71.5 N = near the bottom edge
LAT_SHIFT = 4.0                        # free climate: everything counts as 4 degrees further north (a little warmer)


def to_xy(lon, lat):
    return ((LON_MAX - lon) * SX, (LAT_MAX - lat) * SY + Y0)


def real_lat(y):
    return LAT_MAX - (y + 0.5 - Y0) / SY


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
    (36.0, 69.0), (38.0, 67.4), (41.0, 67.2), (44.5, 67.0), (45.6, 63.0), (44.8, 58.0), (45.6, 52.0), (44.6, 48.0), (43.2, 46.8), (42.4, 45.2), (43.4, 43.8), (45.6, 43.0), (45.6, 42.0), (44.8, 38.0), (45.4, 35.0), (43.6, 33.6), (42.8, 32.0), (44.6, 31.2), (45.2, 30.0), (34.3, 30.0), (34.3, 31.3), (34.9, 32.5),
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
    (32.2, 31.3), (34.0, 31.2), (34.3, 31.3), (45.2, 31.3), (45.6, 30.4), (44.0, 29.8), (38.0, 29.4), (30.0, 29.7), (22.0, 29.2), (14.0, 29.8),
    (6.0, 29.4), (-2.0, 29.9), (-8.0, 29.5), (-12.0, 30.0),
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
    'Black Sea': P([(29.1, 41.3), (28.1, 41.9), (28.0, 43.2), (28.6, 44.2), (29.6, 45.2), (30.7, 46.4), (31.5, 46.6), (32.5, 46.0),
                    (33.0, 44.7), (34.0, 44.5), (35.0, 44.8), (36.5, 45.3), (35.8, 46.2), (37.5, 46.6), (38.8, 47.2), (39.2, 46.6),
                    (38.0, 46.0), (37.8, 44.9), (39.7, 43.6), (41.6, 42.0), (41.6, 41.6), (41.0, 41.0), (38.0, 40.9),
                    (36.3, 41.3), (35.1, 42.0), (33.5, 42.0), (31.5, 41.2)]),
}
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
land = [False] * (W * H)
for x, y in ALL:
    px = x + 0.5 + 0.45 * WARP[0].fbm(x, y, 3, 6)
    py = y + 0.5 + 0.45 * WARP[1].fbm(x, y, 3, 6)
    ok = inside(MAINLAND, px, py) or inside(AFRICA, px, py) or any(inside(p, px, py) for p in ISLANDS.values())
    if ok and any(inside(p, px, py) for p in SEAS.values()):
        ok = False
    if ok:
        for pts, hw in CH:
            if poly_dist(x + 0.5, y + 0.5, pts)[0] <= hw:
                ok = False
    land[I(x, y)] = ok
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


lake = [False] * (W * H)
large = [False] * (W * H)
plot_type = [3 if not land[i] else 2 for i in range(W * H)]      # 0 peak, 1 hills, 2 flat, 3 water

# ---------------------------------------------------------------- lakes
LAKES = [(31.3, 60.7, 1.7, 1.0), (35.4, 61.9, 1.0, 1.1), (27.7, 58.6, 0.8, 1.1), (13.2, 58.9, 1.0, 0.55), (14.5, 58.2, 0.5, 0.9),
         (16.5, 59.5, 0.9, 0.35), (6.5, 46.4, 0.7, 0.3), (9.4, 47.6, 0.6, 0.3), (17.8, 46.9, 1.0, 0.3), (28.2, 61.6, 1.2, 0.9),
         (10.8, 45.6, 0.4, 0.45), (8.6, 46.0, 0.45, 0.4)]
for lo, la, rx, ry in LAKES:
    for x, y in ALL:
        i = I(x, y)
        if not land[i]:
            continue
        lon, lat = LON_MAX - (x + 0.5) / SX, LAT_MAX - (y + 0.5 - Y0) / SY
        n = 1 + 0.35 * NZ[2].fbm(x, y, 2, 3)
        if ((lon - lo) / rx) ** 2 + ((lat - la) / ry) ** 2 < n:
            if not any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y)):
                lake[i], land[i], plot_type[i] = True, False, 3
# glacial lake district: Finland and Karelia
for x, y in ALL:
    i = I(x, y)
    lon, lat = LON_MAX - (x + 0.5) / SX, LAT_MAX - (y + 0.5 - Y0) / SY
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
    ('Dinaric', [(14.0, 45.5), (16.5, 44.0), (18.5, 43.0), (20.0, 41.5), (21.0, 40.0), (22.0, 38.6)], 2.0),
    ('Carpathians', [(17.5, 49.0), (19.5, 49.4), (22.5, 49.0), (24.5, 48.2), (25.5, 47.2), (25.8, 46.0), (24.5, 45.5), (22.5, 45.2)], 2.2),
    ('Balkan', [(22.5, 43.4), (25.0, 42.8), (27.5, 42.8)], 1.6),
    ('Caucasus', [(38.5, 44.2), (41.0, 43.3), (44.5, 42.6)], 2.4),
    ('Pontic', [(31.0, 41.2), (35.0, 41.3), (40.0, 40.8)], 1.5),
    ('Taurus', [(30.0, 37.2), (33.0, 37.1), (36.5, 37.6), (40.0, 38.0)], 2.2),
    ('Kjolen', [(9.8, 60.5), (11.6, 62.0), (13.6, 63.5), (15.4, 65.5), (17.8, 67.5), (20.2, 68.7), (22.8, 69.7)], 2.0),
    ('Highlands', [(-5.3, 57.2), (-4.0, 56.8), (-3.0, 56.7)], 1.4),
    ('Pennines', [(-2.3, 54.5), (-2.0, 53.2)], 0.9),
    ('Tell Atlas', [(-2.5, 35.0), (1.0, 36.0), (5.0, 36.4), (8.0, 36.5)], 1.4),
    ('High Atlas', [(-9.5, 31.2), (-6.5, 32.0), (-3.5, 33.2), (-1.0, 34.0), (2.5, 34.6), (6.0, 34.8), (9.5, 34.6)], 1.9),
    ('Lebanon', [(35.6, 34.5), (36.2, 33.5)], 1.0),
    ('Sinai', [(33.7, 29.5), (34.2, 28.2)], 1.5),
]
BLOBS = [   # centre lon/lat, radius in tiles: hill country
    (3.2, 45.0, 2.8), (8.0, 48.2, 1.8), (14.5, 50.4, 2.4), (5.5, 50.0, 1.6), (10.5, 51.8, 1.2), (-3.5, 52.5, 1.4), (-3.5, 50.5, 1.0),
    (33.0, 50.0, 2.2), (33.0, 57.5, 2.2), (27.0, 56.0, 1.6), (-8.0, 41.5, 1.8), (-1.0, 40.4, 2.0), (-5.5, 37.8, 1.6), (22.0, 41.8, 1.6),
    (16.5, 49.6, 1.8), (23.0, 47.5, 1.4), (14.5, 61.0, 1.8), (16.0, 57.3, 1.2), (-8.5, 53.0, 1.0), (-6.8, 54.8, 0.8),
]
PLATEAUS = [(34.5, 38.8, 4.5, 0.38), (-3.5, 40.0, 4.5, 0.30), (28.0, 61.0, 6.0, 0.18), (45.0, 47.0, 6.0, 0.10)]   # lon, lat, radius, elevation
ridge = [0.0] * (W * H)
elev = [0.0] * (W * H)
RXY = [(n, [to_xy(lo, la) for lo, la in pts], w) for n, pts, w in RANGES]
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    jitter = 0.75 + 0.5 * (0.5 + 0.5 * NZ[5].fbm(x, y, 3, 5))
    v = 0.0
    for n, pts, w in RXY:
        d, _ = poly_dist(x + 0.5, y + 0.5, pts)
        v = max(v, 1 - d / (w * jitter))
    for lo, la, r in BLOBS:
        bx, by = to_xy(lo, la)
        v = max(v, 0.55 * (1 - math.hypot(x + 0.5 - bx, y + 0.5 - by) / r))
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
    if val > 0.66:
        plot_type[i] = 0
    elif val > 0.45 and rrng.random() < 0.85:
        plot_type[i] = 1
    elif val > 0.22 and rrng.random() < 0.5:
        plot_type[i] = 1
# an extra peak here and there next to a chain, so the chains are not uniform walls
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] == 1 and ridge[i] > 0.4 and rrng.random() < 0.10 and any(plot_type[I(nx, ny)] == 0 for nx, ny in nbrs(x, y)):
        plot_type[i] = 0
# lone hills in the open lowlands (resources that need hills), spaced at least three tiles apart
hill_spots = [I(x, y) for x, y in ALL if land[I(x, y)] and plot_type[I(x, y)] == 2 and ridge[I(x, y)] < 0.2]
rrng.shuffle(hill_spots)
LONE = []
for i in hill_spots:
    if len(LONE) >= 130:
        break
    x, y = i % W, i // W
    if all(math.hypot(x - j % W, y - j // W) >= 3 for j in LONE) and not any(plot_type[I(nx, ny)] != 2 for nx, ny in nbrs(x, y) if land[I(nx, ny)]):
        LONE.append(i)
        plot_type[i] = 1

# ---------------------------------------------------------------- large rivers
RIVERS = {
    'Danube': [(8.6, 48.1), (10.0, 48.4), (12.0, 48.9), (13.5, 48.6), (16.4, 48.2), (17.2, 47.9), (19.0, 47.5), (19.1, 46.0), (20.2, 44.8),
               (22.4, 44.6), (24.5, 43.8), (26.5, 44.0), (28.0, 44.6), (29.5, 45.2)],
    'Rhine': [(9.2, 46.7), (9.5, 47.5), (8.2, 47.6), (7.6, 47.6), (8.2, 49.0), (8.4, 50.0), (7.0, 50.8), (6.2, 51.6), (4.6, 51.9), (4.0, 52.0)],
    'Rhone': [(6.1, 46.2), (4.9, 45.8), (4.8, 44.5), (4.7, 43.6), (4.5, 43.2)],
    'Loire': [(4.0, 45.0), (3.6, 46.5), (2.0, 47.5), (0.5, 47.3), (-1.5, 47.2), (-2.3, 47.2)],
    'Elbe': [(15.5, 50.5), (14.0, 50.5), (13.5, 51.0), (11.5, 52.5), (10.0, 53.4), (8.7, 53.9)],
    'Vistula': [(19.0, 49.8), (21.0, 50.0), (21.0, 52.0), (18.8, 53.4), (18.9, 54.5)],
    'Dnieper': [(31.0, 55.0), (30.8, 53.0), (30.5, 50.5), (35.0, 48.5), (34.0, 47.5), (32.3, 46.4)],
    'Don': [(39.0, 52.0), (40.0, 50.0), (40.0, 49.5), (41.0, 48.0), (39.2, 47.0)],
    'Nile': [(32.5, 24.0), (31.0, 28.0), (31.2, 30.0), (31.0, 31.7)],
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


def near_large(x, y):
    return any(large[I(nx, ny)] or lake[I(nx, ny)] for nx, ny in nbrs(x, y))


dcoast = bfs_dist([i for i in range(W * H) if ocean[i]], lambda j: True)
# the Norwegian coast faces the Atlantic: keep a settleable strip (no peaks within three tiles of the water)
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] == 0 and dcoast[i] <= 2 and y < 45 and x >= 26:
        plot_type[i] = 1

# ---------------------------------------------------------------- climate
T = [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    eff = max(0.0, real_lat(y) - LAT_SHIFT)
    t = 1 - (eff / 80.0) ** 1.3
    t -= 0.22 * ridge[i] + (0.03 if plot_type[i] == 1 else 0)
    if real_lat(y) > 44:
        t -= 0.06 * (1 - x / 50.0)                  # continental cold towards the old Russia
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
    t = T[i] + 0.035 * NZ[12].fbm(x, y, 3, 7)
    m = M[i] + 0.07 * NZ[13].fbm(x, y, 3, 6)
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
for i in RIVER_PATHS['Nile']:                          # green ribbon through the desert
    for nx, ny in nbrs(i % W, i // W):
        j = I(nx, ny)
        if land[j] and plot_type[j] == 2 and j not in DELTA and terrain[j] in ('DESERT', 'ROCK_STEPPES', 'SHRUBLAND'):
            terrain[j] = 'SAVANNAH' if T[j] >= 0.8 else 'PLAINS'
            VALLEY_GREEN.add(j)

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
        elif r < 0.30 and not coastal_land(x, y) and any(plot_type[I(nx, ny)] in (0, 1) for nx, ny in nbrs(x, y)):
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

# volcanoes: Etna, Vesuvius, a volcanic Aegean island and the Teide (FEATURE_VOLCANO sits on flat land, hills around the cone)
VOLCANOES = []
for lo, la in ((15.0, 37.75), (14.4, 40.9), (25.5, 36.45), (-16.6, 28.3)):
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


dland = bfs_dist([i for i in range(W * H) if not ocean[i]], lambda j: True)
EUROPE = [ocean[i] and i % W >= W - 16 and dland[i] >= 5 for i in range(W * H)]      # Atlantic: Europe plots, at least 5 tiles from land


def ascii_terrain(x, y):
    i = I(x, y)
    if large[i]:
        return '=' if i in FORD else '~'
    if lake[i]:
        return 'L'
    if ocean[i]:
        return ':' if EUROPE[i] else '.'
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
        return ':' if EUROPE[i] else '.'
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
            c = COL[WATER_TERRAIN[i]] if 'WATER_TERRAIN' in globals() else COL['OCEAN']
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
                img[(H - 1 - y) * S + sy][x * S + sx] = col
    write_png(path, W * S, H * S, img)


if __name__ == '__main__':
    from collections import Counter
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'ascii_terrain.txt'), 'w') as f:
        f.write(ascii_map(ascii_terrain) + '\n')
    with open(os.path.join(OUT, 'ascii_features.txt'), 'w') as f:
        f.write(ascii_map(ascii_features) + '\n')
    write_preview(os.path.join(OUT, 'preview.png'))
    nl = sum(land)
    print('land %d (%.0f%%)' % (nl, 100.0 * nl / (W * H)), 'large river plots', sum(large), 'lakes', sum(lake), 'clay scatter', len(CLAY_SCATTER), 'lone hills', len(LONE))
    print(sorted(Counter(terrain[i] for i in range(W * H) if land[i]).items(), key=lambda kv: -kv[1]))
    print(sorted(Counter(feature[i] for i in range(W * H) if feature[i]).items(), key=lambda kv: -kv[1]))
    print('peaks', sum(1 for i in range(W * H) if land[i] and plot_type[i] == 0), 'hills', sum(1 for i in range(W * H) if land[i] and plot_type[i] == 1))
