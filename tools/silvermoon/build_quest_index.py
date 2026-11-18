"""Build mangos.silvermoon_quest_index from the world database (brain Phase 2a).

Run from the dashboard container, which has the database credentials:
    cat build_quest_index.py | docker exec -i cmangos-deploy-master-dashboard-1 python - > silvermoon_quest_index.sql
Quest items the world DB has no loot row for (skinning, pickpocketing, scripted drops, vendors) use
QuestieDB's item sources, extracted by questie_item_sources.py into questie_item_sources.json. Copy that
file to /tmp/questie_item_sources.json in the container first; without it those objectives stay unplaced.

One row per quest objective area:
  quest, objective (1-4; 0 = exploration trigger), area (0..n per objective), map, x, y, z, radius,
  min_mob_level, max_mob_level, elite_count, spawn_count, drop_chance (percent, 0 for kills),
  kind ('kill', 'item', 'use', 'explore'), flags, chain_xp, source ('db').
flags: 1 escort/event, 2 timed, 4 group quest, 8 objective with no known source, 16 dungeon only.
source: 'db', 'questie' (spawns found through QuestieDB's item sources) or 'trigger'.
"""
import os, sys, math, collections
import pymysql

AREA_LINK = 80.0          # spawns closer than this belong to one area
MAX_AREAS = 12            # largest areas kept per objective
MIN_DROP = 1.0            # ignore creatures that drop a quest item less than 1% of the time
MAX_DROPPERS = 40         # best droppers kept per item

db = pymysql.connect(host=os.environ['DB_HOST'], user=os.environ['DB_USER'], password=os.environ['DB_PASSWORD'],
                     database=os.environ.get('WORLD_DB', 'mangos'), cursorclass=pymysql.cursors.DictCursor)
cur = db.cursor()
log = lambda *a: print(*a, file=sys.stderr)

cur.execute("""SELECT entry, QuestLevel, MinLevel, Type, SuggestedPlayers, LimitTime, SpecialFlags, QuestFlags,
                      NextQuestInChain, RewMoneyMaxLevel, SrcItemId,
                      ReqCreatureOrGOId1, ReqCreatureOrGOId2, ReqCreatureOrGOId3, ReqCreatureOrGOId4,
                      ReqItemId1, ReqItemId2, ReqItemId3, ReqItemId4
               FROM quest_template
               WHERE entry NOT IN (SELECT quest FROM game_event_quest)""")
quests = {q['entry']: q for q in cur.fetchall()}
log('quests', len(quests))

cur.execute("SELECT Entry, MinLevel, MaxLevel, `Rank`, LootId FROM creature_template")
ctpl = {c['Entry']: c for c in cur.fetchall()}
cur.execute("SELECT entry, type, data1 FROM gameobject_template")
gtpl = {g['entry']: g for g in cur.fetchall()}

cspawn = collections.defaultdict(list)
cur.execute("SELECT id, map, position_x, position_y, position_z FROM creature WHERE map IN (0, 1)")
for r in cur.fetchall():
    cspawn[r['id']].append((r['map'], float(r['position_x']), float(r['position_y']), float(r['position_z'])))
gspawn = collections.defaultdict(list)
cur.execute("SELECT id, map, position_x, position_y, position_z FROM gameobject WHERE map IN (0, 1)")
for r in cur.fetchall():
    gspawn[r['id']].append((r['map'], float(r['position_x']), float(r['position_y']), float(r['position_z'])))

# item -> [(creature or -gameobject, chance)]
lootByItem = collections.defaultdict(list)
cur.execute("SELECT entry, item, ChanceOrQuestChance, mincountOrRef FROM reference_loot_template")
refs = collections.defaultdict(list)
for r in cur.fetchall():
    refs[r['entry']].append(r)
creatureByLoot = collections.defaultdict(list)
for e, c in ctpl.items():
    if c['LootId']:
        creatureByLoot[c['LootId']].append(e)
goByLoot = collections.defaultdict(list)
for e, g in gtpl.items():
    if g['type'] == 3 and g['data1']:
        goByLoot[g['data1']].append(e)

def addLoot(owners, item, chance):
    for o in owners:
        lootByItem[item].append((o, abs(float(chance)) or 100.0))

for table, byLoot, sign in (('creature_loot_template', creatureByLoot, 1), ('gameobject_loot_template', goByLoot, -1)):
    cur.execute(f"SELECT entry, item, ChanceOrQuestChance, mincountOrRef FROM {table}")
    for r in cur.fetchall():
        owners = [sign * o for o in byLoot.get(r['entry'], [])]
        if not owners:
            continue
        if r['mincountOrRef'] < 0:
            for rr in refs.get(-r['mincountOrRef'], []):
                addLoot(owners, rr['item'], rr['ChanceOrQuestChance'])
        else:
            addLoot(owners, r['item'], r['ChanceOrQuestChance'])

cur.execute("SELECT quest, id FROM areatrigger_involvedrelation")
triggers = collections.defaultdict(set)
for r in cur.fetchall():
    triggers[r['quest']].add(r['id'])

try:
    import json
    questie = json.load(open('/tmp/questie_item_sources.json'))
except OSError:
    questie = {}
log('questie items', len(questie))

def cluster(points):
    """single link within AREA_LINK, per map, via a grid"""
    cell = AREA_LINK
    grid = collections.defaultdict(list)
    for i, p in enumerate(points):
        grid[(p[0], int(p[1] // cell), int(p[2] // cell))].append(i)
    parent = list(range(len(points)))
    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for (m, gx, gy), idx in grid.items():
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for j in grid.get((m, gx + dx, gy + dy), ()):
                    for i in idx:
                        if i < j and (points[i][1] - points[j][1]) ** 2 + (points[i][2] - points[j][2]) ** 2 <= AREA_LINK ** 2:
                            parent[find(i)] = find(j)
    groups = collections.defaultdict(list)
    for i in range(len(points)):
        groups[find(i)].append(points[i])
    return sorted(groups.values(), key=len, reverse=True)

def questXp(q):
    return int(q['RewMoneyMaxLevel'] / 0.6) if q['RewMoneyMaxLevel'] else 0

def chainXp(qid, depth=0, seen=None):
    seen = seen or set()
    q = quests.get(qid)
    if not q or depth > 10 or qid in seen:
        return 0
    seen.add(qid)
    nxt = q['NextQuestInChain']
    if not nxt or nxt not in quests:
        return 0
    return questXp(quests[nxt]) + chainXp(nxt, depth + 1, seen)

def questFlags(q):
    f = 0
    if q['SpecialFlags'] & 2 and not triggers.get(q['entry']):
        f |= 1                                    # scripted event or escort
    if q['LimitTime']:
        f |= 2
    if q['Type'] in (1, 21, 41, 62, 81) or (q['SuggestedPlayers'] or 0) > 1:
        f |= 4
    return f

rows = []
unplaced = 0
objectives = 0
for qid, q in quests.items():
    base = questFlags(q)
    cxp = chainXp(qid)
    for o in range(1, 5):
        target = q[f'ReqCreatureOrGOId{o}']
        item = q[f'ReqItemId{o}']
        if not target and not item:
            continue
        if item and not target and item == q['SrcItemId']:
            continue                               # the item is handed over at the start: nothing to fetch
        objectives += 1
        sources = []   # (spawn list, level range, elite, chance, kind)
        if target > 0:
            c = ctpl.get(target)
            sources.append((cspawn.get(target, []), c, 0.0, 'kill'))
        elif target < 0:
            sources.append((gspawn.get(-target, []), None, 0.0, 'use'))
        if item:
            droppers = sorted(lootByItem.get(item, []), key=lambda d: -d[1])[:MAX_DROPPERS]
            for owner, chance in droppers:
                if chance < MIN_DROP:
                    continue
                if owner > 0:
                    sources.append((cspawn.get(owner, []), ctpl.get(owner), chance, 'item'))
                else:
                    sources.append((gspawn.get(-owner, []), None, chance, 'item'))
        source = 'db'
        if item and not any(sp for sp, _, _, _ in sources[1 if target else 0:]) and str(item) in questie:
            qs = questie[str(item)]
            for npc in qs['npc'] + qs['vendor']:
                sources.append((cspawn.get(npc, []), ctpl.get(npc), 0.0, 'item'))
            for obj in qs['object']:
                sources.append((gspawn.get(obj, []), None, 0.0, 'item'))
            source = 'questie'
        points = []
        for spawns, tpl, chance, kind in sources:
            for s in spawns:
                elite = 1 if tpl and tpl['Rank'] in (1, 2, 4) else 0
                points.append((s[0], s[1], s[2], s[3], tpl['MinLevel'] if tpl else 0, tpl['MaxLevel'] if tpl else 0, elite, chance, kind))
        if not points:
            unplaced += 1
            known = (target > 0 and target in ctpl) or (target < 0 and -target in gtpl) or (item and (lootByItem.get(item) or str(item) in questie))
            rows.append((qid, o, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'item' if item else 'kill', base | (16 if known else 8), cxp, source))
            continue
        for a, group in enumerate(cluster(points)[:MAX_AREAS]):
            cx = sum(p[1] for p in group) / len(group); cy = sum(p[2] for p in group) / len(group); cz = sum(p[3] for p in group) / len(group)
            radius = max(math.hypot(p[1] - cx, p[2] - cy) for p in group)
            lv = [p[4] for p in group if p[4]]; hv = [p[5] for p in group if p[5]]
            elites = sum(p[6] for p in group)
            chance = max(p[7] for p in group)
            kind = group[0][8]
            flags = base                           # elites in an area are a risk (elite_count), not a ban
            rows.append((qid, o, a, group[0][0], round(cx, 1), round(cy, 1), round(cz, 1), round(radius, 1),
                         min(lv) if lv else 0, max(hv) if hv else 0, elites, len(group), round(chance, 1), kind, flags, cxp, source))

# exploration objectives: area triggers (coordinates come from AreaTrigger.dbc at load time in the
# server; here the row only records that the quest has one, with source 'trigger')
for qid, ids in triggers.items():
    if qid not in quests:
        continue
    for t in sorted(ids):
        rows.append((qid, 0, t, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'explore', questFlags(quests[qid]), chainXp(qid), 'trigger'))

log('objectives', objectives, 'rows', len(rows), 'objectives with no spawn source', unplaced, 'exploration triggers', sum(len(v) for v in triggers.values()))

print("-- Generated by tools/silvermoon/build_quest_index.py. Do not edit by hand.")
print("""CREATE TABLE IF NOT EXISTS `silvermoon_quest_index` (
  `quest` INT UNSIGNED NOT NULL, `objective` TINYINT UNSIGNED NOT NULL, `area` INT UNSIGNED NOT NULL,
  `map` SMALLINT UNSIGNED NOT NULL, `x` FLOAT NOT NULL, `y` FLOAT NOT NULL, `z` FLOAT NOT NULL, `radius` FLOAT NOT NULL,
  `min_mob_level` TINYINT UNSIGNED NOT NULL, `max_mob_level` TINYINT UNSIGNED NOT NULL, `elite_count` SMALLINT UNSIGNED NOT NULL,
  `spawn_count` SMALLINT UNSIGNED NOT NULL, `drop_chance` FLOAT NOT NULL, `kind` VARCHAR(8) NOT NULL,
  `flags` TINYINT UNSIGNED NOT NULL, `chain_xp` INT UNSIGNED NOT NULL, `source` VARCHAR(8) NOT NULL,
  PRIMARY KEY (`quest`, `objective`, `area`)
);""")
print("DELETE FROM `silvermoon_quest_index`;")
for i in range(0, len(rows), 500):
    chunk = rows[i:i + 500]
    vals = ",".join("(" + ",".join(f"'{v}'" if isinstance(v, str) else str(v) for v in r) + ")" for r in chunk)
    print(f"INSERT INTO `silvermoon_quest_index` VALUES {vals};")
