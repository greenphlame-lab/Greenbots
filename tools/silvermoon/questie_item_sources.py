"""Extract item sources from QuestieDB's Classic item database (GPL, github.com/Questie/QuestieDB).

    python questie_item_sources.py <QuestieDB>/data/Classic/classicItemDB.lua > questie_item_sources.json

Output: {"<item id>": {"npc": [...], "object": [...], "vendor": [...]}} for every item with at least one
source. build_quest_index.py uses it for quest items the world DB has no loot row for (skinning,
pickpocketing, scripted drops, vendors). Only reads the data; nothing from QuestieDB is executed.
"""
import json, re, sys

text = open(sys.argv[1], encoding='utf-8').read()

def parse(s, i):
    """minimal Lua literal parser: tables, strings, numbers, nil, true/false -> (value, next index)"""
    while s[i] in ' \t\r\n,':
        i += 1
    c = s[i]
    if c == '{':
        i += 1
        out = []
        while True:
            while s[i] in ' \t\r\n,':
                i += 1
            if s[i] == '}':
                return out, i + 1
            if s[i] == '[':                       # [key] = value: keep only the value
                j = s.index(']', i)
                i = s.index('=', j) + 1
            v, i = parse(s, i)
            out.append(v)
    if c in '\'"':
        j = i + 1
        buf = []
        while s[j] != c:
            if s[j] == '\\':
                buf.append(s[j + 1]); j += 2
            else:
                buf.append(s[j]); j += 1
        return ''.join(buf), j + 1
    m = re.compile(r'-?\d+(\.\d+)?|nil|true|false').match(s, i)
    tok = m.group(0)
    val = None if tok == 'nil' else True if tok == 'true' else False if tok == 'false' else (float(tok) if '.' in tok else int(tok))
    return val, m.end()

out = {}
for m in re.finditer(r'^\[(\d+)\] = ', text, re.M):
    rec, _ = parse(text, m.end())
    rec += [None] * (15 - len(rec))
    npc, obj, vendor = rec[1] or [], rec[2] or [], rec[13] or []
    if npc or obj or vendor:
        out[m.group(1)] = {'npc': npc, 'object': obj, 'vendor': vendor}
print(json.dumps(out, separators=(',', ':')))
print(len(out), 'items with sources', file=sys.stderr)
