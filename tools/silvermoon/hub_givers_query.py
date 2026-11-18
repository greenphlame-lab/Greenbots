import os, pymysql, json
c = pymysql.connect(host=os.environ['DB_HOST'], user=os.environ['DB_USER'], password=os.environ['DB_PASSWORD'], database='mangos')
cur = c.cursor()
cur.execute("""
select c.map, c.position_x, c.position_y, ct.Entry, ct.Name, ct.Faction, q.entry, q.QuestLevel, q.MinLevel, q.ZoneOrSort, q.RequiredRaces
from creature_questrelation r
join creature c on c.id = r.id
join creature_template ct on ct.Entry = r.id
join quest_template q on q.entry = r.quest
where c.map in (0, 1) and q.QuestLevel > 0 and q.entry not in (select quest from game_event_quest) and c.guid not in (select guid from game_event_creature) and (q.QuestFlags & 32) = 0 and q.ZoneOrSort >= 0
""")
print(json.dumps([list(map(lambda v: float(v) if isinstance(v, (int, float)) or hasattr(v, 'as_integer_ratio') else v, r)) for r in cur.fetchall()]))
