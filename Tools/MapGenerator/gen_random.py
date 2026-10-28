"""Random New World generator for WTP (.ColonizationWBSave).

Built on gen.py (Fantasy New World), but nothing is drawn by hand: every seed makes a new world.
Pipeline: tectonic plates (continental crust and ocean floor, each plate drifting) -> height of the crust
plus mountain ranges along colliding plate boundaries (fold mountains between continents, a cordillera with
volcanoes where an ocean plate dives under a continent, volcanic island arcs between ocean plates, rift
valleys where continents part, old worn chains inside the continents) -> sea level by land share -> coast
details -> relief -> lakes -> climate (temperature by latitude and altitude, precipitation by prevailing
winds with orographic rain and rain shadow) -> drainage (large rivers, edge rivers) -> biomes -> set pieces
(searched on the new land) -> features -> connectivity fixes -> WBSave + PNG previews (map, height, rain).
"""
import sys, os, math, random, heapq
from collections import deque, Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from noise import Noise, poly_dist, poly_len, smoothstep, interp
from png import write_png

W, H = 60, 100
SEED = int(sys.argv[1]) if len(sys.argv) > 1 else 1
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out_random')
rng = random.Random(SEED)
NZ = [Noise(SEED * 1000 + i) for i in range(24)]

N4 = [(1, 0), (-1, 0), (0, 1), (0, -1)]
N8 = N4 + [(1, 1), (1, -1), (-1, 1), (-1, -1)]


def I(x, y):
    return y * W + x


def inb(x, y):
    return 0 <= x < W and 0 <= y < H


TOP_LAT, BOTTOM_LAT = 70.0, -40.0


def lat(y):
    return BOTTOM_LAT + (y + 0.5) * (TOP_LAT - BOTTOM_LAT) / H


def lat_y(la):
    """Row of a latitude (inverse of lat)."""
    return int(round((la - BOTTOM_LAT) * H / (TOP_LAT - BOTTOM_LAT) - 0.5))


def nbrs(x, y, d=N8):
    for dx, dy in d:
        nx, ny = x + dx, y + dy
        if 0 <= nx < W and 0 <= ny < H:
            yield nx, ny


ALL = [(x, y) for y in range(H) for x in range(W)]


def blur(a, n=1):
    for _ in range(n):
        b = a[:]
        for x, y in ALL:
            s, c = a[I(x, y)], 1
            for nx, ny in nbrs(x, y):
                s += a[I(nx, ny)]
                c += 1
            b[I(x, y)] = s / c
        a = b
    return a


# ---------------------------------------------------------------- geography: plate tectonics
# The crust is split into plates (a warped, weighted Voronoi partition). Some plates carry continental crust,
# the others ocean floor, and every plate drifts in its own direction. Where plates collide, mountains rise:
# fold mountains between two continents, a cordillera with volcanoes on the continent where an ocean plate
# dives under it, a volcanic island arc where two ocean plates meet. Where two continental plates part, a
# rift valley with long lakes sinks in. The open sea in the east (Atlantic with the Europe plots) and in the
# west (Pacific) stays free of land.
tect = random.Random(SEED * 7 + 1)
MARGIN_E, MARGIN_W = 13, 3          # open water for Europe (east) and the Pacific (west)
LAND_SHARE = 0.34                    # share of land plots on the whole map
CONT_SHARE = 0.40                    # share of the map on continental plates (land and shelf)

NPL = tect.randint(10, 12)
plates = []
for _ in range(2000):
    if len(plates) >= NPL:
        break
    px, py = tect.uniform(-2, W + 2), tect.uniform(-2, H + 2)
    if all(math.hypot(px - p['x'], py - p['y']) > 13 for p in plates):
        a, s = tect.uniform(0, 2 * math.pi), tect.uniform(0.4, 1.0)
        plates.append(dict(x=px, y=py, vx=s * math.cos(a), vy=s * math.sin(a), w=tect.uniform(0.8, 1.2), cont=False))
NPL = len(plates)


def plate_of(x, y):
    wx = x + 5.0 * NZ[0].fbm(x, y, 3, 14)
    wy = y + 5.0 * NZ[1].fbm(x, y, 3, 14)
    return min(range(NPL), key=lambda k: math.hypot(wx - plates[k]['x'], wy - plates[k]['y']) * plates[k]['w'])


plate = [plate_of(x + 0.5, y + 0.5) for x, y in ALL]
plate_area = Counter(plate)
# continental plates: random, but not in the open sea at the east and west edge
order_pl = sorted(range(NPL), key=lambda k: tect.random() + (2 if not MARGIN_W + 8 <= plates[k]['x'] <= W - MARGIN_E - 5 else 0))
got = 0
for k in order_pl:
    if got >= CONT_SHARE * W * H and sum(p['cont'] for p in plates) >= 2:
        break
    plates[k]['cont'] = True
    got += plate_area[k]
crust = blur([1.0 if plates[plate[i]]['cont'] else -1.0 for i in range(W * H)], 4)     # continental shelf and slope

# plate boundaries: which way do the neighbours move relative to each other?
border = {}                     # (a, b) -> boundary points on the side of plate a
for x, y in ALL:
    a = plate[I(x, y)]
    for nx, ny in nbrs(x, y, N4):
        b = plate[I(nx, ny)]
        if b != a:
            border.setdefault((a, b), []).append((x + 0.5, y + 0.5))


def chain_lines(points, min_len=5.0):
    """Order a band of boundary points into polylines (nearest-neighbour walk from one end), every ~3 tiles a point."""
    pts = sorted(set((round(x, 2), round(y, 2)) for x, y in points))
    left, lines = set(range(len(pts))), []
    while left:
        k0 = min(left)
        cur = max(left, key=lambda k: math.hypot(pts[k][0] - pts[k0][0], pts[k][1] - pts[k0][1]))
        line = [pts[cur]]
        left.discard(cur)
        while True:
            cx, cy = pts[cur]
            for k in [k for k in left if math.hypot(pts[k][0] - cx, pts[k][1] - cy) < 1.6]:
                left.discard(k)
            near = [k for k in left if math.hypot(pts[k][0] - cx, pts[k][1] - cy) < 3.3]
            if not near:
                break
            cur = min(near, key=lambda k: math.hypot(pts[k][0] - cx, pts[k][1] - cy))
            line.append(pts[cur])
            left.discard(cur)
        if len(line) < 2 or poly_len(line) < min_len:
            continue
        out, run = [line[0]], 0.0
        for p, q in zip(line, line[1:]):
            run += math.hypot(q[0] - p[0], q[1] - p[1])
            if run >= 3.0:
                out.append(q)
                run = 0.0
        if out[-1] != line[-1]:
            out.append(line[-1])
        if len(out) > 2:                               # soften the zigzag of the grid
            out = [out[0]] + [((a[0] + 2 * b[0] + c[0]) / 4.0, (a[1] + 2 * b[1] + c[1]) / 4.0) for a, b, c in zip(out, out[1:], out[2:])] + [out[-1]]
        lines.append(out)
    return lines


def with_passes(L):
    ps, s = [], tect.uniform(5, 9)
    while s < L - 3:
        ps.append(s)
        s += tect.uniform(10, 15)
    return ps


RANGES, VALLEYS, LAKES = [], [], []   # ranges: pts, width, height, pass positions (arc length), kind
RIFTS = []
for (a, b), pa in sorted(border.items()):
    if a > b or (b, a) not in border:
        continue
    pb = border[(b, a)]
    A, B = plates[a], plates[b]
    nx_, ny_ = B['x'] - A['x'], B['y'] - A['y']
    nl = math.hypot(nx_, ny_) or 1.0
    nx_, ny_ = nx_ / nl, ny_ / nl
    conv = (A['vx'] - B['vx']) * nx_ + (A['vy'] - B['vy']) * ny_        # > 0: the plates close in
    c = min(1.0, max(0.0, conv))
    if A['cont'] and B['cont'] and conv > 0.25:
        lines, kind, w, h = chain_lines(pa + pb), 'fold', 1.8 + 0.5 * c, 0.95 + 0.25 * c
    elif A['cont'] != B['cont'] and conv > 0.2:          # the ocean floor dives under the continent: cordillera inland
        cp, sgn = (pa, -1) if A['cont'] else (pb, 1)
        lines = chain_lines([(x + sgn * 2.2 * nx_, y + sgn * 2.2 * ny_) for x, y in cp])
        kind, w, h = 'cordillera', 1.5, 0.9 + 0.25 * c
    elif not A['cont'] and not B['cont'] and conv > 0.35:
        lines = chain_lines([(x - 1.5 * nx_, y - 1.5 * ny_) for x, y in pa])
        kind, w, h = 'arc', 0.9, 0.62 + 0.2 * c
    elif A['cont'] and B['cont'] and conv < -0.3:
        RIFTS += chain_lines(pa + pb, 8.0)
        continue
    else:
        continue
    for pts in lines:
        pts = [(x, y) for x, y in pts if MARGIN_W + 2 <= x <= W - MARGIN_E - 1] if kind == 'arc' else pts
        if len(pts) >= 2 and poly_len(pts) >= 5:
            RANGES.append(dict(pts=pts, w=w, h=h, passes=with_passes(poly_len(pts)), kind=kind))
for pts in RIFTS:
    VALLEYS.append((pts, 0.3, 2.2))
    mx, my = pts[len(pts) // 2]
    vert = abs(pts[-1][1] - pts[0][1]) > abs(pts[-1][0] - pts[0][0])
    LAKES.append((mx, my, 0.9 if vert else 2.4, 2.4 if vert else 0.9))


def ridge_of(x, y):
    best = 0.0
    wx = x + 1.1 * NZ[3].fbm(x, y, 3, 6)
    wy = y + 1.1 * NZ[4].fbm(x, y, 3, 6)
    for k, R in enumerate(RANGES):
        d, s = poly_dist(wx, wy, R['pts'])
        if d > R['w'] * 3:
            continue
        prof = math.exp(-(d / R['w']) ** 2)
        var = 0.78 + 0.45 * NZ[5].fbm(s * 0.8, k * 31.0, 2, 3)
        pf = 1.0
        for ps in R['passes']:
            pf *= 1 - 0.6 * math.exp(-((s - ps) / 1.5) ** 2)
        spur = math.exp(-(d / (2.6 * R['w'])) ** 2) * max(0.0, NZ[18].ridged(x, y, 3, 4) - 0.55) * 1.5
        best = max(best, R['h'] * pf * max(prof * var, spur))
    return best


# height of the crust: continental plates stand high, the ocean floor low; noise gives plains, uplands and
# shelves, the ranges lift the land (and island arcs out of the sea), rifts sink in
BASE, HEIGHT0 = [0.0] * (W * H), [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    b = 0.55 * crust[i] + 0.32 * NZ[2].fbm(x, y, 4, 12) + 0.10 * NZ[7].fbm(x, y, 3, 4)
    for pts, depth, wd in VALLEYS:
        d, _ = poly_dist(x, y, pts)
        b -= depth * math.exp(-(d / wd) ** 2)
    if x > W - MARGIN_E - 3:
        b -= (x - (W - MARGIN_E - 3)) * 0.25
    if x < MARGIN_W + 2:
        b -= (MARGIN_W + 2 - x) * 0.4
    BASE[i] = b
    HEIGHT0[i] = b + 0.6 * ridge_of(x, y)
SEA_LEVEL = sorted(HEIGHT0)[int((1 - LAND_SHARE) * W * H)]
land = [HEIGHT0[i] > SEA_LEVEL for i in range(W * H)]


def components(mask, d=N8):
    seen = [False] * (W * H)
    comps = []
    for x, y in ALL:
        i = I(x, y)
        if mask[i] and not seen[i]:
            q, comp = deque([(x, y)]), []
            seen[i] = True
            while q:
                cx, cy = q.popleft()
                comp.append((cx, cy))
                for nx, ny in nbrs(cx, cy, d):
                    j = I(nx, ny)
                    if mask[j] and not seen[j]:
                        seen[j] = True
                        q.append((nx, ny))
            comps.append(comp)
    return comps


# drop single-tile specks, fill tiny enclosed water holes (lakes are placed later)
for comp in components(land):
    if len(comp) < 2:
        for x, y in comp:
            land[I(x, y)] = False
water_comps = components([not v for v in land], N4)
for comp in water_comps:
    if len(comp) < 4 and not any(x in (0, W - 1) or y in (0, H - 1) for x, y in comp):
        for x, y in comp:
            land[I(x, y)] = True


def free_water(x, y, r):
    return all(inb(x + dx, y + dy) and not land[I(x + dx, y + dy)] for dx in range(-r, r + 1) for dy in range(-r, r + 1))


# the island sea: the warm sea most enclosed by land (like the Caribbean) gets the island world
ISLE_R = 10
best = None
for x, y in ALL:
    if not (-12 <= lat(y) <= 30) or not (MARGIN_W + 8 <= x <= W - MARGIN_E - 2) or not free_water(x, y, 2):
        continue
    enc = sum(1 for dx in range(-ISLE_R, ISLE_R + 1, 2) for dy in range(-ISLE_R, ISLE_R + 1, 2)
              if dx * dx + dy * dy <= ISLE_R * ISLE_R and inb(x + dx, y + dy) and land[I(x + dx, y + dy)])
    sc = enc - 0.15 * abs(lat(y) - 15) + tect.random()
    if best is None or sc > best[0]:
        best = (sc, x, y)
ISLE_SEA = (best[1], best[2]) if best else (W // 2, lat_y(15))


def in_isle_sea(x, y, r=ISLE_R):
    return math.hypot(x - ISLE_SEA[0], y - ISLE_SEA[1]) <= r


# ---------------------------------------------------------------- coastline details
crng = random.Random(SEED * 7 + 11)


# fjords: narrow glacial inlets cut into the cold west coasts of the north
fjords = []


def carve_fjord(cx, cy, ang, length, branch):
    fx, fy = float(cx), float(cy)
    for n in range(length):
        ang += crng.uniform(-0.45, 0.45)
        ang = max(-0.9, min(0.9, ang))
        nx, ny = int(round(fx + math.cos(ang))), int(round(fy + math.sin(ang)))
        if not (2 <= nx < W - MARGIN_E and 2 <= ny < H - 2):
            break
        if nx != cx and ny != cy:          # keep the channel 4-connected
            land[I(nx, cy)] = False
        land[I(nx, ny)] = False
        fx, fy, cx, cy = fx + math.cos(ang), fy + math.sin(ang), nx, ny
        if branch and n == length // 2:
            carve_fjord(cx, cy, ang + crng.choice((-1.1, 1.1)), crng.randint(2, 4), False)


y0 = H - 4
while lat(y0) > 46:
    row = [x for x in range(2, W // 2) if land[I(x, y0)] and not land[I(x - 1, y0)]]
    if row:
        land[I(row[0], y0)] = False
        carve_fjord(row[0], y0, crng.uniform(-0.5, 0.5), crng.randint(4, 9), crng.random() < 0.45)
        fjords.append((row[0], y0))
    y0 -= crng.randint(3, 4)

# skerries: small rocky islets off the fjord coast
skerries = []
for x, y in ALL:
    if x < W // 2 and lat(y) > 49 and any(math.hypot(x - fx, y - fy) < 8 for fx, fy in fjords) and not land[I(x, y)] and free_water(x, y, 1) and not free_water(x, y, 2):
        if crng.random() < 0.10:
            skerries.append((x, y))
for x, y in skerries:
    land[I(x, y)] = True

# island world in the island sea: two loose island arcs of settleable islands (~10 tiles) across it;
# every island keeps at least two water tiles to any other land
_a = crng.uniform(0, math.pi)
ISLE_ARCS = [[(ISLE_SEA[0] + t * math.cos(_a) - o * math.sin(_a), ISLE_SEA[1] + t * math.sin(_a) + o * math.cos(_a)) for t in (-9, -3, 3, 9)]
             for o in (-2.5, 2.5)]
archipelago = []
isle_cells = set()


def isle_free(x, y, own):                     # at least 2 water tiles to any other land
    for dx in range(-2, 3):
        for dy in range(-2, 3):
            p = (x + dx, y + dy)
            if inb(*p) and land[I(*p)] and p not in own:
                return False
    return True


def grow_island(cx, cy, size, min_size=1):
    cells, tries = [(cx, cy)], 0
    while len(cells) < size and tries < 250:
        tries += 1
        x, y = crng.choice(cells)
        dx, dy = crng.choice(N4 if crng.random() < 0.8 else N8)
        nx, ny = x + dx, y + dy
        if (nx, ny) in cells or not (2 <= nx < W - MARGIN_E and 2 <= ny < H - 2) or not isle_free(nx, ny, set(cells)):
            continue
        cells.append((nx, ny))
    if len(cells) < min_size:
        return []
    for x, y in cells:
        land[I(x, y)] = True
        isle_cells.add((x, y))
    return cells


# volcanic island: a cone in the middle of a ~12 tile island
volcano_island, VOLCANO_ISLE = None, []
for x, y in sorted([(x, y) for x in range(ISLE_SEA[0] - 9, ISLE_SEA[0] + 10) for y in range(ISLE_SEA[1] - 8, ISLE_SEA[1] + 9)
                    if free_water(x, y, 3)], key=lambda p: math.hypot(p[0] - ISLE_SEA[0] - 2, p[1] - ISLE_SEA[1])):
    VOLCANO_ISLE = grow_island(x, y, 12, 9)
    if VOLCANO_ISLE:
        volcano_island = (x, y)
        break

centres = []
for arc in ISLE_ARCS:
    for (ax, ay), (bx, by) in zip(arc, arc[1:]):
        n = max(1, int(math.hypot(bx - ax, by - ay) / 4.5))
        for k in range(n):
            t = (k + crng.random() * 0.4) / n
            centres.append((int(round(ax + (bx - ax) * t + crng.uniform(-1, 1))), int(round(ay + (by - ay) * t + crng.uniform(-1, 1)))))
for _ in range(60):
    centres.append((ISLE_SEA[0] + crng.randint(-ISLE_R, ISLE_R), ISLE_SEA[1] + crng.randint(-ISLE_R, ISLE_R)))
for cx, cy in centres:
    if len(archipelago) >= 6:
        break
    if inb(cx, cy) and not land[I(cx, cy)] and isle_free(cx, cy, set()):
        isle = grow_island(cx, cy, crng.choice([8, 9, 10, 10, 11, 12, 14]), 7)
        if isle:
            archipelago.append(isle)

# random islands off the other coasts; terrain follows the local climate like on the mainland
SEA_ISLES = []
cands = [(x, y) for x in range(MARGIN_W + 6, W - MARGIN_E - 1) for y in range(3, H - 3)      # the Pacific route and Europe stay open
         if not in_isle_sea(x, y, ISLE_R + 4) and free_water(x, y, 3) and not free_water(x, y, 8)]
crng.shuffle(cands)
for x, y in cands:
    if len(SEA_ISLES) >= 7:
        break
    if any(math.hypot(x - c[0][0], y - c[0][1]) < 11 for c in SEA_ISLES) or not free_water(x, y, 3):
        continue
    isle = grow_island(x, y, crng.choice([5, 6, 8, 9, 10, 12]), 5)
    if isle:
        SEA_ISLES.append(isle)

# barrier islands: a chain of narrow sand islands in front of a straight, warm-temperate east coast,
# enclosing a shallow lagoon; inlets between the islands
LAGOON, BARRIER = set(), set()
best = None
for y0 in range(3, H - 16):
    for ln in (13, 11, 9):
        rows = list(range(y0, y0 + ln))
        if not all(20 <= lat(y) <= 48 for y in rows):
            continue
        cx = []
        for y in rows:
            xs = [x for x in range(W // 2, W - MARGIN_E) if land[I(x, y)]]
            cx.append(max(xs) if xs else -1)
        if min(cx) < W // 2 + 6 or max(cx) - min(cx) > 3 or any(abs(a - b) > 1 for a, b in zip(cx, cx[1:])):
            continue
        if any(in_isle_sea(c, y, ISLE_R + 3) for c, y in zip(cx, rows)):
            continue
        if not all(all(not land[I(c + d, y)] for d in range(1, 7)) for c, y in zip(cx, rows)):
            continue
        sc = ln - (max(cx) - min(cx))
        if best is None or sc > best[0]:
            best = (sc, rows, cx)
        break
if best:
    _, rows, cx = best
    gap = crng.randint(3, 5)
    for n, (y, c) in enumerate(zip(rows, cx)):
        bx_ = max(cx[max(0, n - 1):n + 2]) + 2      # the banks follow the coast two tiles out, the lagoon stays edge-connected
        for x in range(c + 1, bx_ + (1 if n in (0, len(rows) - 1) else 0)):
            LAGOON.add(I(x, y))
        if n in (0, len(rows) - 1):
            continue
        if n == gap:                                   # inlet
            gap += crng.randint(4, 6)
            LAGOON.add(I(bx_, y))
            continue
        BARRIER.add(I(bx_, y))
        land[I(bx_, y)] = True
    LAGOON -= BARRIER

# coral atolls in the gaps of the warm inner sea
atolls = []
cands = [(x, y) for x, y in ALL if in_isle_sea(x, y, ISLE_R + 3) and free_water(x, y, 2)]
crng.shuffle(cands)
for x, y in cands:
    if len(atolls) >= 4:
        break
    if all(max(abs(x - ax), abs(y - ay)) >= 5 for ax, ay in atolls):
        atolls.append((x, y))
        land[I(x, y)] = True

def bfs_dist(sources, passable):
    dist = [10 ** 6] * (W * H)
    q = deque()
    for i in sources:
        dist[i] = 0
        q.append(i)
    while q:
        i = q.popleft()
        x, y = i % W, i // W
        for nx, ny in nbrs(x, y):
            j = I(nx, ny)
            if passable(j) and dist[j] > dist[i] + 1:
                dist[j] = dist[i] + 1
                q.append(j)
    return dist


dcoast = bfs_dist([i for i in range(W * H) if not land[i]], lambda j: land[j])

# ---------------------------------------------------------------- relief
mrng0 = random.Random(SEED * 7 + 2)
# hidden high valley: a fertile basin ringed by mountains with a single pass
hidden_valley = None
for min_dr, min_dc in ((8, 7), (6, 6), (5, 5), (4, 5), (3, 4)):
    best = None
    for x, y in ALL:
        i = I(x, y)
        if not land[i] or dcoast[i] < min_dc or not (22 <= abs(lat(y)) <= 55):
            continue
        dr = min([poly_dist(x, y, R['pts'])[0] for R in RANGES] + [99])
        if dr < min_dr:
            continue
        if any(math.hypot(x - lx, y - ly) < 9 for lx, ly, _, _ in LAKES) or any(poly_dist(x, y, v[0])[0] < 6 for v in VALLEYS):
            continue
        if any(not inb(x + dx, y + dy) or not land[I(x + dx, y + dy)]
               for dx in range(-6, 7) for dy in range(-6, 7) if dx * dx + dy * dy <= 36):
            continue
        sc = dcoast[i] + 0.3 * dr
        if best is None or sc > best[0]:
            best = (sc, x, y)
    if best:
        hidden_valley = (best[1], best[2])
        break
if hidden_valley:
    hx, hy = hidden_valley
    ring = []
    for a in range(25):
        ang = a * math.pi / 12
        rr = 1 + 0.2 * NZ[22].fbm(math.cos(ang) * 3 + 50, math.sin(ang) * 3 + 50, 2, 1.5)
        ring.append((hx + 5.0 * rr * math.cos(ang), hy + 4.2 * rr * math.sin(ang)))
    ring[-1] = ring[0]
    RANGES.append(dict(pts=ring, w=0.8, h=1.3, passes=[poly_len(ring) * 0.62, poly_len(ring) * 0.125]))
    VALLEYS.append(([(hx - 0.5, hy), (hx + 0.5, hy)], 0.35, 3.0))
    LAKES.append((hx, hy, 1.7, 1.2))

# endorheic salt basin in the rain shadow of a range (the westerlies bring the rain from the west): a salt lake
# between mountain arcs, deep inside the land
def range_dist(x, y, kinds=None):
    return min([poly_dist(x, y, R['pts'])[0] for R in RANGES if kinds is None or R.get('kind') in kinds] + [99])


def solid_land(x, y, r):
    return all(inb(x + dx, y + dy) and land[I(x + dx, y + dy)] for dx in range(-r, r + 1) for dy in range(-r, r + 1) if dx * dx + dy * dy <= r * r)


SALT_BASIN, best = None, None
for x, y in ALL:
    i = I(x, y)
    if not land[i] or not 25 <= abs(lat(y)) <= 48 or dcoast[i] < 4 or not solid_land(x, y, 5) or range_dist(x, y) < 5.5:
        continue
    if hidden_valley and math.hypot(x - hidden_valley[0], y - hidden_valley[1]) < 11:
        continue
    lee = any(poly_dist(x - d, y, R['pts'])[0] < 2.5 for R in RANGES if R['h'] >= 0.85 for d in range(5, 14, 2))
    if not lee:
        continue
    sc = dcoast[i] + 3 * mrng0.random()
    if best is None or sc > best[0]:
        best = (sc, x + 0.5, y + 0.5)
if best:
    SALT_BASIN = (best[1], best[2])
    LAKES.append((SALT_BASIN[0], SALT_BASIN[1], 1.9, 1.3))
# altiplano: a wide, barren high plateau with a large mountain lake inside the highest tropical range
ALTIPLANO, best = None, None
ALTI_R = (3.3, 4.8)
for R in RANGES:
    if R.get('kind') not in ('cordillera', 'fold'):
        continue
    for x, y in ALL:
        i = I(x, y)
        if not land[i] or abs(lat(y)) > 25 or dcoast[i] < 4 or poly_dist(x, y, R['pts'])[0] > 1.0 or not solid_land(x, y, 5):
            continue
        if (hidden_valley and math.hypot(x - hidden_valley[0], y - hidden_valley[1]) < 10) or \
                (SALT_BASIN and math.hypot(x - SALT_BASIN[0], y - SALT_BASIN[1]) < 12):
            continue
        sc = R['h'] + 0.05 * dcoast[i] + 0.2 * mrng0.random()
        if best is None or sc > best[0]:
            best = (sc, x + 0.5, y + 0.5)
if best:
    ALTIPLANO = (best[1], best[2])
    LAKES.append((ALTIPLANO[0], ALTIPLANO[1], 1.5, 2.0))


def in_alti(x, y, f=1.0):
    return ALTIPLANO and ((x - ALTIPLANO[0]) / ALTI_R[0]) ** 2 + ((y - ALTIPLANO[1]) / ALTI_R[1]) ** 2 < f


def in_basin(x, y, r=3.6):
    return SALT_BASIN and math.hypot(x - SALT_BASIN[0], (y - SALT_BASIN[1]) * 0.95) < r

# small inland massifs to break up large plains
mrng = random.Random(SEED * 7 + 3)
cands = [(x, y) for x, y in ALL if land[I(x, y)] and dcoast[I(x, y)] >= 4
         and range_dist(x, y) > 6 and not in_alti(x, y, 2.5)]
mrng.shuffle(cands)
minis = []
for x, y in cands:
    if len(minis) >= 10:
        break
    if any(math.hypot(x - mx, y - my) < 9 for mx, my in minis) or in_basin(x, y, 8):
        continue
    a = mrng.random() * math.pi
    ln = mrng.uniform(1.5, 3.5)
    bend = mrng.uniform(-1.2, 1.2)
    pts = [(x - ln * math.cos(a), y - ln * math.sin(a)), (x + bend * math.sin(a), y - bend * math.cos(a)),
           (x + ln * math.cos(a), y + ln * math.sin(a))]
    RANGES.append(dict(pts=pts, w=0.85, h=mrng.uniform(0.85, 1.0), passes=[]))
    minis.append((x, y))
if SALT_BASIN:                                 # mountain arcs closing the salt basin to the north and south
    sx_, sy_ = SALT_BASIN
    for a0, a1, ps in ((0.15, 0.85, 0.6), (1.15, 1.85, 0.35)):     # northern and southern arc (in units of pi)
        arc = [(sx_ + 4.4 * math.cos(math.pi * (a0 + (a1 - a0) * k / 8.0)), sy_ + 4.6 * math.sin(math.pi * (a0 + (a1 - a0) * k / 8.0)))
               for k in range(9)]
        RANGES.append(dict(pts=arc, w=0.8, h=0.95, passes=[poly_len(arc) * ps]))
# old, worn-down chains (the remains of collisions long past) break up the wide plains far from the young ranges
EAST_RANGES = []
cands = [(x, y) for x, y in ALL if land[I(x, y)] and dcoast[I(x, y)] >= 3 and range_dist(x, y) > 7 and not in_alti(x, y, 3) and not in_basin(x, y, 8)]
mrng.shuffle(cands)
for x, y in cands:
    if len(EAST_RANGES) >= 4:
        break
    if any(math.hypot(x - R['pts'][1][0], y - R['pts'][1][1]) < 13 for R in EAST_RANGES) or range_dist(x, y) <= 7:
        continue
    a, ln = mrng.random() * math.pi, mrng.uniform(3.5, 7.0)
    pts = [(x + t * ln * math.cos(a) + mrng.uniform(-1, 1) * math.sin(a), y + t * ln * math.sin(a) - mrng.uniform(-1, 1) * math.cos(a))
           for t in (-1, -0.33, 0.33, 1)]
    if not all(inb(int(px), int(py)) and land[I(int(px), int(py))] for px, py in pts):
        continue
    EAST_RANGES.append(dict(pts=pts, w=1.1, h=mrng.uniform(0.66, 0.86), passes=[poly_len(pts) * 0.6] if ln > 5 else [], kind='old'))
RANGES += EAST_RANGES
# a caldera: a ring of volcanic mountains around a crater lake, on the flank of a cordillera or in an island arc
CALDERA = None
cands = [(x, y) for x, y in ALL if land[I(x, y)] and dcoast[I(x, y)] >= 3 and 3 <= range_dist(x, y, ('cordillera', 'arc')) <= 7
         and range_dist(x, y, ('fold', 'old')) > 6 and solid_land(x, y, 4) and not in_alti(x, y, 3) and not in_basin(x, y, 9)
         and not (hidden_valley and math.hypot(x - hidden_valley[0], y - hidden_valley[1]) < 10)]
if cands:
    cx_, cy_ = mrng.choice(cands)
    CALDERA = (cx_ + 0.5, cy_ + 0.5)
    ring = [(CALDERA[0] + 3.0 * math.cos(a * math.pi / 6), CALDERA[1] + 3.0 * math.sin(a * math.pi / 6)) for a in range(13)]
    RANGES.append(dict(pts=ring, w=0.8, h=0.82, passes=[], kind='caldera'))
    LAKES.append((CALDERA[0], CALDERA[1], 1.5, 1.2))

# the far north breaks up into an arctic archipelago; a winding strait (like Magellan's) runs between the
# mainland and the large islands - an ice-free route from the Atlantic to the Pacific
_sy = lat_y(61)
STRAIT_PTS = [(x, _sy + mrng.uniform(-1.5, 1.5)) for x in range(0, W - 8, 6)] + [(W - 8, _sy)]


def strait_y(x):
    for (ax, ay), (bx, by) in zip(STRAIT_PTS, STRAIT_PTS[1:]):
        if ax <= x <= bx:
            return ay + (by - ay) * (x - ax) / float(bx - ax)
    return STRAIT_PTS[-1][1]


PASSAGE = set()
for wd in (0.8, 1.1, 1.5):
    for x, y in ALL:
        i = I(x, y)
        if not land[i] or y < _sy - 6:
            continue
        d, _ = poly_dist(x + 0.9 * NZ[3].fbm(x, y, 2, 4), y + 0.9 * NZ[4].fbm(x, y, 2, 4), STRAIT_PTS)
        if d < wd + 0.7 * max(0.0, NZ[2].fbm(x * 0.6, 7.0, 2, 3)):
            land[i] = False
            PASSAGE.add(i)
        elif y > strait_y(x) and NZ[23].fbm(x * 1.1 + 100, y * 1.1, 3, 5) + 0.04 * (y - strait_y(x)) > -0.02:
            land[i] = False                      # open arctic sea between the islands
    water = [not land[i] for i in range(W * H)]
    west = next((c for c in components(water, N4) if any(p[0] == 0 and abs(p[1] - strait_y(0)) <= 3 for p in c)), [])
    if any(p[0] >= W - 10 and -2 <= p[1] - strait_y(W - 10) <= 5 for p in west):
        break
for comp in components(land):                    # no arctic specks
    if len(comp) < 4 and all(y > strait_y(x) for x, y in comp):
        for x, y in comp:
            land[I(x, y)] = False
dcoast = bfs_dist([i for i in range(W * H) if not land[i]], lambda j: land[j])

ridge = [0.0] * (W * H)
for x, y in ALL:
    if not land[I(x, y)]:
        continue
    ridge[I(x, y)] = ridge_of(x, y)

PEAK_T, HILL_T = 0.56, 0.3
plot_type = [3] * (W * H)  # 0 peak, 1 hills, 2 flat, 3 water
hrng = random.Random(SEED * 7 + 5)
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    r = ridge[i]
    if r > PEAK_T and dcoast[i] > 0:
        plot_type[i] = 0
    elif r > HILL_T and hrng.random() < 0.45:
        plot_type[i] = 1
    elif r > 0.14 and hrng.random() < 0.12:
        plot_type[i] = 1
    else:
        plot_type[i] = 2


def hill_nbrs(x, y):
    return sum(1 for nx, ny in nbrs(x, y) if plot_type[I(nx, ny)] == 1)


order_xy = ALL[:]
hrng.shuffle(order_xy)
for x, y in order_xy:          # scattered single hills and small pairs
    i = I(x, y)
    if plot_type[i] == 2 and hill_nbrs(x, y) <= 1:
        p = 0.035 + (0.02 if lat(y) > 50 else 0) + 0.03 * max(0.0, NZ[6].fbm(x, y, 3, 6))
        if hrng.random() < p:
            plot_type[i] = 1
for x, y in order_xy:          # break up clumps
    i = I(x, y)
    if plot_type[i] == 1 and hill_nbrs(x, y) >= 4 and hrng.random() < 0.6:
        plot_type[i] = 2
for x, y in atolls:
    plot_type[I(x, y)] = 2
if volcano_island:
    vx, vy = volcano_island
    for x, y in VOLCANO_ISLE:
        plot_type[I(x, y)] = 1 if max(abs(x - vx), abs(y - vy)) == 1 and (x + y) % 2 else 2
    plot_type[I(vx, vy)] = 0

# irregular mountain flanks: clustered extra peaks so chains are not uniform 1-tile lines
pass_pts = []
for R in RANGES:
    for ps in R['passes']:
        run = 0.0
        for (ax, ay), (bx, by) in zip(R['pts'], R['pts'][1:]):
            seg = math.hypot(bx - ax, by - ay)
            if seg > 0 and run + seg >= ps:
                t = (ps - run) / seg
                pass_pts.append((ax + (bx - ax) * t, ay + (by - ay) * t))
                break
            run += seg
brng = random.Random(SEED * 7 + 17)
bulges = []
for x, y in ALL:
    i = I(x, y)
    if plot_type[i] not in (1, 2) or dcoast[i] < 2:
        continue
    pk = sum(1 for nx, ny in nbrs(x, y) if plot_type[I(nx, ny)] == 0)
    if pk == 0 or any(math.hypot(x - px, y - py) < 2.5 for px, py in pass_pts):
        continue
    if hidden_valley and ((x - hidden_valley[0]) / 4.0) ** 2 + ((y - hidden_valley[1]) / 3.3) ** 2 < 1.0:
        continue
    b = NZ[21].fbm(x * 1.3 + 7, y * 1.3 - 3, 2, 2.5)
    p = 0.45 if b > 0.2 else (0.1 if pk >= 2 and b > 0.0 else 0.0)
    if brng.random() < p:
        bulges.append(i)
for i in bulges:
    plot_type[i] = 0
# open plateau and basin floors: no peaks inside, only a few scattered hills
for x, y in order_xy:
    i = I(x, y)
    if not land[i] or not (in_alti(x, y, 0.8) or in_basin(x, y, 3.0)):
        continue
    plot_type[i] = 1 if hill_nbrs(x, y) == 0 and hrng.random() < (0.2 if in_alti(x, y) else 0.08) else 2

elev = [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    e = 0.06 + 0.025 * min(dcoast[i], 10) + 0.3 * max(0.0, min(0.6, BASE[i] - SEA_LEVEL)) + 0.9 * ridge[i] + 0.06 * NZ[7].fbm(x, y, 3, 8)
    if plot_type[i] == 1:
        e += 0.08
    for pts, depth, wd in VALLEYS:
        d, sv = poly_dist(x, y, pts)
        e -= depth * (0.3 + 0.7 * sv / poly_len(pts)) * math.exp(-(d / wd) ** 2)
    elev[i] = max(0.02, e)

# ---------------------------------------------------------------- lakes
lake = [False] * (W * H)
for cx, cy, rx, ry in LAKES:
    for x, y in ALL:
        dx, dy = (x - cx) / rx, (y - cy) / ry
        if dx * dx + dy * dy + 0.25 * NZ[8].fbm(x, y, 2, 3) < 1.0 and land[I(x, y)]:
            lake[I(x, y)] = True
# glacial lake district
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] == 2 and 47 < lat(y) < 64 and dcoast[i] > 2:
        if NZ[9].fbm(x, y, 3, 3) > 0.42:
            lake[i] = True
# finger lakes: long narrow parallel lakes left by glaciers between low ridges in the north-east
FINGER = set()
best = None
for cy in range(lat_y(40), min(H - 7, lat_y(58))):
    for cx in range(W // 3, W - MARGIN_E - 2):
        box = [(x, y) for x in range(cx - 5, cx + 6) for y in range(cy - 3, cy + 4)]
        if sum(1 for x, y in box if not inb(x, y) or not land[I(x, y)] or dcoast[I(x, y)] < 2 or lake[I(x, y)]) > 3:
            continue
        sc = sum(dcoast[I(x, y)] for x, y in box) / len(box) - 0.5 * sum(1 for x, y in box if plot_type[I(x, y)] == 0)
        if best is None or sc > best[0]:
            best = (sc, cx, cy)
if best:
    _, fx, fy = best
    frng = random.Random(SEED * 7 + 31)
    for k, ox in enumerate((-4, -1, 2, 5)):
        if ox == 5 and frng.random() < 0.5:
            continue
        ln = frng.randint(3, 6)
        y_start = fy - ln // 2 + frng.randint(-1, 1)
        x = fx + ox
        for n in range(ln):
            y = y_start + n
            if n == ln // 2 and frng.random() < 0.5:     # a slight kink
                x += frng.choice((-1, 1)) if abs(x - (fx + ox)) == 0 else 0
            i = I(x, y)
            if land[i] and dcoast[i] >= 2:
                lake[i] = True
                FINGER.add(i)
    for x, y in [(x, y) for x in range(fx - 6, fx + 8) for y in range(fy - 5, fy + 6)]:   # low wooded ridges between the lakes
        i = I(x, y)
        if land[i] and not lake[i] and any(lake[I(nx, ny)] for nx, ny in nbrs(x, y)):     # glaciers ground the ridges down
            plot_type[i] = 1 if hill_nbrs(x, y) == 0 and frng.random() < 0.3 else 2

# oxbow and floodplain lakes in the equatorial lowlands
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] == 2 and abs(lat(y)) < 13 and dcoast[i] > 2 and ridge[i] < 0.1:
        if NZ[23].fbm(x, y, 3, 2.5) > 0.38:
            lake[i] = True
# enclosed water bodies left by the landmass noise become lakes too
for comp in components([not land[i] for i in range(W * H)], N8):
    if len(comp) <= 45 and not any(x in (0, W - 1) or y in (0, H - 1) for x, y in comp):
        for x, y in comp:
            lake[I(x, y)] = True
for i in range(W * H):
    if lake[i]:
        land[i] = False
        plot_type[i] = 3
# no lake may touch the ocean (would merge) and none may exceed 45 plots
ocean = [not land[i] and not lake[i] for i in range(W * H)]
for x, y in ALL:
    i = I(x, y)
    if lake[i] and any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y)):
        lake[i] = False
        land[i] = True
        plot_type[i] = 2
for comp in components(lake):
    if len(comp) > 45:
        for x, y in comp[45:]:
            i = I(x, y)
            lake[i], land[i], plot_type[i] = False, True, 2

FINGER = set(i for i in FINGER if lake[i])
FINGER_SHORE = set(I(nx, ny) for i in FINGER for nx, ny in nbrs(i % W, i // W) if land[I(nx, ny)])
ocean = [not land[i] and not lake[i] for i in range(W * H)]
dcoast = bfs_dist([i for i in range(W * H) if ocean[i]], lambda j: not ocean[j])

# ---------------------------------------------------------------- climate
T = [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    t = 1 - (abs(lat(y)) / 80.0) ** 1.3
    t -= 0.22 * ridge[i] + 0.12 * max(0.0, min(0.6, BASE[i] - SEA_LEVEL)) + (0.03 if plot_type[i] == 1 else 0)     # ranges and high plateaus are colder
    t += 0.03 * NZ[10].fbm(x, y, 3, 10)
    T[i] = t

RAIN_LAT = [(0, 1.0), (7, 0.95), (14, 0.8), (21, 0.56), (27, 0.48), (34, 0.62), (44, 0.85), (55, 0.8), (64, 0.55), (75, 0.45)]


def march(y, xs):
    out = {}
    m, prev = 1.0, None
    for x in xs:
        i = I(x, y)
        if ocean[i]:
            m, prev = min(1.0, m + 0.5), 0.0
            continue
        if lake[i]:
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
    a = abs(lat(y))
    w_west = smoothstep(26, 32, a) * (1 - smoothstep(58, 64, a))
    from_w = march(y, range(W))
    from_e = march(y, range(W - 1, -1, -1))
    for x in range(W):
        i = I(x, y)
        if land[i]:
            fw, fe = from_w.get(x, 0), from_e.get(x, 0)
            P1[i] = w_west * (fw + 0.7 * fe) + (1 - w_west) * (fe + 0.35 * fw)
# soften stripes
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

# moisture reach from any sea, blocked by mountains
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
    x, y = i % W, i // W
    for nx, ny in nbrs(x, y, N4):
        j = I(nx, ny)
        step = 1 + 6 * ridge[j] + (2 if plot_type[j] == 0 else 0)
        if c + step < cost[j]:
            cost[j] = c + step
            heapq.heappush(pq, (c + step, j))

P = [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    p1 = min(1.0, P1[i] / 0.14)
    p2 = math.exp(-cost[i] / 14.0)
    near_lake = 0.12 if any(lake[I(nx, ny)] for nx, ny in nbrs(x, y)) else 0
    P[i] = max(0.0, interp(RAIN_LAT, abs(lat(y))) * (0.55 * p1 + 0.6 * p2) + near_lake + 0.06 * NZ[11].fbm(x, y, 3, 12))

# ---------------------------------------------------------------- large rivers (plot drainage)
down = [-1] * (W * H)
order = []
filled = [0.0] * (W * H)
seen = [False] * (W * H)
pq = []
for x, y in ALL:
    i = I(x, y)
    if ocean[i]:
        continue
    for nx, ny in nbrs(x, y, N4):
        if ocean[I(nx, ny)]:
            seen[i] = True
            down[i] = I(nx, ny)
            filled[i] = elev[i]
            heapq.heappush(pq, (elev[i], NZ[21].fbm(x, y, 3, 3), i))
            break
while pq:
    e, _, i = heapq.heappop(pq)
    order.append(i)
    x, y = i % W, i // W
    for nx, ny in nbrs(x, y, N4):
        j = I(nx, ny)
        if ocean[j] or seen[j]:
            continue
        seen[j] = True
        down[j] = i
        filled[j] = max(elev[j], e)
        heapq.heappush(pq, (filled[j], NZ[21].fbm(nx, ny, 3, 3), j))
acc = [0.0] * (W * H)
for i in reversed(order):
    acc[i] += P[i] if land[i] else 0.3
    if down[i] >= 0 and not ocean[down[i]]:
        acc[down[i]] += acc[i]
children = [[] for _ in range(W * H)]
for i in order:
    if down[i] >= 0 and not ocean[down[i]]:
        children[down[i]].append(i)

large = [False] * (W * H)
mouths = sorted([i for i in order if ocean[down[i]] and land[i]], key=lambda i: -acc[i])
LR_COUNT, LR_FRAC, LR_MAXLEN = 5, 0.33, 30
river_paths = []
used = 0
for m in mouths:
    if used >= LR_COUNT or acc[m] < 35:
        break
    if any(large[I(nx, ny)] for nx, ny in nbrs(m % W, m // W)):
        continue
    path, cur, n = [], m, 0
    while cur >= 0 and n < LR_MAXLEN:
        if land[cur]:
            if plot_type[cur] == 0:
                break
            path.append(cur)
            n += 1
        ch = [c for c in children[cur] if acc[c] >= LR_FRAC * acc[m]]
        if not ch:
            break
        cur = max(ch, key=lambda c: acc[c])
    run = best_run = 1
    for a, b, c in zip(path, path[1:], path[2:]):
        run = run + 1 if b - a == c - b else 1
        best_run = max(best_run, run)
    if len(path) >= 5 and best_run <= 10:          # long ruler-straight rivers look artificial
        for i in path:
            large[i] = True
            land[i] = False
            plot_type[i] = 3
        river_paths.append(path)
        used += 1

# ---------------------------------------------------------------- edge rivers (corner drainage)
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
        cacc[k] = sum(P[I(px, py)] for px, py in corner_plots(i, j)) / 4.0
for k in reversed(corder):
    if cdown[k] >= 0:
        cacc[cdown[k]] += cacc[k]

RIVER_T = 3.0


def n4_join(cells):
    """Drowned plots must touch the open water or each other along an edge, not only at a corner."""
    cells = set(cells)
    while True:
        start = [j for j in cells if any(ocean[I(nx, ny)] for nx, ny in nbrs(j % W, j // W, N4))]
        reach, dq = set(start), deque(start)
        while dq:
            j = dq.popleft()
            for nx, ny in nbrs(j % W, j // W, N4):
                q = I(nx, ny)
                if q in cells and q not in reach:
                    reach.add(q)
                    dq.append(q)
        rest = cells - reach
        if not rest:
            return cells
        bridged = False
        for j in sorted(rest):
            x, y = j % W, j // W
            for dx, dy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
                q = (x + dx, y + dy)
                if not inb(*q) or not (I(*q) in reach or ocean[I(*q)]):
                    continue
                for bx, by in ((x + dx, y), (x, y + dy)):
                    k = I(bx, by)
                    if land[k] and plot_type[k] != 0 and not any(lake[I(nx, ny)] or large[I(nx, ny)] for nx, ny in nbrs(bx, by)):
                        cells.add(k)
                        bridged = True
                        break
                if bridged:
                    break
            if bridged:
                break
        if not bridged:
            return cells - rest
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
if volcano_island:
    vx, vy = volcano_island

    def edge_ok(a, b):
        return all(inb(px, py) and land[I(px, py)] for px, py in edge_plots_c(a, b))

    def edge_plots_c(a, b):
        ai, aj, bi, bj = a % CW, a // CW, b % CW, b // CW
        if ai == bi:
            return [(ai - 1, min(aj, bj)), (ai, min(aj, bj))]
        return [(min(ai, bi), aj - 1), (min(ai, bi), aj)]

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
    VOLCANO_RIVER = len(best_path) - 1


def has_river(x, y):
    i = I(x, y)
    if i in wofr or i in nofr:
        return True
    return (x > 0 and I(x - 1, y) in wofr) or (y < H - 1 and I(x, y + 1) in nofr)


# natural harbours: deep, sheltered bays behind a narrow entrance on straight open coasts,
# carved after drainage so they keep clear of river mouths
HARBOUR_WATER, SHALLOW_WATER = set(), set(LAGOON)
hb_rng = random.Random(SEED * 7 + 23)
river_cells = [i for path in river_paths for i in path]

# estuary: the sea has drowned the lower course of an eastern river - a branching deep bay
cchildren = [[] for _ in range(CW * CH)]
for k in range(CW * CH):
    if cdown[k] >= 0:
        cchildren[cdown[k]].append(k)


def edge_plots(a, b):
    ai, aj, bi, bj = a % CW, a // CW, b % CW, b // CW
    if ai == bi:
        return [(ai - 1, min(aj, bj)), (ai, min(aj, bj))]
    return [(min(ai, bi), aj - 1), (min(ai, bi), aj)]


def drown_ok(x, y):
    i = I(x, y)
    return inb(x, y) and land[i] and plot_type[i] != 0 and not any(lake[I(nx, ny)] or large[I(nx, ny)] for nx, ny in nbrs(x, y)) \
        and i not in BARRIER


ESTUARY, ESTUARY_ARM = set(), set()
best = None
for k in range(CW * CH):
    if not outlet[k]:
        continue
    i, j = k % CW, k // CW
    if not (W // 2 <= i <= W - MARGIN_E) or not (20 <= lat(j) <= 56) or in_isle_sea(i, j, ISLE_R + 2):
        continue
    if not any(ocean[I(px, py)] for px, py in corner_plots(i, j)):
        continue
    for q in cchildren[k]:
        if cacc[q] >= RIVER_T and not outlet[q] and (best is None or cacc[q] > best[0]) \
                and all(math.hypot(i - r % W, j - r // W) > 6 for r in river_cells) \
                and all(math.hypot(i - l % W, j - l // W) > 5 for l in LAGOON):
            best = (cacc[q], k, q)
if best:
    _, k, q = best
    main = [(k, q)]
    while len(main) < 9:
        ch = [c for c in cchildren[main[-1][1]] if cacc[c] >= RIVER_T and not outlet[c]]
        if not ch:
            break
        main.append((main[-1][1], max(ch, key=lambda c: cacc[c])))
    n_main = len(main)
    for n, (a, b) in enumerate(main):
        sides = edge_plots(a, b)
        if n >= n_main - 2:                           # the head narrows into the river
            sides = sides[:1] if hb_rng.random() < 0.5 else sides[1:]
        for x, y in sides:
            if drown_ok(x, y):
                ESTUARY.add(I(x, y))
        if n < 3:                                     # wide mouth
            x, y = sides[0]
            dx, dy = (sides[0][0] - sides[-1][0], sides[0][1] - sides[-1][1]) if len(sides) == 2 else (0, 0)
            if (dx, dy) != (0, 0) and drown_ok(x + dx, y + dy) and hb_rng.random() < 0.7:
                ESTUARY.add(I(x + dx, y + dy))
        # side arms along tributaries
        for c in cchildren[b]:
            if 1 <= n < n_main - 1 and c != (main[n + 1][1] if n + 1 < n_main else -1) and cacc[c] >= 0.5 * RIVER_T and not outlet[c]:
                prev, cur = b, c
                for _ in range(hb_rng.randint(2, 4)):
                    x, y = edge_plots(prev, cur)[hb_rng.randint(0, 1)]
                    if drown_ok(x, y):
                        ESTUARY_ARM.add(I(x, y))
                    nxt = [d for d in cchildren[cur] if not outlet[d]]
                    if not nxt:
                        break
                    prev, cur = cur, max(nxt, key=lambda d: cacc[d])
    ESTUARY_ARM -= ESTUARY
    joined = n4_join(ESTUARY | ESTUARY_ARM)
    ESTUARY_ARM = (ESTUARY_ARM | (joined - ESTUARY)) & joined
    ESTUARY &= joined
    for j in ESTUARY | ESTUARY_ARM:
        land[j], ocean[j], plot_type[j] = False, True, 3
    HARBOUR_WATER |= ESTUARY
    SHALLOW_WATER |= ESTUARY_ARM
    # drop drowned bits that ended up cut off from the bay
    bay = set(ESTUARY | ESTUARY_ARM)
    reach, dq = set(), deque(j for j in bay if any(ocean[I(nx, ny)] and I(nx, ny) not in bay for nx, ny in nbrs(j % W, j // W)))
    reach.update(dq)
    while dq:
        j = dq.popleft()
        for nx, ny in nbrs(j % W, j // W):
            q2 = I(nx, ny)
            if q2 in bay and q2 not in reach:
                reach.add(q2)
                dq.append(q2)
    for j in bay - reach:
        land[j], ocean[j], plot_type[j] = True, False, 2
        ESTUARY.discard(j)
        ESTUARY_ARM.discard(j)
        HARBOUR_WATER.discard(j)
        SHALLOW_WATER.discard(j)

harbours = []
cands = []
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] == 0 or not (-25 < lat(y) < 58) or not 1 <= x < W - MARGIN_E - 2 or not 1 <= y < H - 1 \
            or in_isle_sea(x, y, ISLE_R + 2) or any(math.hypot(x - fx, y - fy) < 6 for fx, fy in fjords):
        continue
    sea = [(dx, dy) for dx, dy in N4 if ocean[I(x + dx, y + dy)]]
    if len(sea) != 1:
        continue
    near_sea = sum(1 for dx in range(-2, 3) for dy in range(-2, 3) if inb(x + dx, y + dy) and ocean[I(x + dx, y + dy)])
    if any(I(x + dx, y + dy) in SHALLOW_WATER or I(x + dx, y + dy) in ESTUARY for dx in range(-4, 5) for dy in range(-4, 5) if inb(x + dx, y + dy)):
        continue
    if 7 <= near_sea <= 11:                           # straight coast, no headland or bay yet
        cands.append((x, y, -sea[0][0], -sea[0][1]))
hb_rng.shuffle(cands)
for x, y, nx_, ny_ in cands:
    if len(harbours) >= 2:
        break
    if any(math.hypot(x - hx, y - hy) < 14 for hx, hy, _, _ in harbours):
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
    for cx, cy in water_cells:
        j = I(cx, cy)
        land[j], ocean[j], plot_type[j] = False, True, 3
        HARBOUR_WATER.add(j)
    for hx, hy in ((x - ny_, y + nx_), (x + ny_, y - nx_)):   # rocky headlands guard the entrance
        j = I(hx, hy)
        if land[j] and plot_type[j] == 2 and hb_rng.random() < 0.6:
            plot_type[j] = 1
# small coves: 3-5 water tiles cut into the coast, a natural anchorage for a coastal town
coves = []
for x, y, nx_, ny_ in cands:
    if len(coves) >= 8:
        break
    if any(math.hypot(x - hx, y - hy) < 9 for hx, hy, _, _ in harbours) or any(math.hypot(x - cx, y - cy) < 7 for cx, cy, _, _ in coves):
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
    for n, (cx, cy) in enumerate(water_cells):
        j = I(cx, cy)
        land[j], ocean[j], plot_type[j] = False, True, 3
        if mode < 0.4 or (mode < 0.7 and n >= 2):
            SHALLOW_WATER.add(j)
        else:
            HARBOUR_WATER.add(j)
for i in HARBOUR_WATER | SHALLOW_WATER:
    x, y = i % W, i // W
    wofr.pop(i, None)
    nofr.pop(i, None)
    if x > 0:
        wofr.pop(I(x - 1, y), None)
    if y < H - 1:
        nofr.pop(I(x, y + 1), None)


# ---------------------------------------------------------------- biomes
terrain = [''] * (W * H)
M = [0.0] * (W * H)
for x, y in ALL:
    i = I(x, y)
    if not land[i]:
        continue
    t = T[i] + 0.035 * NZ[12].fbm(x, y, 3, 7)
    m = P[i] + 0.07 * NZ[13].fbm(x, y, 3, 6) + (0.05 if has_river(x, y) else 0)
    m += 0.05 * smoothstep(18, 26, lat(y))     # the northern continent: more room for fertile land
    M[i] = m
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
            b = 'ROCK_STEPPES' if elev[i] > 0.4 else 'SHRUBLAND'
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
    # wet lowlands
    if plot_type[i] == 2 and m > 0.68 and elev[i] < 0.3 and b not in ('SNOW', 'DESERT'):
        lowwet = dcoast[i] <= 2 or has_river(x, y) or any(lake[I(nx, ny)] or large[I(nx, ny)] for nx, ny in nbrs(x, y))
        if lowwet and NZ[15].fbm(x, y, 2, 3) > 0.05:
            b = 'MARSH' if t >= 0.58 else ('WETLAND' if t >= 0.2 else b)
    if plot_type[i] == 2 and t > 0.78 and m > 0.55 and b == 'SAVANNAH' and NZ[15].fbm(x, y, 3, 3) > 0.12:
        b = 'MARSH'
    if plot_type[i] == 0:
        if t < 0.3 and abs(lat(y)) > 52:
            b = 'SNOW'
        elif t < 0.3:
            b = 'TUNDRA'
        elif b in ('MARSH', 'WETLAND'):
            b = 'GRASS' if t < 0.66 else 'SAVANNAH'
    terrain[i] = b

# the driest scrub and rock of the warm north turns back into true desert (the moister north would lose it otherwise)
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] != 0 and lat(y) > 18 and T[i] >= 0.5 and terrain[i] in ('SHRUBLAND', 'ROCK_STEPPES') \
            and M[i] - 0.05 * smoothstep(18, 26, lat(y)) < 0.15 + 0.03 * NZ[13].fbm(x * 1.5, y * 1.5, 2, 3):
        terrain[i] = 'DESERT'

DELTA, VALLEY_GREEN, OASIS_RING = set(), set(), set()
for path in river_paths:
    m = path[0]
    mx, my = m % W, m // W
    for nx, ny in nbrs(mx, my, N4):           # second delta channel
        j = I(nx, ny)
        if land[j] and plot_type[j] == 2 and j not in path and not has_river(nx, ny) \
                and any(ocean[I(ax, ay)] for ax, ay in nbrs(nx, ny, N4)):
            large[j], land[j], plot_type[j] = True, False, 3
            break
    for x, y in ALL:
        i = I(x, y)
        if land[i] and plot_type[i] == 2 and math.hypot(x - mx, y - my) <= 2.6 and T[i] >= 0.2:
            terrain[i] = 'MARSH' if T[i] >= 0.55 else 'WETLAND'
            DELTA.add(i)
for path in river_paths:                       # green ribbon through dry land
    for i in path:
        for nx, ny in nbrs(i % W, i // W):
            j = I(nx, ny)
            if land[j] and plot_type[j] == 2 and j not in DELTA and terrain[j] in ('DESERT', 'ROCK_STEPPES', 'SHRUBLAND'):
                terrain[j] = 'SAVANNAH' if T[j] >= 0.8 else 'PLAINS'
                VALLEY_GREEN.add(j)
orng = random.Random(SEED * 7 + 13)
dry = ('DESERT', 'SHRUBLAND', 'ROCK_STEPPES')
cands = [(x, y) for x, y in ALL if terrain[I(x, y)] == 'DESERT' and plot_type[I(x, y)] == 2 and dcoast[I(x, y)] > 3
         and not has_river(x, y) and all(land[I(nx, ny)] and terrain[I(nx, ny)] in dry for nx, ny in nbrs(x, y))
         and not in_alti(x, y, 3.0) and not in_basin(x, y, 8)]
orng.shuffle(cands)
oases = []
for x, y in cands:
    if len(oases) >= 5:
        break
    if all(math.hypot(x - ox, y - oy) >= 7 for ox, oy in oases):
        oases.append((x, y))
        i = I(x, y)
        lake[i], land[i], plot_type[i], terrain[i] = True, False, 3, ''
        for nx, ny in nbrs(x, y):
            j = I(nx, ny)
            if plot_type[j] == 2:
                terrain[j] = 'SHRUBLAND' if orng.random() < 0.5 else 'PLAINS'
                OASIS_RING.add(j)
for x, y in atolls:
    terrain[I(x, y)] = 'SAVANNAH'


def dry_terrain(i):
    return 'DESERT' if T[i] >= 0.5 else 'ROCK_STEPPES'


def near_lake(x, y):
    return any(lake[I(nx, ny)] for nx, ny in nbrs(x, y))


# salt basin: bare shores, salt flats a little further out, desert and rock up to the arcs
BASIN, ALTI = set(), set()
if SALT_BASIN:
    for x, y in ALL:
        i = I(x, y)
        if land[i] and plot_type[i] != 0 and in_basin(x, y, 4.2):
            n = NZ[14].fbm(x * 1.4, y * 1.4, 2, 3)
            if near_lake(x, y):
                terrain[i] = 'SHRUBLAND' if n > -0.1 else 'ROCK_STEPPES'
            else:
                terrain[i] = dry_terrain(i) if n < 0.15 else 'SHRUBLAND'
            BASIN.add(i)
# altiplano: open puna grass, rock and tundra; reed wetlands on the lake shore
if ALTIPLANO:
    for x, y in ALL:
        i = I(x, y)
        if land[i] and plot_type[i] != 0 and in_alti(x, y):
            n = NZ[14].fbm(x * 1.2 + 40, y * 1.2, 2, 3)
            if near_lake(x, y) and plot_type[i] == 2 and n > 0.1:
                terrain[i] = 'WETLAND'
            elif n > 0.12:
                terrain[i] = 'TUNDRA'
            elif n > -0.12:
                terrain[i] = 'PLAINS'
            else:
                terrain[i] = 'ROCK_STEPPES'
            ALTI.add(i)


# canyon: the driest stretch of a large river, cut between rock walls
CANYON_WALL = set()
best = None
for path in river_paths:
    for k in range(0, max(1, len(path) - 5)):
        seg = path[k:k + 10]
        if len(seg) < 6:
            continue
        ps = [P[I(nx, ny)] for i in seg for nx, ny in nbrs(i % W, i // W) if land[I(nx, ny)]]
        if ps:
            sc = sum(ps) / len(ps)
            if best is None or sc < best[0]:
                best = (sc, seg)
canyon = best[1] if best and best[0] < 0.35 else []
if canyon:
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
            bulge = NZ[21].fbm(x * 1.7, y * 1.7, 2, 2) + 0.25 * (rng.random() - 0.5)
            gap = k % 5 == 2
            if d == 2:
                pt = 0 if not gap and rng.random() < 0.85 else 1
            elif d == 3:
                pt = 0 if not gap and bulge > 0.05 else (1 if rng.random() < 0.55 else None)
            else:
                pt = 0 if not gap and bulge > 0.4 else (1 if rng.random() < 0.25 else None)
            if pt is None:
                continue
            terrain[i] = dry_terrain(i)
            plot_type[i] = pt
            CANYON_WALL.add(i)
            VALLEY_GREEN.discard(i)

# mesas: free-standing rock towers and buttes in the largest desert
MESA, MESA_BASE = set(), set()
deserts = sorted(components([land[i] and terrain[i] == 'DESERT' and plot_type[i] in (1, 2) and i not in CANYON_WALL
                             for i in range(W * H)]), key=len, reverse=True)
mesa_centres = []
for comp in deserts[:3]:
    if len(comp) < 12:
        continue
    pool = [(x, y) for x, y in comp if dcoast[I(x, y)] > 2 and plot_type[I(x, y)] == 2 and not in_basin(x, y, 6)]
    rng.shuffle(pool)
    want = max(2, min(6, len(comp) // 10))
    got = 0
    for x, y in pool:
        if got >= want:
            break
        if any(max(abs(x - mx), abs(y - my)) < 4 for mx, my in mesa_centres):
            continue
        mesa_centres.append((x, y))
        got += 1
        r = rng.random()
        cells = [(x, y)] + ([] if r < 0.3 else rng.sample([(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)], 1 if r < 0.7 else 2))
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

# hidden valley interior: lush and green
HIDDEN = set()
if hidden_valley:
    hx, hy = hidden_valley
    for x, y in ALL:
        i = I(x, y)
        if land[i] and plot_type[i] != 0 and ((x - hx) / 4.0) ** 2 + ((y - hy) / 3.3) ** 2 < 1.0:
            terrain[i] = 'GRASS' if T[i] < 0.62 else 'SAVANNAH'
            HIDDEN.add(i)

def green_terrain(i):
    return 'TAIGA' if T[i] < 0.4 else ('GRASS' if T[i] < 0.7 else 'SAVANNAH')


def window(cx, cy, r):
    return [(x, y) for x in range(cx - r, cx + r + 1) for y in range(cy - r, cy + r + 1) if inb(x, y)]


def clear_edges(i):
    x, y = i % W, i // W
    wofr.pop(i, None)
    nofr.pop(i, None)
    if x > 0:
        wofr.pop(I(x - 1, y), None)
    if y < H - 1:
        nofr.pop(I(x, y + 1), None)



# every set piece must offer a usable city site: a city works the 3x3 ring first, later the 5x5 without corners
RING1_MIN, AREA_MIN = 6, 10
CITY_AREA = [(dx, dy) for dx in range(-2, 3) for dy in range(-2, 3) if (dx, dy) != (0, 0) and not (abs(dx) == 2 and abs(dy) == 2)]


# river island: the large river forks around a lens-shaped island and joins again
canyon_set = set(canyon)
ISLAND, ISLAND_ARM = set(), set()
blocked = DELTA | CANYON_WALL | MESA | HIDDEN | BASIN | ALTI
best = None
for pn, path in enumerate(river_paths):
    for k in range(1, len(path) - 5):
        for span in (5, 6, 7, 8):
            if k + span >= len(path):
                break
            seg = path[k + 1:k + span]
            A, B = path[k], path[k + span]
            if any(p in canyon_set for p in path[k:k + span + 1]):
                continue
            def dseg(x, y):
                return min(max(abs(x - p % W), abs(y - p // W)) for p in seg)
            def dend(x, y):
                return min(max(abs(x - p % W), abs(y - p // W)) for p in (A, B))
            ax_, ay_ = A % W, A // W
            def ok_arm(j):
                x, y = j % W, j // W
                return land[j] and plot_type[j] != 0 and j not in blocked and not any(lake[I(nx, ny)] or ocean[I(nx, ny)] for nx, ny in nbrs(x, y)) \
                    and dseg(x, y) <= 5 and abs(x - ax_) <= 10 and abs(y - ay_) <= 10
            dist, prev = {}, {}
            pq = []
            for nx, ny in nbrs(ax_, ay_, N4):
                j = I(nx, ny)
                if ok_arm(j) and j not in seg:
                    dist[j] = 1.0
                    heapq.heappush(pq, (1.0, j))
            goal = set(I(nx, ny) for nx, ny in nbrs(B % W, B // W, N4))
            found = None
            while pq:
                c, j = heapq.heappop(pq)
                if c > dist[j]:
                    continue
                if j in goal:
                    found = j
                    break
                x, y = j % W, j // W
                for nx, ny in nbrs(x, y, N4):
                    q = I(nx, ny)
                    if not ok_arm(q):
                        continue
                    d, e = dseg(nx, ny), dend(nx, ny)
                    want = min(3.2 + 1.6 * NZ[19].fbm(nx * 1.3, ny * 1.3, 2, 2), 1 + e)   # lens: narrow at both ends
                    step = 1 + 4 * abs(d - want)
                    if c + step < dist.get(q, 1e9):
                        dist[q], prev[q] = c + step, j
                        heapq.heappush(pq, (c + step, q))
            if found is None:
                continue
            arm = []
            j = found
            while j is not None:
                arm.append(j)
                j = prev.get(j)
            arm_set = set(arm)
            # the island is the land enclosed between the two arms
            x0, y0 = A % W, A // W
            free = lambda j: land[j] and j not in arm_set
            comp = None
            for p in seg:
                for nx, ny in nbrs(p % W, p // W, N4):
                    q = I(nx, ny)
                    if not free(q) or (comp and q in comp):
                        continue
                    cc, dq = {q}, deque([q])
                    while dq and len(cc) <= 40:
                        r = dq.popleft()
                        for mx, my in nbrs(r % W, r // W, N4):
                            s = I(mx, my)
                            if free(s) and s not in cc:
                                cc.add(s)
                                dq.append(s)
                    if 9 <= len(cc) <= 40 and (comp is None or len(cc) > len(comp)):
                        comp = cc
            if not comp or any(plot_type[q] == 0 or q in blocked for q in comp):
                continue
            def counts(q):
                x, y = q % W, q // W
                r1 = sum(1 for dx, dy in CITY_AREA if max(abs(dx), abs(dy)) == 1 and I(x + dx, y + dy) in comp)
                return r1, sum(1 for dx, dy in CITY_AREA if I(x + dx, y + dy) in comp)
            r1, tot = max(counts(q) for q in comp)
            if r1 < RING1_MIN or tot < AREA_MIN:
                continue
            sc = tot + sum(M[q] + (0.5 if terrain[q] in ('GRASS', 'PLAINS_FERTILE', 'PLAINS', 'SAVANNAH') else 0) for q in comp) / len(comp) * 6 \
                - 0.8 * len(arm) - sum(1 for q in comp if plot_type[q] == 1)
            if best is None or sc > best[0]:
                best = (sc, pn, k, span, comp, arm)
island_at = None
if best:
    _, pn, k, span, comp, arm = best
    island_at = (pn, k + span // 2)
    for j in arm:
        large[j], land[j], plot_type[j] = True, False, 3
        ISLAND_ARM.add(j)
        VALLEY_GREEN.discard(j)
        OASIS_RING.discard(j)
        clear_edges(j)
    for j in comp:
        plot_type[j] = 2 if plot_type[j] == 2 or rng.random() < 0.6 else 1
        if terrain[j] not in ('GRASS', 'PLAINS_FERTILE', 'PLAINS', 'SAVANNAH', 'TAIGA'):
            terrain[j] = green_terrain(j)
        ISLAND.add(j)
        clear_edges(j)


# gorge: a mountain ridge crossing a large river, which breaks through in a narrow wooded cleft
gorge, GORGE, GORGE_FLOOR = None, set(), set()
best = None
for pn, path in enumerate(river_paths):
    for k in range(4, len(path) - 3):
        c = path[k]
        cx, cy = c % W, c // W
        if any(p in canyon_set for p in path[max(0, k - 5):k + 6]) or any(math.hypot(cx - d % W, cy - d // W) < 3 for d in DELTA):
            continue
        if any(math.hypot(cx - j % W, cy - j // W) < 7 for j in ISLAND):
            continue
        if in_basin(cx, cy, 8) or in_alti(cx, cy, 2.5) or (hidden_valley and math.hypot(cx - hidden_valley[0], cy - hidden_valley[1]) < 7):
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
            best = (sc, pn, k, ax, ay)
if best:
    _, pn, gk, ax, ay = best
    path = river_paths[pn]
    gorge = (pn, gk)
    c = path[gk]
    cx, cy = c % W, c // W
    seg = path[max(0, gk - 5):gk + 6]
    grng = random.Random(SEED * 7 + 29)
    for x, y in window(cx, cy, 8):
        j = I(x, y)
        if not land[j] or j in CANYON_WALL or j in HIDDEN or j in DELTA or j in BASIN or j in ALTI or j in ISLAND:
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
            plot_type[j] = 0 if grng.random() < 0.85 else 1
            GORGE.add(j)
        elif grng.random() < 0.3:
            plot_type[j] = 0 if grng.random() < 0.4 else 1
            GORGE.add(j)
        if j in GORGE and plot_type[j] == 1 and terrain[j] in ('DESERT', 'SNOW'):
            terrain[j] = 'ROCK_STEPPES' if terrain[j] == 'DESERT' else 'TUNDRA'


# clay hills: hills on marsh/wetland (only these, tundra and peat lands yield clay on hills)
clay_rng = random.Random(SEED * 7 + 19)
CLAY_HILLS = []
wet = ('MARSH', 'WETLAND')
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] != 2 or terrain[i] not in wet or large[i]:
        continue
    if sum(1 for nx, ny in nbrs(x, y) if plot_type[I(nx, ny)] == 1) <= 1 and clay_rng.random() < 0.13:
        plot_type[i] = 1
        CLAY_HILLS.append(i)
for x, y in ALL:
    i = I(x, y)
    if land[i] and plot_type[i] == 1 and terrain[i] not in wet and i not in CANYON_WALL and i not in MESA:
        wn = [terrain[I(nx, ny)] for nx, ny in nbrs(x, y) if land[I(nx, ny)] and terrain[I(nx, ny)] in wet]
        if len(wn) >= 3 and clay_rng.random() < 0.6:
            terrain[i] = Counter(wn).most_common(1)[0][0]
            CLAY_HILLS.append(i)

# scattered clay hills elsewhere: every settled region keeps a clay source within reach
CLAY_R = 7
def is_clay(i):
    return land[i] and plot_type[i] == 1 and terrain[i] in ('MARSH', 'WETLAND', 'TUNDRA')
cdist = bfs_dist([i for i in range(W * H) if is_clay(i)], lambda j: land[j])
taken = DELTA | VALLEY_GREEN | OASIS_RING | CANYON_WALL | MESA | MESA_BASE | HIDDEN | BASIN | GORGE | GORGE_FLOOR | ISLAND
cands = []
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] == 0 or i in taken or (x, y) in atolls or terrain[i] in ('MARSH', 'WETLAND', 'TUNDRA'):
        continue
    wet_near = has_river(x, y) or any(lake[I(nx, ny)] or large[I(nx, ny)] for nx, ny in nbrs(x, y))
    if terrain[i] in ('DESERT', 'ROCK_STEPPES', 'SNOW') and not wet_near:
        continue
    cands.append((-(M[i] + (0.5 if wet_near else 0) + 0.2 * clay_rng.random()), i))
cands.sort()
CLAY_SCATTER = []


def place_clay_hill(i):
    plot_type[i] = 1
    terrain[i] = 'MARSH' if T[i] >= 0.58 else 'WETLAND'
    CLAY_SCATTER.append(i)
    q = deque([i])
    cdist[i] = 0
    while q:
        c = q.popleft()
        if cdist[c] >= CLAY_R:
            continue
        for nx, ny in nbrs(c % W, c // W):
            j = I(nx, ny)
            if land[j] and cdist[j] > cdist[c] + 1:
                cdist[j] = cdist[c] + 1
                q.append(j)


for _, i in cands:
    if cdist[i] > CLAY_R:
        place_clay_hill(i)
# denser along rivers in the temperate grass- and prairie lands
CLAY_R_RIVER = 3
for _, i in cands:
    x, y = i % W, i // W
    if cdist[i] > CLAY_R_RIVER and terrain[i] in ('GRASS', 'PLAINS', 'PLAINS_FERTILE') and has_river(x, y) \
            and sum(1 for nx, ny in nbrs(x, y) if plot_type[I(nx, ny)] == 1) <= 1:
        place_clay_hill(i)
CLAY_HILLS += CLAY_SCATTER

# every set piece must offer a usable city site (see RING1_MIN, AREA_MIN)


def workable(j):
    return land[j] and plot_type[j] != 0


def site_counts(x, y):
    r1 = tot = 0
    for dx, dy in CITY_AREA:
        if inb(x + dx, y + dy) and workable(I(x + dx, y + dy)):
            tot += 1
            r1 += max(abs(dx), abs(dy)) == 1
    return r1, tot


def ensure_city_site(name, cells):
    cands = [i for i in cells if workable(i)]
    if not cands:
        return
    def cost(i):
        x, y = i % W, i // W
        r1, tot = site_counts(x, y)
        land_r1 = sum(1 for dx, dy in CITY_AREA if max(abs(dx), abs(dy)) == 1 and inb(x + dx, y + dy) and land[I(x + dx, y + dy)])
        land_tot = sum(1 for dx, dy in CITY_AREA if inb(x + dx, y + dy) and land[I(x + dx, y + dy)])
        feasible = land_r1 >= RING1_MIN and land_tot >= AREA_MIN
        need1 = max(0, RING1_MIN - r1)
        need = max(need1, AREA_MIN - tot)
        return (not feasible, need, -tot)
    best = min(cands, key=cost)
    x, y = best % W, best // W
    opened = 0
    while True:
        r1, tot = site_counts(x, y)
        if r1 >= RING1_MIN and tot >= AREA_MIN:
            break
        peaks = [(max(abs(dx), abs(dy)), dx, dy) for dx, dy in CITY_AREA
                 if inb(x + dx, y + dy) and land[I(x + dx, y + dy)] and plot_type[I(x + dx, y + dy)] == 0]
        if r1 < RING1_MIN:
            peaks = [p for p in peaks if p[0] == 1] or peaks
        if not peaks:
            break
        peaks.sort(key=lambda p: (p[0], rng.random()))
        _, dx, dy = peaks[0]
        j = I(x + dx, y + dy)
        plot_type[j] = 1
        if terrain[j] == 'SNOW' and abs(lat(y)) < 60:
            terrain[j] = 'TUNDRA'
        opened += 1
    city_sites[name] = (x, y) + site_counts(x, y) + (opened,)


city_sites = {}
if canyon:
    ensure_city_site('canyon', [I(x, y) for x, y in ALL
                                if min(max(abs(x - c % W), abs(y - c // W)) for c in canyon[2:-2]) <= 2])
if hidden_valley:
    ensure_city_site('hidden valley', list(HIDDEN))
if MESA_BASE:
    ensure_city_site('mesas', list(MESA_BASE))
for n, (ox, oy) in enumerate(oases):
    ensure_city_site('oasis %d' % n, [I(nx, ny) for nx, ny in nbrs(ox, oy)])
for n, path in enumerate(river_paths):
    near = [i for i in DELTA if math.hypot(i % W - path[0] % W, i // W - path[0] // W) < 4]
    if near:
        ensure_city_site('delta %d' % n, near)
if BASIN:
    ensure_city_site('salt basin', [i for i in BASIN if near_lake(i % W, i // W)])
if ALTI:
    ensure_city_site('altiplano', [i for i in ALTI if near_lake(i % W, i // W)] or list(ALTI))
if gorge:
    gp = river_paths[gorge[0]]
    ensure_city_site('gorge', [I(x, y) for x, y in window(gp[gorge[1]] % W, gp[gorge[1]] // W, 6)
                               if min(max(abs(x - p % W), abs(y - p // W)) for p in gp[max(0, gorge[1] - 6):gorge[1] + 7]) <= 3])
if ISLAND:
    ensure_city_site('river island', list(ISLAND))
if ESTUARY:
    ensure_city_site('estuary', [i for i in range(W * H) if land[i] and any(I(nx, ny) in ESTUARY for nx, ny in nbrs(i % W, i // W))])
if FINGER_SHORE:
    ensure_city_site('finger lakes', list(FINGER_SHORE))
if BARRIER:
    ensure_city_site('lagoon coast', [i for i in range(W * H) if land[i] and i not in BARRIER and any(I(nx, ny) in LAGOON for nx, ny in nbrs(i % W, i // W))])
for n, (hx, hy, nx_, ny_) in enumerate(harbours):
    ensure_city_site('harbour %d' % n, [I(x, y) for x, y in window(hx + 3 * nx_, hy + 3 * ny_, 3)
                                        if land[I(x, y)] and any(I(ax, ay) in HARBOUR_WATER for ax, ay in nbrs(x, y))])

SET_PIECES = BARRIER | FINGER_SHORE | DELTA | VALLEY_GREEN | OASIS_RING | CANYON_WALL | MESA | MESA_BASE | HIDDEN | BASIN | ALTI | GORGE | GORGE_FLOOR | ISLAND | set(I(x, y) for x, y in atolls) | set(CLAY_SCATTER) | set(i for i in range(W * H) if is_clay(i))
for _ in range(2):
    changes = {}
    for x, y in ALL:
        i = I(x, y)
        if not land[i] or plot_type[i] == 0 or i in SET_PIECES:
            continue
        cnt = Counter(terrain[I(nx, ny)] for nx, ny in nbrs(x, y) if land[I(nx, ny)] and plot_type[I(nx, ny)] != 0)
        if cnt and cnt.get(terrain[i], 0) <= 1:
            top, n = cnt.most_common(1)[0]
            if n >= 5:
                changes[i] = top
    for i, t in changes.items():
        terrain[i] = t
smoothed = len(changes)
# where grassland and prairie meet, a band of fertile plains mediates between them
prng = random.Random(SEED * 7 + 37)
to_plains = []
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] == 0 or i in SET_PIECES or terrain[i] not in ('GRASS', 'PLAINS'):
        continue
    other = 'PLAINS' if terrain[i] == 'GRASS' else 'GRASS'
    n = sum(1 for nx, ny in nbrs(x, y) if land[I(nx, ny)] and terrain[I(nx, ny)] == other)
    if n >= 2 and prng.random() < 0.55 + 0.1 * n:
        to_plains.append(i)
for i in to_plains:
    terrain[i] = 'PLAINS_FERTILE'
# scattered single hills in the open lowlands as well, away from the mountains (several resources need hills)
lone_rng = random.Random(SEED * 7 + 43)
LONE_HILLS = []
order = [(x, y) for x, y in ALL]
lone_rng.shuffle(order)
for x, y in order:
    i = I(x, y)
    if not land[i] or plot_type[i] != 2 or i in SET_PIECES or large[i] or has_river(x, y) and lone_rng.random() < 0.5:
        continue
    if terrain[i] not in ('GRASS', 'PLAINS', 'PLAINS_FERTILE', 'SAVANNAH', 'TAIGA', 'SHRUBLAND', 'TUNDRA'):
        continue
    if any(plot_type[I(nx, ny)] in (0, 1) for nx, ny in nbrs(x, y)):
        continue
    if any(max(abs(x - j % W), abs(y - j // W)) < 3 for j in LONE_HILLS):
        continue
    if lone_rng.random() < 0.6:
        plot_type[i] = 1
        LONE_HILLS.append(i)
for x, y in ALL:
    i = I(x, y)
    if land[i] and terrain[i] == 'SNOW' and abs(lat(y)) < 60 and any(lake[I(nx, ny)] for nx, ny in nbrs(x, y)):
        terrain[i] = 'TUNDRA'

for x, y in ALL:
    i = I(x, y)
    if large[i]:
        terrain[i] = 'LARGE_RIVERS'
    elif lake[i]:
        terrain[i] = 'LAKE'
    elif i in SHALLOW_WATER:
        terrain[i] = 'SHALLOW_COAST'
    elif i in HARBOUR_WATER:
        terrain[i] = 'COAST'
    elif ocean[i]:
        coastal = any(land[I(nx, ny)] or large[I(nx, ny)] for nx, ny in nbrs(x, y))
        if coastal:
            near = [(nx, ny) for nx in range(x - 3, x + 4) for ny in range(y - 3, y + 4) if inb(nx, ny)]
            shelter = sum(1 for nx, ny in near if not ocean[I(nx, ny)]) / float(len(near))
            island_sea = in_isle_sea(x, y, ISLE_R + 2)
            shallow = (island_sea and NZ[16].fbm(x, y, 2, 4) > -0.15) or (shelter > 0.5 and -25 < lat(y) < 45 and NZ[16].fbm(x, y, 2, 4) > 0.0)
            terrain[i] = 'SHALLOW_COAST' if shallow else 'COAST'
        else:
            terrain[i] = 'OCEAN'

# ---------------------------------------------------------------- connectivity (peaks + large rivers are barriers)
feature = [None] * (W * H)
for path in river_paths:
    for n, i in enumerate(path):
        if n % 7 == 3:
            feature[i] = 'RIVER_FORD'
if gorge:
    feature[river_paths[gorge[0]][gorge[1]]] = 'RIVER_FORD'


def passable(i):
    return land[i] and plot_type[i] != 0 or (large[i] and feature[i] == 'RIVER_FORD')


carved = 0
for mass in components([land[i] or large[i] for i in range(W * H)]):
    if len(mass) < 6:
        continue
    while True:
        sub = set(I(x, y) for x, y in mass)
        comps = [c for c in components([passable(i) and i in sub for i in range(W * H)]) if len(c) >= 3]
        if len(comps) <= 1:
            break
        comps.sort(key=len)
        target = set(I(x, y) for x, y in comps[-1])
        src = [I(x, y) for x, y in comps[0]]
        dist, prev, pq = {i: 0 for i in src}, {}, [(0, i) for i in src]
        found = None
        while pq:
            c, i = heapq.heappop(pq)
            if i in target:
                found = i
                break
            if c > dist[i]:
                continue
            x, y = i % W, i // W
            for nx, ny in nbrs(x, y, N4):
                j = I(nx, ny)
                if j not in sub:
                    continue
                step = 0.01 if passable(j) else 1 + ridge[j]
                if c + step < dist.get(j, 1e9):
                    dist[j], prev[j] = c + step, i
                    heapq.heappush(pq, (c + step, j))
        i = found
        while i is not None and i in prev:
            if not passable(i):
                if large[i]:
                    feature[i] = 'RIVER_FORD'
                else:
                    plot_type[i] = 1
                    if terrain[i] == 'SNOW' and T[i] > 0.15:
                        terrain[i] = 'TUNDRA'
                carved += 1
            i = prev[i]

# ---------------------------------------------------------------- features
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


def coastal_land(x, y):
    return any(ocean[I(nx, ny)] for nx, ny in nbrs(x, y))


def pick(options):
    r = rng.random() * sum(w for w, _ in options)
    for w, f in options:
        r -= w
        if r <= 0:
            return f
    return options[-1][1]


for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] == 0:
        continue
    b, t, m = terrain[i], T[i], M[i]
    flat = plot_type[i] == 2
    river = has_river(x, y)
    patch = 0.5 + 0.5 * NZ[17].fbm(x, y, 3, 3)       # coherent woods with clearings
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
            f = pick([(1, 'LICHEN'), (1, 'MOSS')]) if flat or rng.random() < 0.5 else 'MOSS'
            if f == 'LICHEN' and not flat:
                f = 'MOSS'
    elif b == 'MARSH':
        r = rng.random()
        if coastal_land(x, y) and r < 0.55:
            f = 'MANGROVE'
        elif t > 0.78 and score < 0.55:
            f = 'JUNGLE'
        elif r < 0.25:
            f = 'TROPICAL_GROVES'
        elif r < 0.28 and flat and coastal_land(x, y):
            f = 'SALT_DRAINAGE'
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
        elif r < 0.45 and flat and not coastal_land(x, y):
            f = 'BARREL_CACTUS'
        elif r < 0.58:
            f = 'LIGHT_FOREST'
    elif b == 'DESERT':
        r = rng.random()
        if river and flat and r < 0.6:
            f = 'FLOOD_PLAINS'
        elif r < 0.08 and flat:
            f = 'CACTUS_TREES'
        elif r < 0.12 and flat and not coastal_land(x, y):
            f = 'BARREL_CACTUS'
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

for i in DELTA:
    x, y = i % W, i // W
    r = rng.random()
    if terrain[i] == 'MARSH':
        feature[i] = 'MANGROVE' if coastal_land(x, y) and r < 0.65 else ('JUNGLE' if T[i] > 0.78 and r < 0.5 else ('TROPICAL_GROVES' if r < 0.4 else None))
    else:
        feature[i] = pick([(1, 'BOG'), (1, 'SWAMP')]) if has_river(x, y) and plot_type[i] == 2 and r < 0.5 else (pick([(2, 'LIGHT_FOREST'), (1, 'THICKET')]) if r < 0.45 else None)
for i in VALLEY_GREEN:
    x, y = i % W, i // W
    r = rng.random()
    if terrain[i] == 'SAVANNAH':
        feature[i] = 'TROPICAL_GROVES' if r < 0.45 else None
    else:
        feature[i] = 'LUSHGRASS' if has_river(x, y) and r < 0.5 else ('LIGHT_FOREST' if r < 0.3 else None)
for i in OASIS_RING:
    r = rng.random()
    if terrain[i] == 'SHRUBLAND':
        feature[i] = 'SHRUB' if r < 0.4 else ('LIGHT_FOREST' if r < 0.6 else None)
    else:
        feature[i] = 'LIGHT_FOREST' if r < 0.4 else None
for x, y in atolls:
    feature[I(x, y)] = 'TROPICAL_GROVES'
for i in CANYON_WALL | MESA | MESA_BASE:
    x, y = i % W, i // W
    if plot_type[i] == 0:
        feature[i] = None
        continue
    rock = 'ROCK_FORMATIONS_DESERT' if terrain[i] == 'DESERT' else 'ROCK_FORMATIONS_TEMPERATE'
    chance = 0.3 if i in MESA_BASE else 0.5
    feature[i] = rock if not coastal_land(x, y) and rng.random() < chance else None
for i in HIDDEN:
    x, y = i % W, i // W
    r = rng.random()
    if has_river(x, y) and r < 0.45:
        feature[i] = 'LUSHGRASS' if plot_type[i] == 2 else 'MEADOW'
    elif r < 0.6:
        feature[i] = pick([(3, 'FOREST'), (1, 'LIGHT_FOREST')]) if terrain[i] == 'GRASS' else pick([(2, 'TROPICAL_GROVES'), (1, 'FOREST')])
    else:
        feature[i] = None
for i in BARRIER:
    if land[i]:
        terrain[i] = 'SAVANNAH' if T[i] >= 0.75 else 'PLAINS'
        plot_type[i] = 2
        r = rng.random()
        feature[i] = ('TROPICAL_GROVES' if terrain[i] == 'SAVANNAH' else 'LIGHT_FOREST') if r < 0.2 else None
for i in FINGER_SHORE:
    if feature[i] is None and terrain[i] in ('GRASS', 'PLAINS', 'PLAINS_FERTILE', 'TAIGA') and rng.random() < 0.6:
        feature[i] = 'FOREST_EVERGREEN' if T[i] < 0.5 else 'FOREST'
for i in GORGE_FLOOR:                          # dense old woods in the cleft
    x, y = i % W, i // W
    b, r = terrain[i], rng.random()
    if b == 'TAIGA':
        f = pick([(2, 'ANCIENT_FOREST'), (2, 'FOREST_EVERGREEN')])
    elif b == 'GRASS':
        f = pick([(2, 'FOREST'), (1, 'FOREST_EVERGREEN')] + ([(2, 'DENSE_FOREST')] if not coastal_land(x, y) else []))
    else:
        f = pick([(3, 'JUNGLE'), (1, 'TROPICAL_GROVES')])
    feature[i] = f if r < 0.85 else None
for i in GORGE:
    if plot_type[i] == 0:
        continue
    b = terrain[i]
    if b in ('GRASS', 'PLAINS', 'PLAINS_FERTILE', 'TAIGA') and rng.random() < 0.7:
        feature[i] = 'FOREST_EVERGREEN'
    elif b in ('SAVANNAH', 'MARSH') and rng.random() < 0.7:
        feature[i] = 'JUNGLE'
for i in ISLAND:
    r = rng.random()
    b = terrain[i]
    if b == 'SAVANNAH':
        feature[i] = 'TROPICAL_GROVES' if r < 0.35 else None
    elif b == 'TAIGA':
        feature[i] = 'FOREST_EVERGREEN' if r < 0.4 else None
    else:
        feature[i] = pick([(2, 'FOREST'), (1, 'LIGHT_FOREST')]) if r < 0.4 else None
for i in BASIN:
    x, y = i % W, i // W
    b, r = terrain[i], rng.random()
    if plot_type[i] == 0:
        continue
    if b == 'SHRUBLAND':
        feature[i] = 'SHRUB' if r < 0.3 else None
    elif b == 'ROCK_STEPPES':
        feature[i] = 'LICHEN' if r < 0.15 and plot_type[i] == 2 else None
    else:
        feature[i] = 'ROCK_FORMATIONS_DESERT' if r < 0.15 and not near_lake(x, y) and any(plot_type[I(nx, ny)] in (0, 1) for nx, ny in nbrs(x, y)) else None
for i in ALTI:
    x, y = i % W, i // W
    b, r = terrain[i], rng.random()
    flat = plot_type[i] == 2
    if b == 'WETLAND':
        f = pick([(1, 'BOG'), (1, 'SWAMP')]) if has_river(x, y) and flat and r < 0.5 else ('THICKET' if r < 0.3 else None)
    elif b == 'TUNDRA':
        f = ('LICHEN' if flat else 'MOSS') if r < 0.3 else None
    elif b == 'ROCK_STEPPES':
        f = 'MOSS' if r < 0.15 else None
    else:
        f = 'LIGHT_FOREST' if r < 0.12 else None
    feature[i] = f

salt = [(x, y) for x, y in ALL if terrain[I(x, y)] in ('DESERT', 'ROCK_STEPPES') and plot_type[I(x, y)] == 2
        and feature[I(x, y)] is None and not coastal_land(x, y) and not has_river(x, y)]
salt.sort(key=lambda p: -(filled[I(*p)] - elev[I(*p)]) - 0.02 * rng.random())
salt_pans = []
basin_salt = [(x, y) for x, y in salt if I(x, y) in BASIN and not near_lake(x, y)]
basin_salt.sort(key=lambda p: math.hypot(p[0] - SALT_BASIN[0], p[1] - SALT_BASIN[1]))
for x, y in basin_salt:
    if len(salt_pans) < 2 and all(math.hypot(x - sx, y - sy) >= 2 for sx, sy in salt_pans):
        salt_pans.append((x, y))
        feature[I(x, y)] = 'SALT_PAN'
for x, y in salt:
    if len(salt_pans) >= 4:
        break
    if I(x, y) in SET_PIECES or (hidden_valley and math.hypot(x - hidden_valley[0], y - hidden_valley[1]) < 8):
        continue
    if any(I(nx, ny) in SET_PIECES for nx, ny in nbrs(x, y)):
        continue
    if all(math.hypot(x - sx, y - sy) >= 10 for sx, sy in salt_pans):
        salt_pans.append((x, y))
        feature[I(x, y)] = 'SALT_PAN'

# large summit models on peaks (same rules as CvMapGenerator::addImpassableMountains)
MTN_TERRAINS = {'GRASS', 'WETLAND', 'SAVANNAH', 'PLAINS', 'SHRUBLAND', 'PLAINS_FERTILE', 'TUNDRA', 'TAIGA', 'SNOW'}
for x, y in ALL:
    i = I(x, y)
    if plot_type[i] != 0 or terrain[i] not in MTN_TERRAINS or coastal_land(x, y):
        continue
    adj = [plot_type[I(nx, ny)] == 0 for nx, ny in nbrs(x, y)]
    if any((feature[I(nx, ny)] or '').startswith('REMARKABLE') for nx, ny in nbrs(x, y)) and rng.random() >= 0.05:
        continue
    n = sum(adj)
    chance = {0: 5, 1: 10, 2: 20}.get(n, 30 if n <= 4 else (45 if n <= 6 else 55))
    if rng.random() * 100 < chance:
        feature[i] = 'REMARKABLE_MOUNTAIN%d' % rng.randint(1, 3)

# meteor crater in the tropical jungle
crater = None
cands = [(x, y) for x, y in ALL if land[I(x, y)] and plot_type[I(x, y)] == 2 and abs(lat(y)) < 18
         and not coastal_land(x, y) and not has_river(x, y) and terrain[I(x, y)] in ('MARSH', 'SAVANNAH', 'WETLAND')
         and all(land[I(nx, ny)] and plot_type[I(nx, ny)] != 0 for nx, ny in nbrs(x, y))]
cands.sort(key=lambda p: (-sum(1 for nx, ny in nbrs(*p) if feature[I(nx, ny)] == 'JUNGLE'), rng.random()))
if cands:
    crater = cands[0]
    i = I(*crater)
    terrain[i] = 'MARSH'
    feature[i] = 'CRATER'
    for nx, ny in nbrs(*crater):                     # the impact rim is overgrown by jungle
        j = I(nx, ny)
        if terrain[j] in ('MARSH', 'SAVANNAH') and feature[j] is None and rng.random() < 0.7:
            feature[j] = 'JUNGLE'

# hardwood: clusters of dense old-growth forest in the temperate and boreal woods (not in jungle or savanna)
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
    if len(hw_centres) >= 18:
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

# ruins of a vanished civilisation deep in the tropical jungle, joined by overgrown roads; single ruins on the
# altiplano and near the crater
IMPROVEMENT, ROUTE = {}, {}
ru_rng = random.Random(SEED * 7 + 53)


def ruin_ok(j):
    x, y = j % W, j // W
    return land[j] and plot_type[j] == 2 and not large[j] and j not in SET_PIECES and feature[j] not in ('VOLCANO', 'CRATER', 'SALT_PAN') \
        and not coastal_land(x, y)


jungle = [i for i in range(W * H) if ruin_ok(i) and feature[i] == 'JUNGLE' and abs(lat(i // W)) < 12   # deep in the tropical jungle
          and not (crater and math.hypot(i % W - crater[0], i // W - crater[1]) < 10)]
jungle.sort(key=lambda i: -sum(1 for nx, ny in nbrs(i % W, i // W) if feature[I(nx, ny)] == 'JUNGLE') - ru_rng.random())
RUINS = []
for i in jungle:
    if len(RUINS) >= 5:
        break
    if all(3 <= max(abs(i % W - r % W), abs(i // W - r // W)) for r in RUINS) and \
            (not RUINS or max(abs(i % W - RUINS[0] % W), abs(i // W - RUINS[0] // W)) <= 7):
        RUINS.append(i)
JUNGLE_RUINS = list(RUINS)
for centre, radius in ((ALTIPLANO, 4), (crater, 4)):
    if centre:
        c = [i for i in range(W * H) if ruin_ok(i) and math.hypot(i % W - centre[0], i // W - centre[1]) <= radius]
        if c:
            RUINS.append(min(c, key=lambda i: (math.hypot(i % W - centre[0], i // W - centre[1]) < 2, ru_rng.random())))
for i in RUINS:
    IMPROVEMENT[i] = 'IMPROVEMENT_CITY_RUINS'


def road_path(a, b):
    dist, prev, pq = {a: 0}, {}, [(0, a)]
    while pq:
        c, j = heapq.heappop(pq)
        if j == b:
            break
        if c > dist[j]:
            continue
        for nx, ny in nbrs(j % W, j // W):
            q = I(nx, ny)
            if not land[q] or plot_type[q] == 0:
                continue
            step = (1.0 if plot_type[q] == 2 else 2.0) * (1.4 if nx != j % W and ny != j // W else 1.0) \
                + 1.2 * (0.5 + 0.5 * NZ[18].fbm(nx * 1.7, ny * 1.7, 2, 2))      # old tracks wind through the forest
            if c + step < dist.get(q, 1e9):
                dist[q], prev[q] = c + step, j
                heapq.heappush(pq, (c + step, q))
    if b not in prev:
        return []
    path, j = [b], b
    while j != a:
        j = prev[j]
        path.append(j)
    return path


connected = JUNGLE_RUINS[:1]
for r in JUNGLE_RUINS[1:]:                       # a small road network: every ruin to its nearest connected one
    near = min(connected, key=lambda c: math.hypot(r % W - c % W, r // W - c // W))
    path = road_path(r, near)
    if path and len(path) <= 10:
        keep = True                              # overgrown: only stretches of the old road survive
        for n, j in enumerate(path):
            if n and ru_rng.random() < 0.3:
                keep = not keep
            if keep or min(max(abs(j % W - q % W), abs(j // W - q // W)) for q in RUINS) <= 1:
                ROUTE[j] = 'ROUTE_ROAD'
    connected.append(r)

# volcanoes: on the cordilleras and island arcs (where ocean floor dives under), in the caldera, on the volcanic island
volcanoes = []
for kinds, want in ((('cordillera', 'arc'), 3), (('caldera',), 1)):
    peaks = [(x, y) for x, y in ALL if plot_type[I(x, y)] == 0 and range_dist(x, y, kinds) < 1.6]
    rng.shuffle(peaks)
    got = 0
    for x, y in peaks:
        if got < want and all(math.hypot(x - vx, y - vy) >= 4 for vx, vy in volcanoes):
            volcanoes.append((x, y))
            got += 1
if volcano_island:
    volcanoes.append(volcano_island)
for x, y in volcanoes:
    i = I(x, y)
    feature[i] = 'VOLCANO'
    plot_type[i] = 2
    if terrain[i] in ('SNOW', 'TUNDRA', 'MARSH', 'WETLAND'):
        terrain[i] = 'PLAINS' if T[i] < 0.66 else 'SAVANNAH'

# reef rings around atolls and the volcanic island
for ax, ay, rr in [(x, y, 2) for x, y in atolls] + ([volcano_island + (3,)] if volcano_island else []):
    for dx in range(-rr, rr + 1):
        for dy in range(-rr, rr + 1):
            x, y = ax + dx, ay + dy
            i = I(x, y)
            if land[i] or feature[i]:
                continue
            if terrain[i] in ('COAST', 'SHALLOW_COAST'):
                terrain[i] = 'SHALLOW_COAST'
                r = rng.random()
                feature[i] = 'STONE_REEF' if r < 0.2 else ('SEAGRASS' if r < 0.35 else None)
            elif terrain[i] == 'OCEAN' and rng.random() < 0.45:
                feature[i] = 'REEF'

# deep-water lanes: shallow coast may not cut off deep water (large ships cannot enter it)
deep = lambda i: ocean[i] and terrain[i] in ('OCEAN', 'COAST')
lanes, isolated = 0, set()
while True:
    comps = sorted([c for c in components([deep(i) for i in range(W * H)]) if I(*c[0]) not in isolated], key=len)
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
            terrain[i] = 'COAST'
            lanes += 1
        i = prev[i]

# water features
for x, y in ALL:
    i = I(x, y)
    if land[i] or large[i] or feature[i] or i in HARBOUR_WATER:
        continue
    tr, la, r = terrain[i], lat(y), rng.random()
    ice_ok = i not in PASSAGE
    if tr == 'OCEAN':
        if la > 65 and r < (la - 64) / 7.0 and ice_ok:
            feature[i] = 'ICE'
        elif r < 0.03:
            feature[i] = 'VENT_FAV'
        elif r < 0.06:
            feature[i] = 'VENT_DEF'
        elif r < 0.075 and 8 < abs(la) < 35:
            feature[i] = 'STORM'
        elif (r < 0.08 and abs(la) < 25) or (r < 0.16 and in_isle_sea(x, y, ISLE_R + 2)):
            feature[i] = 'REEF'
    elif tr in ('COAST', 'SHALLOW_COAST', 'LAKE'):
        if la > 66 and tr != 'LAKE' and r < 0.3 and ice_ok:
            feature[i] = 'ICE'
        elif r < 0.05:
            feature[i] = 'KELP' if abs(la) > 35 else 'SEAGRASS'
        elif (r < 0.08 and tr != 'LAKE') or (r < 0.12 and in_isle_sea(x, y, ISLE_R + 2) and tr != 'LAKE'):
            feature[i] = 'STONE_REEF'

pearls = []
cands = [(x, y) for x, y in ALL if terrain[I(x, y)] in ('SHALLOW_COAST', 'COAST') and feature[I(x, y)] in (None, 'SEAGRASS', 'STONE_REEF')
         and abs(lat(y)) < 25 and I(x, y) not in HARBOUR_WATER]
rng.shuffle(cands)
cands.sort(key=lambda p: terrain[I(*p)] != 'SHALLOW_COAST')
for x, y in cands:
    if len(pearls) >= 5:
        break
    if all(math.hypot(x - px, y - py) >= 10 for px, py in pearls):
        pearls.append((x, y))
        feature[I(x, y)] = 'PEARL_REEF'

# ocean currents (movement bonus arrows): an equatorial current runs west out of the open ocean, warm currents follow
# the east coasts towards the poles, a cold current runs along the west edge towards the equator
cur_rng = random.Random(SEED * 7 + 61)


def east_coast_x(y):
    xs = [x for x in range(W - MARGIN_E + 2) if not ocean[I(x, y)]]
    return max(xs) if xs else None


CURRENTS = []
ye = lat_y(cur_rng.uniform(5, 12))
xs = [x for x in range(W - 1, -1, -1) if any(inb(x - d, ye + e) and not ocean[I(x - d, ye + e)] for d in range(4) for e in (-1, 0, 1))]
x_stop = xs[0] + 3 if xs else MARGIN_W + 4
if x_stop < W - 8:
    CURRENTS.append([(W - 1, ye), ((W - 1 + x_stop) / 2.0, ye + cur_rng.uniform(-1.5, 1.5)), (x_stop, ye + cur_rng.uniform(-1, 1))])
for la0, la1, st in ((14, 50, 5), (-8, -36, -5)):
    pts = []
    for la in range(la0, la1, st):
        y = lat_y(la)
        cx = east_coast_x(y)
        if cx is None or cx + 4 >= W - 2:
            if pts:
                break
            continue
        pts.append((cx + 4 + cur_rng.uniform(-0.5, 0.5), y))
    if len(pts) >= 3:
        CURRENTS.append(pts)
CURRENTS.append([(2 + cur_rng.uniform(0, 1.5), lat_y(la)) for la in range(64, -32, -12)])
DIRS = ['E', 'NE', 'N', 'NW', 'W', 'SW', 'S', 'SE']
for pts in CURRENTS:
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        steps = int(math.hypot(bx - ax, by - ay) * 2)
        ang = math.atan2(by - ay, bx - ax)
        dname = DIRS[int(round(ang / (math.pi / 4))) % 8]
        for s in range(steps + 1):
            px, py = int(round(ax + (bx - ax) * s / steps)), int(round(ay + (by - ay) * s / steps))
            for qx, qy in ((px, py), (px + 1, py)):
                if inb(qx, qy) and terrain[I(qx, qy)] == 'OCEAN' and feature[I(qx, qy)] in (None, 'VENT_FAV', 'VENT_DEF'):
                    feature[I(qx, qy)] = 'ARROW_' + dname

# edge rivers may only run between two land plots
for i in list(wofr):
    x, y = i % W, i // W
    if not land[i] or x + 1 >= W or not land[I(x + 1, y)]:
        del wofr[i]
for i in list(nofr):
    x, y = i % W, i // W
    if not land[i] or y == 0 or not land[I(x, y - 1)]:
        del nofr[i]

# Europe (east)
dland = bfs_dist([i for i in range(W * H) if not ocean[i]], lambda j: True)
europe = [ocean[i] and terrain[i] == 'OCEAN' and i % W >= W - MARGIN_E - 3 and dland[i] >= 5 for i in range(W * H)]
pacific = [ocean[i] and terrain[i] == 'OCEAN' and i % W < MARGIN_W + 9 and dland[i] >= 5 for i in range(W * H)]

# ---------------------------------------------------------------- Atlantic coves
# three small coves on the east coasts in the south; carved last (own random stream) so the rest of the map stays as it was
SE_COVES = []
cove_rng = random.Random(SEED * 7 + 24)
cove_cands = []
for x, y in ALL:
    i = I(x, y)
    if x < W // 2 or not 1 <= y < H - 1 or lat(y) > 8 or not land[i] or plot_type[i] == 0 or i in IMPROVEMENT or i in ROUTE:
        continue
    if [(dx, dy) for dx, dy in N4 if ocean[I(x + dx, y + dy)]] != [(1, 0)]:
        continue
    if feature[I(x + 1, y)] in ('REEF', 'STONE_REEF', 'PEARL_REEF'):
        continue
    cove_cands.append((x, y))
cove_rng.shuffle(cove_cands)
for x, y in cove_cands:
    if len(SE_COVES) >= 3:
        break
    if any(abs(y - cy) < 6 for cx, cy in SE_COVES) or any(math.hypot(x - hx, y - hy) < 9 for hx, hy, _, _ in harbours):
        continue
    if any(math.hypot(x - r % W, y - r // W) < 4 for r in river_cells) or any(large[I(x + dx, y + dy)] for dx in range(-3, 4) for dy in range(-3, 4) if inb(x + dx, y + dy)):
        continue
    water_cells = [(x, y), (x - 1, y)]                  # a short random walk inland, no fixed shape
    want = cove_rng.randint(3, 5)
    for _ in range(30):
        if len(water_cells) >= want:
            break
        bx, by = cove_rng.choice(water_cells[1:])
        dx, dy = cove_rng.choice(N4)
        if dx > 0 or (bx + dx, by + dy) in water_cells or bx + dx - x > -1:
            continue
        water_cells.append((bx + dx, by + dy))
    ok = all(inb(cx, cy) and land[I(cx, cy)] and plot_type[I(cx, cy)] != 0 and not lake[I(cx, cy)] and I(cx, cy) not in IMPROVEMENT and I(cx, cy) not in ROUTE
             for cx, cy in water_cells)
    for cx, cy in water_cells:
        for qx, qy in nbrs(cx, cy):
            if (qx, qy) in water_cells or qx - x > 0:
                continue
            if not inb(qx, qy) or not land[I(qx, qy)]:
                ok = False
    if not ok:
        continue
    SE_COVES.append((x, y))
    mode = cove_rng.random()                            # shallow cove, shallow inner part, or deep anchorage
    for n, (cx, cy) in enumerate(water_cells):
        j = I(cx, cy)
        land[j], ocean[j], plot_type[j], feature[j] = False, True, 3, None
        if mode < 0.4 or (mode < 0.7 and n >= 2):
            terrain[j] = 'SHALLOW_COAST'
            SHALLOW_WATER.add(j)
        else:
            terrain[j] = 'COAST'
            HARBOUR_WATER.add(j)
        for key in (j, I(cx - 1, cy) if cx > 0 else None):    # river edges inside the cove disappear
            wofr.pop(key, None)
        nofr.pop(j, None)
        if cy < H - 1:
            nofr.pop(I(cx, cy + 1), None)

# ---------------------------------------------------------------- Caribbean revision
# Late changes (own random stream, so the rest of the map stays as it was): the westernmost gulf island makes way for a narrow strip of
# Atlantic Europe plots, most rock reefs of the island sea disappear, an ocean current curves in from the east towards the strip,
# and part of the shrubland in the southern continent turns into desert.
car_rng = random.Random(SEED * 7 + 25)
west_isle = min(archipelago, key=lambda c: min(x for x, _ in c)) if archipelago else []
if west_isle:
    archipelago.remove(west_isle)
for x, y in west_isle:
    j = I(x, y)
    land[j], ocean[j], plot_type[j], terrain[j], feature[j] = False, True, 3, 'COAST', None
    IMPROVEMENT.pop(j, None)
    ROUTE.pop(j, None)
    wofr.pop(j, None)
    nofr.pop(j, None)
    wofr.pop(I(x - 1, y), None)
    nofr.pop(I(x, y + 1), None)
wx, wy = ISLE_SEA
if west_isle:
    wx = int(round(sum(x for x, _ in west_isle) / float(len(west_isle))))
    wy = int(round(sum(y for _, y in west_isle) / float(len(west_isle))))
CARIB_EUROPE = [I(wx, wy + d) for d in range(-2, 3) if inb(wx, wy + d)] if west_isle else []     # a narrow strip, five plots long
for i in CARIB_EUROPE:
    terrain[i], feature[i] = 'OCEAN', None
    for qx, qy in nbrs(i % W, i // W):
        j = I(qx, qy)
        if ocean[j] and not large[j] and not lake[j] and j not in CARIB_EUROPE:
            if terrain[j] == 'SHALLOW_COAST':
                terrain[j] = 'COAST'
            if feature[j] in ('REEF', 'STONE_REEF', 'PEARL_REEF', 'SEAGRASS', 'KELP'):
                feature[j] = None
    SHALLOW_WATER.discard(i)
    HARBOUR_WATER.discard(i)

# the strip must stay reachable for large ships: join it to the main body of deep water if necessary
deep = lambda i: ocean[i] and terrain[i] in ('OCEAN', 'COAST') and not large[i]
comps = sorted(components([deep(i) for i in range(W * H)]), key=len)
main_deep = set(I(x, y) for x, y in comps[-1])
if CARIB_EUROPE and CARIB_EUROPE[0] not in main_deep:
    prev, dq, found = {CARIB_EUROPE[0]: None}, deque([CARIB_EUROPE[0]]), None
    while dq and found is None:
        i = dq.popleft()
        for nx, ny in nbrs(i % W, i // W):
            j = I(nx, ny)
            if j in prev or not ocean[j] or large[j] or lake[j]:
                continue
            prev[j] = i
            if j in main_deep:
                found = j
                break
            dq.append(j)
    while found is not None and prev[found] is not None:
        found = prev[found]
        if terrain[found] == 'SHALLOW_COAST':
            terrain[found] = 'COAST'
            feature[found] = None

# most rock reefs (FEATURE_STONE_REEF) of the island sea go; coral reefs (FEATURE_REEF) and pearl reefs stay
reefs_removed = 0
for x, y in ALL:
    i = I(x, y)
    if in_isle_sea(x, y, ISLE_R + 3) and feature[i] == 'STONE_REEF' and car_rng.random() < 0.75:
        feature[i] = None
        reefs_removed += 1

# warm current: a narrow, interrupted half circle from the east ocean through the island sea to the strip; arrows only fit on open ocean,
# so plain deep coast without land next to it (no features) is opened up as ocean where the arc crosses it
cx_, cy_ = (wx + min(W - MARGIN_E, ISLE_SEA[0] + ISLE_R + 2)) / 2.0, float(wy - 2)
ax_, by_ = cx_ - wx, 6.5
CAR_ARROWS, prev_pt = 0, None
for k in range(401 if CARIB_EUROPE else 0):
    t = math.pi * k / 400.0                                          # from the east point over the north to the west point
    gx, gy = int(round(cx_ + ax_ * math.cos(t))), int(round(cy_ + by_ * math.sin(t)))
    if (gx, gy) == prev_pt or not inb(gx, gy):
        continue
    prev_pt = (gx, gy)
    i = I(gx, gy)
    if not ocean[i] or large[i] or lake[i] or feature[i] is not None or i in CARIB_EUROPE:
        continue
    if terrain[i] == 'COAST' and not any(land[I(nx, ny)] for nx, ny in nbrs(gx, gy)):
        terrain[i] = 'OCEAN'
    if terrain[i] == 'OCEAN':
        ang = math.atan2(by_ * math.cos(t), -ax_ * math.sin(t))     # tangent in the direction of travel
        feature[i] = 'ARROW_' + DIRS[int(round(ang / (math.pi / 4))) % 8]
        CAR_ARROWS += 1

# shrubland of the southern continent turns into desert, starting where it already borders desert (about 30 %); the cape in the far south keeps its shrubland
south_shrub = [I(x, y) for x, y in ALL if lat(y) < 0 and land[I(x, y)] and terrain[I(x, y)] == 'SHRUBLAND']
dd = bfs_dist([i for i in range(W * H) if land[i] and terrain[i] == 'DESERT'], lambda j: True)
protected = set()
for ox, oy in oases:
    for dx in range(-3, 4):
        for dy in range(-3, 4):
            if inb(ox + dx, oy + dy):
                protected.add(I(ox + dx, oy + dy))
elig = [i for i in south_shrub if i not in protected and i // W > 4 and not has_river(i % W, i // W)
        and not any(lake[I(nx, ny)] or large[I(nx, ny)] for nx, ny in nbrs(i % W, i // W))]
elig.sort(key=lambda i: dd[i] + car_rng.uniform(0, 4))
to_desert = elig[:int(0.3 * len(south_shrub))]
for i in to_desert:
    terrain[i] = 'DESERT'
    if feature[i] not in (None, 'ROCK_FORMATIONS_DESERT', 'CACTUS_TREES', 'BARREL_CACTUS'):
        feature[i] = None
print('caribbean revision: west isle', len(west_isle), 'strip', [(i % W, i // W) for i in CARIB_EUROPE], 'rock reefs removed', reefs_removed, 'current arrows', CAR_ARROWS,
      'shrubland south', len(south_shrub), 'to desert', len(to_desert))

# ---------------------------------------------------------------- landmark names
# map signs (landmarks visible to every player) give the invented landscape its names;
# captions are plain ASCII, without commas or '='
SIGNS = []


def add_sign(cells, caption, gap=5):
    for c in cells:
        x, y = (c % W, c // W) if isinstance(c, int) else c
        if inb(x, y) and all(max(abs(x - sx), abs(y - sy)) >= gap for sx, sy, _ in SIGNS):
            SIGNS.append((x, y, caption))
            return True
    return False


def by_dist(cells, cx, cy):
    return sorted(cells, key=lambda c: math.hypot(c % W - cx, c // W - cy))


def range_peaks(R):
    return [i for i in range(W * H) if plot_type[i] == 0 and poly_dist(i % W, i // W, R['pts'])[0] < 2 * R['w'] + 0.5]


def name_range(R, peak_name, range_name=None):
    pk = range_peaks(R)
    if not pk:
        return
    if peak_name:                                    # the highest summit of the chain
        add_sign(sorted(pk, key=lambda i: -(ridge[i] + 0.02 * (feature[i] or '').startswith('REMARKABLE'))), peak_name)
    if range_name:
        mx, my = R['pts'][len(R['pts']) // 2]
        add_sign(by_dist(pk, mx, my), range_name)


# set pieces first, they own their spot
if volcano_island:
    add_sign([volcano_island], 'Saint Brendan Island')
if crater:
    add_sign([crater], 'Halloway Crater')
if canyon:
    add_sign(canyon[len(canyon) // 2:] + canyon, 'Red Rock Canyon')
if hidden_valley:
    add_sign(by_dist(list(HIDDEN), *hidden_valley), 'Paradise Valley')
if gorge:
    gp = river_paths[gorge[0]]
    add_sign([gp[gorge[1]]] + gp, 'Crawford Gorge')
if ISLAND:
    add_sign(by_dist(list(ISLAND), sum(i % W for i in ISLAND) / len(ISLAND), sum(i // W for i in ISLAND) / len(ISLAND)), 'Beaver Island')
if ESTUARY:
    add_sign(by_dist(list(ESTUARY), *max(((i % W, i // W) for i in ESTUARY), key=lambda p: -p[1])), 'Chester Bay')
if LAGOON:
    ban = sorted(BARRIER, key=lambda i: i // W)
    add_sign(ban[len(ban) // 2:] + ban, 'Hatton Banks')
if FINGER:
    add_sign(by_dist(list(FINGER), sum(i % W for i in FINGER) / len(FINGER), sum(i // W for i in FINGER) / len(FINGER)), 'Mohegan Lakes')
if SALT_BASIN:
    add_sign(by_dist([i for i in range(W * H) if lake[i]], *SALT_BASIN), 'Great Salt Basin')
if ALTIPLANO:
    add_sign(by_dist([i for i in range(W * H) if lake[i]], *ALTIPLANO), 'Altiplano de Sorata')
if mesa_centres:
    add_sign([I(x, y) for x, y in mesa_centres], 'Painted Mesas')
for (ox, oy), name in zip(oases, ('Sweetwater Springs', 'Willow Springs', 'Agua Caliente', 'Bitter Springs', 'Ojo del Toro')):
    add_sign([(ox, oy)], name)
if PASSAGE:
    pas = sorted(PASSAGE, key=lambda i: i % W)
    add_sign([i for i in pas if W - 20 <= i % W <= W - 10] + pas, 'Frobisher Strait')
arctic = sorted([c for c in components(land) if all(y > strait_y(x) for x, y in c)], key=len, reverse=True)
for comp, name in zip(arctic, ('Prince Rupert Island', 'Bylot Island')):
    if len(comp) >= 8:
        cx = sum(x for x, _ in comp) / float(len(comp))
        cy = sum(y for _, y in comp) / float(len(comp))
        add_sign(by_dist([I(x, y) for x, y in comp if plot_type[I(x, y)] != 0] or [I(x, y) for x, y in comp], cx, cy), name)
if archipelago:
    allc = [I(x, y) for isle in archipelago for x, y in isle]
    add_sign(by_dist(allc, *ISLE_SEA), 'Windward Isles')
for (hx, hy, nx_, ny_), name in zip(harbours, ('Mercy Bay', 'Hope Sound')):
    add_sign([(hx + 3 * nx_, hy + 3 * ny_), (hx + nx_, hy + ny_)], name)
if fjords:
    add_sign([(fx + 2, fy) for fx, fy in fjords[1:]] + fjords, 'Markland Coast')
for (vx, vy), name in zip([v for v in volcanoes if v != volcano_island], ('Volcan San Lorenzo', 'Volcan Santa Rosa', 'Cerro Humeante')):
    add_sign([(vx, vy)], name)

if JUNGLE_RUINS:
    cx = sum(i % W for i in JUNGLE_RUINS) / float(len(JUNGLE_RUINS))
    cy = sum(i // W for i in JUNGLE_RUINS) / float(len(JUNGLE_RUINS))
    add_sign(sorted(JUNGLE_RUINS, key=lambda i: math.hypot(i % W - cx, i // W - cy)), 'Ciudad Perdida')
for i, name in zip(RUINS[len(JUNGLE_RUINS):], ('Ruinas de Huaynacocha', 'Ruinas de Xunantal')):
    add_sign([i], name, 3)

# mountain ranges: the highest summit of each chain and the chain itself
# (largest first; in the warm south Spanish and Portuguese names)
names = {False: (['Mount Whitcomb', 'Mount Harcourt', 'Mount Ellery', 'Bald Knob', 'Mount Ashcombe', 'Mount Tolland'],
                 ['Montgomery Range', 'Granite Range', 'Copper Ridge', 'Laurel Ridge', 'Pine Ridge', 'Blackstone Mountains', 'Thunder Range']),
         True: (['Cerro Alto', 'Cerro Blanco', 'Nevado del Sol', 'Pico Bandeira'],
                ['Sierra Grande', 'Serra Verde', 'Cordillera Azul', 'Sierra de Plata', 'Serra do Mar'])}
for R in sorted([R for R in RANGES if R.get('kind') in ('fold', 'cordillera', 'arc', 'old')], key=lambda R: -poly_len(R['pts']) * R['h']):
    peaks_, chains_ = names[lat(R['pts'][len(R['pts']) // 2][1]) < 12]
    pk_ = peaks_.pop(0) if peaks_ and R['h'] >= 0.9 else None
    ch_ = chains_.pop(0) if chains_ and poly_len(R['pts']) >= 8 else None
    if pk_ or ch_:
        name_range(R, pk_, ch_)
for R in RANGES:
    if R.get('kind') == 'caldera':
        name_range(R, None, 'Cerros Redondos')

# large rivers: the name sits a little upstream of the mouth
north_rivers = ['Raven River', 'Elk River', 'Saint Croix River', 'Black River', 'Kettle River']
south_rivers = ['Rio Dorado', 'Rio Verde', 'Rio Pardo', 'Rio Negro']
for pn, path in enumerate(river_paths):
    if (canyon and canyon[0] in path) or (gorge and gorge[0] == pn):
        continue                                     # the canyon or gorge sign names this river already
    k = max(1, len(path) // 3)
    pool = south_rivers if lat(path[0] // W) < 15 else north_rivers
    if pool:
        add_sign(path[k:] + path[:k], pool.pop(0))

# remaining lakes from five tiles upwards
lake_names = ['Lake Winthrop', 'Lake Pembroke', 'Clear Lake', 'Lake Hartley', 'Loon Lake', 'Lake Whitmore', 'Long Lake',
              'Lake Ashby', 'Lake Mercer']
south_lakes = ['Laguna Seca', 'Lago Azul', 'Laguna Grande']
for comp in sorted(components(lake), key=len, reverse=True):
    if len(comp) < 5 or not lake_names or any(I(x, y) in FINGER for x, y in comp):
        continue
    cx = sum(x for x, _ in comp) / float(len(comp))
    cy = sum(y for _, y in comp) / float(len(comp))
    if any(math.hypot(cx - sx, cy - sy) < 4 for sx, sy, _ in SIGNS):
        continue
    pool = south_lakes if lat(cy) < 15 and south_lakes else lake_names
    if add_sign(by_dist([I(x, y) for x, y in comp], cx, cy), pool[0]):
        pool.pop(0)

# large stretches of flat woodland get a regional name, unless a bay, river or other sign already names the area
WOODS = {'FOREST', 'FOREST_EVERGREEN', 'DENSE_FOREST', 'ANCIENT_FOREST', 'FOREST_TUNDRA', 'JUNGLE', 'TROPICAL_GROVES', 'LIGHT_FOREST'}
region_names = {'boreal': ['New Caledonia', 'Keewatin'], 'temperate': ['New Albion', 'Acadia', 'New Hanover', 'Carolana'],
                'subtropical': ['Nueva Galicia', 'Vera Paz'],
                'south': ['Terra de Santa Cruz', 'Nueva Andalucia', 'Tierra Firme']}
cands = []
for x, y in ALL:
    i = I(x, y)
    if not land[i] or plot_type[i] != 2:
        continue
    disc = [(x + dx, y + dy) for dx in range(-4, 5) for dy in range(-4, 5) if dx * dx + dy * dy <= 16 and inb(x + dx, y + dy)]
    lnd = [I(px, py) for px, py in disc if land[I(px, py)]]
    if len(lnd) < 0.85 * len(disc) or sum(1 for j in lnd if plot_type[j] == 0) > 3:
        continue
    woods = sum(1 for j in lnd if plot_type[j] == 2 and feature[j] in WOODS) / float(len(lnd))
    if woods >= 0.5:
        cands.append((-woods, i))
cands.sort()
regions = 0
for _, i in cands:
    x, y = i % W, i // W
    if any(math.hypot(x - sx, y - sy) < 7 for sx, sy, _ in SIGNS):
        continue
    zone = 'south' if lat(y) < 5 else ('boreal' if T[i] < 0.4 else ('subtropical' if T[i] >= 0.66 else 'temperate'))
    if region_names[zone] and add_sign([i], region_names[zone].pop(0)):
        regions += 1

# Europe: Atlantic (east) and Pacific (west), both at least five tiles from land
dland = bfs_dist([i for i in range(W * H) if not ocean[i]], lambda j: True)
europe = [ocean[i] and terrain[i] == 'OCEAN' and i % W >= W - MARGIN_E - 3 and dland[i] >= 5 for i in range(W * H)]
pacific = [ocean[i] and terrain[i] == 'OCEAN' and i % W < MARGIN_W + 9 and dland[i] >= 5 for i in range(W * H)]
for i in CARIB_EUROPE:
    europe[i] = True                         # narrow Atlantic strip inside the island sea
# where the west coast runs close to the map edge, the outermost columns still open to the Pacific
for y in range(H):
    if not any(pacific[I(x, y)] for x in range(MARGIN_W + 9)):
        for x in (0, 1):
            i = I(x, y)
            if ocean[i] and feature[i] != 'ICE' and i not in HARBOUR_WATER:
                terrain[i] = 'OCEAN'                 # Europe plots are open ocean, even this close to the coast
                if feature[i] in ('KELP', 'SEAGRASS', 'STONE_REEF', 'PEARL_REEF'):
                    feature[i] = None
                pacific[i] = True

# ---------------------------------------------------------------- final checks
# On generated land the late carvings (coves, the strip in the island sea) can put a feature onto a new coast; the
# coast rules of the features are read from CIV4FeatureInfos.xml. Small water bodies cut off from the open sea
# would become lakes when the game loads the map, so they become lakes here.
import re as _re
_fx = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Assets', 'XML', 'Terrain', 'CIV4FeatureInfos.xml'),
           encoding='utf8').read()
COAST_RULE = {}
for _b in _re.findall(r'<FeatureInfo>(.*?)</FeatureInfo>', _fx, _re.S):
    _g = lambda k: (_re.search('<' + k + r'>(\d)', _b) or [0, '0'])[1] == '1'
    _tb = _re.search(r'<TerrainBooleans>(.*?)</TerrainBooleans>', _b, _re.S)
    _ok = set(a[8:] for a, v in _re.findall(r'<TerrainType>(\w+)</TerrainType>\s*<bTerrain>(\d)', _tb.group(1)) if v == '1') if _tb else set()
    COAST_RULE[_re.search(r'<Type>FEATURE_(\w+)</Type>', _b).group(1)] = (_g('bNoCoast'), _g('bOnlyCoastalLand'), _ok)
fixed_features = 0
for x, y in ALL:
    i = I(x, y)
    f = feature[i]
    if f and f in COAST_RULE and COAST_RULE[f][2] and terrain[i] not in COAST_RULE[f][2]:
        feature[i] = None                      # a later terrain change left the feature on a terrain it does not grow on
        fixed_features += 1
        continue
    if not f or not land[i]:
        continue
    nocoast, onlycoast, _ = COAST_RULE.get(f, (False, False, None))
    coastal = any(ocean[I(nx, ny)] and not lake[I(nx, ny)] and not large[I(nx, ny)] for nx, ny in nbrs(x, y))
    if (nocoast and coastal) or (onlycoast and not coastal):
        feature[i] = 'FOREST' if f == 'DENSE_FOREST' and terrain[i] in FEATURE_TERRAINS['FOREST'] else None
        fixed_features += 1
small_seas = 0
for comp in components([ocean[i] and not large[i] for i in range(W * H)]):
    cells = [I(x, y) for x, y in comp]
    if len(cells) < 50 and not any(europe[j] or pacific[j] for j in cells):
        for j in cells:
            ocean[j], lake[j], terrain[j] = False, True, 'LAKE'
            if feature[j] not in ('KELP', 'SEAGRASS'):
                feature[j] = None
            HARBOUR_WATER.discard(j)
        small_seas += 1
print('final checks: features fixed', fixed_features, 'small seas turned into lakes', small_seas)

# ---------------------------------------------------------------- output
os.makedirs(OUT, exist_ok=True)
wb = os.path.join(OUT, 'Random New World Huge.ColonizationWBSave')
with open(wb, 'w', newline='\r\n') as f:
    f.write('Version=11\nBeginGame\n\tDescription=TXT_KEY_WORLDBUILDER_RANDOM_NEW_WORLD_DESC\n\tModPath=\n')
    f.write('\tWBNorthAmericanNative=1\n\tWBSouthAmericanNative=1\n\tWBCentralAmericanNative=1\nEndGame\n')
    for p in range(49):
        f.write('BeginPlayer\n\tTeam=%d\n\tLeaderType=NONE\n\tCivType=NONE\nEndPlayer\n' % p)
    f.write('BeginMap\n\tgrid width=%d\n\tgrid height=%d\n\ttop latitude=%d\n\tbottom latitude=%d\n' % (W, H, TOP_LAT, BOTTOM_LAT))
    f.write('\twrap X=0\n\twrap Y=0\n\tworld size=WORLDSIZE_HUGE\n\tclimate=CLIMATE_TEMPERATE\n\tsealevel=SEALEVEL_MEDIUM\n')
    f.write('\tnum plots written=%d\n\tnum signs written=%d\n' % (W * H, len(SIGNS)))
    f.write('\tRandomize Features=false\n\tRandomize Resources=true\n\tRandomize Goodies=true\nEndMap\n\n### Plot Info ###\n')
    for x in range(W):
        for y in range(H):
            i = I(x, y)
            f.write('BeginPlot\n\tx=%d,y=%d\n' % (x, y))
            if i in wofr:
                f.write('\tRiverNSDirection=%d\n\tisWOfRiver\n' % wofr[i])
            if i in nofr:
                f.write('\tRiverWEDirection=%d\n\tisNOfRiver\n' % nofr[i])
            if europe[i]:
                f.write('\tEuropeType=EUROPE_EAST\n')
            elif pacific[i]:
                f.write('\tEuropeType=EUROPE_WEST\n')
            if i in IMPROVEMENT:
                f.write('\tImprovementType=%s\n' % IMPROVEMENT[i])
            if feature[i]:
                f.write('\tFeatureType=FEATURE_%s, FeatureVariety=0\n' % feature[i])
            if i in ROUTE:
                f.write('\tRouteType=%s\n' % ROUTE[i])
            f.write('\tTerrainType=TERRAIN_%s\n\tPlotType=%d\nEndPlot\n' % (terrain[i], plot_type[i]))
    f.write('\n### Sign Info ###\n')
    for x, y, caption in SIGNS:
        f.write('BeginSign\n\tplotX=%d\n\tplotY=%d\n\tplayerType=-1\n\tcaption=%s\nEndSign\n' % (x, y, caption))

# ---------------------------------------------------------------- preview
COL = {
    'OCEAN': (28, 58, 110), 'COAST': (45, 95, 160), 'SHALLOW_COAST': (70, 140, 185), 'LAKE': (60, 120, 200),
    'LARGE_RIVERS': (40, 110, 230), 'SNOW': (240, 240, 245), 'TUNDRA': (170, 175, 160), 'TAIGA': (110, 140, 115),
    'GRASS': (90, 170, 70), 'PLAINS_FERTILE': (165, 190, 90), 'PLAINS': (200, 185, 110), 'SAVANNAH': (175, 175, 60),
    'SHRUBLAND': (200, 160, 100), 'DESERT': (235, 215, 150), 'ROCK_STEPPES': (165, 145, 120),
    'MARSH': (80, 130, 100), 'WETLAND': (100, 140, 140),
}
FOREST_COL = {'JUNGLE': (20, 90, 30), 'DENSE_FOREST': (20, 80, 30), 'ANCIENT_FOREST': (30, 70, 50)}
S = 8
img = [[(0, 0, 0)] * (W * S) for _ in range(H * S)]
for x, y in ALL:
    i = I(x, y)
    c = COL[terrain[i]]
    if europe[i] or pacific[i]:
        c = (22, 45, 90)
    f = feature[i] or ''
    for sy in range(S):
        for sx in range(S):
            px, py = x * S + sx, (H - 1 - y) * S + sy
            col = c
            if plot_type[i] == 1 and (sx + sy) % 4 == 0:
                col = tuple(max(0, v - 45) for v in c)
            if plot_type[i] == 0:
                col = (95, 85, 80) if abs(sx - S // 2) <= sy // 2 else c
                if f == 'VOLCANO' and abs(sx - S // 2) <= sy // 2:
                    col = (200, 40, 20)
                elif f.startswith('REMARKABLE') and abs(sx - S // 2) <= sy // 2:
                    col = (60, 50, 45)
            if f == 'VOLCANO' and abs(sx - S // 2) <= sy // 2:
                col = (200, 40, 20)
            if ('FOREST' in f or f in ('JUNGLE', 'TROPICAL_GROVES', 'MANGROVE')) and (sx % 3 == 1 and sy % 3 == 1):
                col = FOREST_COL.get(f, (30, 100, 40))
            if f.startswith('ARROW') and sx == S // 2 and sy == S // 2:
                col = (200, 220, 255)
            if i in ROUTE and sy == S // 2:
                col = (150, 90, 40)
            if i in IMPROVEMENT and 2 <= sx <= 5 and 2 <= sy <= 5:
                col = (120, 120, 120)
            if f == 'RIVER_FORD' and sy in (3, 4):
                col = (170, 140, 90)
            img[py][px] = col
for x, y, _ in SIGNS:
    for sy in range(2):
        for sx in range(2):
            img[(H - 1 - y) * S + sy][x * S + sx] = (255, 255, 255)
# edge rivers
RC = (40, 110, 230)
for i, d in wofr.items():
    x, y = i % W, i // W
    for sy in range(S):
        img[(H - 1 - y) * S + sy][x * S + S - 1] = RC
        if x + 1 < W:
            img[(H - 1 - y) * S + sy][(x + 1) * S] = RC
for i, d in nofr.items():
    x, y = i % W, i // W
    for sx in range(S):
        img[(H - 1 - y) * S + S - 1][x * S + sx] = RC
        if y - 1 >= 0:
            img[(H - 1 - (y - 1)) * S][x * S + sx] = RC
write_png(os.path.join(OUT, 'preview.png'), W * S, H * S, img)


# height map (land: lowland green to snowy summits, sea: shelf to deep ocean) and precipitation map (land: dry
# brown to wet blue, peaks dark): the rain shadow behind the ranges shows directly
def ramp(stops, v):
    v = max(0.0, min(1.0, v))
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if v <= b:
            t = (v - a) / (b - a) if b > a else 0.0
            return tuple(int(ca[k] + (cb[k] - ca[k]) * t) for k in range(3))
    return stops[-1][1]


HSTOPS = [(0.0, (70, 130, 70)), (0.25, (150, 175, 95)), (0.45, (200, 170, 105)), (0.7, (135, 105, 85)), (1.0, (250, 250, 250))]
DSTOPS = [(0.0, (110, 170, 210)), (0.35, (45, 100, 170)), (1.0, (15, 35, 85))]
PSTOPS = [(0.0, (190, 140, 80)), (0.2, (225, 205, 125)), (0.4, (160, 195, 95)), (0.65, (60, 150, 80)), (1.0, (40, 90, 175))]
S2 = 6
himg = [[(0, 0, 0)] * (W * S2) for _ in range(H * S2)]
pimg = [[(0, 0, 0)] * (W * S2) for _ in range(H * S2)]
for x, y in ALL:
    i = I(x, y)
    if land[i] or lake[i] or large[i]:
        hc = (60, 120, 200) if (lake[i] or large[i]) else ramp(HSTOPS, elev[i] / 1.2)
        pc = (60, 120, 200) if (lake[i] or large[i]) else ramp(PSTOPS, P[i] / 1.1)
    else:
        hc = ramp(DSTOPS, (SEA_LEVEL - HEIGHT0[i]) / 0.9)
        pc = (70, 80, 95)
    for sy in range(S2):
        for sx in range(S2):
            px, py = x * S2 + sx, (H - 1 - y) * S2 + sy
            himg[py][px] = hc
            peak = plot_type[i] == 0 and abs(sx - S2 // 2) <= sy // 2
            pimg[py][px] = (70, 60, 55) if peak else pc
write_png(os.path.join(OUT, 'heightmap.png'), W * S2, H * S2, himg)
write_png(os.path.join(OUT, 'rainfall.png'), W * S2, H * S2, pimg)

if os.environ.get('LRDUMP'):
    for y in range(H - 1, -1, -1):
        print('%3d ' % y + ''.join('L' if large[I(x, y)] else ('o' if lake[I(x, y)] else ('~' if ocean[I(x, y)] else ('^' if plot_type[I(x, y)] == 0 else '.'))) for x in range(W)))
# ---------------------------------------------------------------- stats
from collections import Counter
nl = sum(1 for i in range(W * H) if land[i])
print('clay hills', len(CLAY_HILLS), 'scattered', len(CLAY_SCATTER)); print('city sites (x, y, ring1, area, peaks opened):', city_sites); print('bulge peaks', len(bulges), 'salt pans', salt_pans); print('islands', len(archipelago), sorted(len(a) for a in archipelago), 'lanes opened', lanes); print('hidden valley', hidden_valley, 'canyon', [(c % W, c // W) for c in canyon[:1]], len(canyon), 'mesas', len(mesa_centres)); print('minis', len(minis), 'fjords', len(fjords), 'skerries', len(skerries), 'atolls', atolls, 'volcanoes', volcanoes, 'oases', oases); print('seed', SEED, 'land', nl, '(%.0f%%)' % (100.0 * nl / (W * H)), 'carved passes', carved)
print('coves', [(h[0], h[1]) for h in coves], 'sea isles', [(c[0], len(c)) for c in SEA_ISLES], 'gulf isles', sorted(len(a) for a in archipelago), 'volcano isle', len(VOLCANO_ISLE))
print('passage', len(PASSAGE), 'estuary', len(ESTUARY), len(ESTUARY_ARM), 'lagoon', len(LAGOON), 'barrier', len(BARRIER), 'finger lakes', len(FINGER), 'crater', crater)
print('harbours', [(h[0], h[1]) for h in harbours], 'salt basin', SALT_BASIN, len(BASIN), 'altiplano', ALTIPLANO, len(ALTI),
      'gorge', gorge and (river_paths[gorge[0]][gorge[1]] % W, river_paths[gorge[0]][gorge[1]] // W), len(GORGE), 'island', sorted((i % W, i // W) for i in ISLAND)[:1], len(ISLAND))
print('signs', len(SIGNS), SIGNS)
print('hardwood clusters', len(hw_centres), len(HARDWOOD), 'ruins', [(i % W, i // W) for i in RUINS], 'road plots', len(ROUTE))
print('lone hills', len(LONE_HILLS), 'plains converted', len(to_plains))
print('plates', NPL, 'continental', sum(p['cont'] for p in plates), 'ranges', Counter(R.get('kind') for R in RANGES), 'rifts', len(RIFTS),
      'island sea', ISLE_SEA, 'caldera', CALDERA, 'currents', len(CURRENTS))
print('peaks', sum(1 for i in range(W * H) if plot_type[i] == 0), 'hills', sum(1 for i in range(W * H) if plot_type[i] == 1))
print('lakes', sum(lake), 'large river plots', sum(large), 'paths', [(len(p), p[0] % W, p[0] // W) for p in river_paths])
print('river edges', len(wofr) + len(nofr), 'europe', sum(europe), 'pacific', sum(pacific))
print(sorted(Counter(terrain[i] for i in range(W * H) if land[i]).items(), key=lambda kv: -kv[1]))
print(sorted(Counter(feature[i] for i in range(W * H) if feature[i]).items(), key=lambda kv: -kv[1]))
