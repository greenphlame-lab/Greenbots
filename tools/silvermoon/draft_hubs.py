"""Draft quest hub list from quest-giver spawns: cluster givers within 150 yards, keep clusters
with 5+ distinct quests, label the side from the givers' faction templates."""
import json, struct, sys, statistics, collections, math

DBC = sys.argv[1]
givers = json.load(open(sys.argv[2]))

def load(f):
    d = open(f, 'rb').read(); _, n, fc, rs, ss = struct.unpack('<4s4I', d[:20])
    recs = [struct.unpack('<%dI' % fc, d[20 + i * rs:20 + (i + 1) * rs]) for i in range(n)]
    st = d[20 + n * rs:]
    return recs, (lambda o: st[o:st.index(b'\0', o)].decode('latin1'))

areas, astr = load(DBC + '/AreaTable.dbc')
areaName = {a[0]: astr(a[11]) for a in areas}
areaMap = {a[0]: a[1] for a in areas}
fts, _ = load(DBC + '/FactionTemplate.dbc')
ft = {f[0]: f for f in fts}

def side(factionTemplate):
    f = ft.get(int(factionTemplate))
    if not f: return '?'
    our, friend, enemy = f[3], f[4], f[5]
    a_ok = not (enemy & 2); h_ok = not (enemy & 4)
    a_fr = (friend & 2) or (our & 2); h_fr = (friend & 4) or (our & 4)
    if a_fr and not h_fr: return 'A'
    if h_fr and not a_fr: return 'H'
    if a_ok and h_ok: return 'both'
    return '?'

# cluster per map, single link 150 yd
pts = [g for g in givers]
clusters = []
used = [False] * len(pts)
for i, p in enumerate(pts):
    if used[i]: continue
    stack = [i]; used[i] = True; members = []
    while stack:
        k = stack.pop(); members.append(pts[k])
        for j, q in enumerate(pts):
            if not used[j] and q[0] == pts[k][0] and abs(q[1] - pts[k][1]) < 150 and abs(q[2] - pts[k][2]) < 150 and math.hypot(q[1] - pts[k][1], q[2] - pts[k][2]) < 150:
                used[j] = True; stack.append(j)
    clusters.append(members)

hubs = []
for m in clusters:
    quests = {}
    for g in m:
        quests[int(g[6])] = g
    if len(quests) < 5: continue
    levels = sorted(q[7] for q in quests.values())
    lo = levels[int(0.15 * (len(levels) - 1))]; hi = levels[int(0.85 * (len(levels) - 1))]
    zones = collections.Counter(int(q[9]) for q in quests.values() if q[9] > 0 and areaMap.get(int(q[9])) == int(m[0][0]))
    zone = areaName.get(zones.most_common(1)[0][0], '?') if zones else '?'
    sides = collections.Counter(side(g[5]) for g in m)
    s = 'A' if sides['A'] and not sides['H'] else 'H' if sides['H'] and not sides['A'] else 'both' if sides['both'] and not sides['A'] and not sides['H'] else 'mixed'
    if s == 'mixed':
        s = 'A' if sides['A'] > sides['H'] else 'H'
    cx = statistics.mean(g[1] for g in m); cy = statistics.mean(g[2] for g in m)
    giverNames = collections.Counter(g[4] for g in m).most_common(2)
    hubs.append(dict(map=int(m[0][0]), x=round(cx), y=round(cy), zone=zone, side=s, quests=len(quests),
                     lo=int(lo), hi=int(hi), givers=', '.join(n for n, _ in giverNames)))

hubs.sort(key=lambda h: (h['side'], h['lo'], h['zone']))
print('| Side | Zone | Quest levels | Quests | Map, x, y | Main quest givers |')
print('| --- | --- | --- | --- | --- | --- |')
for h in hubs:
    print(f"| {h['side']} | {h['zone']} | {h['lo']}-{h['hi']} | {h['quests']} | {h['map']}, {h['x']}, {h['y']} | {h['givers']} |")
print()
print(collections.Counter(h['side'] for h in hubs), file=sys.stderr)
json.dump(hubs, open(sys.argv[3], 'w'), indent=1)
