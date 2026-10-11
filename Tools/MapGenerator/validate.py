import os, re, sys
from collections import deque

MOD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Assets', 'XML', 'Terrain')
path = sys.argv[1]

terrains = set(re.findall(r'<Type>(TERRAIN_\w+)</Type>', open(os.path.join(MOD, 'CIV4TerrainInfos.xml'), encoding='utf8').read()))
finfo = {}
for b in re.findall(r'<FeatureInfo>(.*?)</FeatureInfo>', open(os.path.join(MOD, 'CIV4FeatureInfos.xml'), encoding='utf8').read(), re.S):
    t = re.search(r'<Type>(\w+)</Type>', b).group(1)
    tb = re.search(r'<TerrainBooleans>(.*?)</TerrainBooleans>', b, re.S)
    ok = {a for a, v in re.findall(r'<TerrainType>(\w+)</TerrainType>\s*<bTerrain>(\d)', tb.group(1)) if v == '1'} if tb else set()
    g = lambda k: (re.search('<' + k + r'>(\d)', b) or [0, '0'])[1] == '1'
    finfo[t] = dict(ok=ok, flat=g('bRequiresFlatlands'), hills=g('bRequiresHills'), river=g('bRequiresRiver'), nocoast=g('bNoCoast'), onlycoast=g('bOnlyCoastalLand'))

plots, cur = {}, None
W = H = 0
for line in open(path):
    s = line.strip()
    m = re.match(r'grid (width|height)=(\d+)', s)
    if m:
        if m.group(1) == 'width':
            W = int(m.group(2))
        else:
            H = int(m.group(2))
    if s == 'BeginPlot':
        cur = {}
    elif s == 'EndPlot':
        plots[(cur['x'], cur['y'])] = cur
        cur = None
    elif cur is not None:
        m = re.match(r'x=(\d+),y=(\d+)', s)
        if m:
            cur['x'], cur['y'] = int(m.group(1)), int(m.group(2))
        elif s.startswith('FeatureType='):
            cur['f'] = s.split('=')[1].split(',')[0]
        else:
            k, _, v = s.partition('=')
            cur[k] = v if v else True

err = []
assert len(plots) == W * H, (len(plots), W, H)
water = lambda p: p['PlotType'] == '3'


def river_at(x, y):
    p = plots[(x, y)]
    return 'isWOfRiver' in p or 'isNOfRiver' in p or (x > 0 and 'isWOfRiver' in plots[(x - 1, y)]) or (y < H - 1 and 'isNOfRiver' in plots[(x, y + 1)])


for (x, y), p in plots.items():
    t = p['TerrainType']
    if t not in terrains:
        err.append('unknown terrain %s at %d,%d' % (t, x, y))
    if water(p) != (t in ('TERRAIN_OCEAN', 'TERRAIN_COAST', 'TERRAIN_SHALLOW_COAST', 'TERRAIN_LAKE', 'TERRAIN_ICE_LAKE', 'TERRAIN_LARGE_RIVERS')):
        err.append('plot type/terrain mismatch at %d,%d' % (x, y))
    f = p.get('f')
    if f:
        fi = finfo.get(f)
        if fi is None:
            err.append('unknown feature %s' % f)
            continue
        if t not in fi['ok'] and not f.startswith('FEATURE_REMARKABLE') and f != 'FEATURE_VOLCANO':
            err.append('%s not allowed on %s at %d,%d' % (f, t, x, y))
        if fi['flat'] and p['PlotType'] != '2':
            err.append('%s needs flat at %d,%d' % (f, x, y))
        if fi['hills'] and p['PlotType'] != '1':
            err.append('%s needs hills at %d,%d' % (f, x, y))
        if fi['river'] and not river_at(x, y):
            err.append('%s needs river at %d,%d' % (f, x, y))
        if p['PlotType'] != '3' and (fi['nocoast'] or fi['onlycoast']):
            coastal = any(plots[(x + dx, y + dy)]['TerrainType'] in ('TERRAIN_OCEAN', 'TERRAIN_COAST', 'TERRAIN_SHALLOW_COAST')
                          for dx in (-1, 0, 1) for dy in (-1, 0, 1) if (x + dx, y + dy) in plots)
            if fi['nocoast'] and coastal:
                err.append('%s not allowed on the coast at %d,%d' % (f, x, y))
            if fi['onlycoast'] and not coastal:
                err.append('%s needs the coast at %d,%d' % (f, x, y))
        if f == 'FEATURE_VOLCANO' and p['PlotType'] != '2':
            err.append('volcano not on flat land at %d,%d' % (x, y))
        if f.startswith('FEATURE_REMARKABLE') and p['PlotType'] != '0':
            err.append('mountain model off peak at %d,%d' % (x, y))
    if 'isWOfRiver' in p and (water(p) or x + 1 >= W or water(plots[(x + 1, y)])):
        err.append('bad W-of-river at %d,%d' % (x, y))
    if 'isNOfRiver' in p and (water(p) or y == 0 or water(plots[(x, y - 1)])):
        err.append('bad N-of-river at %d,%d' % (x, y))
    if 'EuropeType' in p and t != 'TERRAIN_OCEAN':
        err.append('europe on %s at %d,%d' % (t, x, y))

# water regions as the DLL sees them (large rivers excluded)
seen = set()
for start, p in plots.items():
    if start in seen or p['TerrainType'] not in ('TERRAIN_OCEAN', 'TERRAIN_COAST', 'TERRAIN_SHALLOW_COAST', 'TERRAIN_LAKE'):
        continue
    q, reg = deque([start]), []
    seen.add(start)
    while q:
        x, y = q.popleft()
        reg.append((x, y))
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                n = (x + dx, y + dy)
                if n in plots and n not in seen and plots[n]['TerrainType'] in ('TERRAIN_OCEAN', 'TERRAIN_COAST', 'TERRAIN_SHALLOW_COAST', 'TERRAIN_LAKE'):
                    seen.add(n)
                    q.append(n)
    europe = any('EuropeType' in plots[c] for c in reg)
    lakes = sum(1 for c in reg if plots[c]['TerrainType'] == 'TERRAIN_LAKE')
    if lakes and (europe or len(reg) >= 50):
        err.append('lake plots inside a region the game treats as ocean (%d plots, start %s)' % (len(reg), start))
    if not lakes and not europe and len(reg) < 50:
        err.append('sea region of %d plots will become a lake (start %s)' % (len(reg), start))

print('%d plots, %d problems' % (len(plots), len(err)))
for e in err[:40]:
    print(' ', e)
