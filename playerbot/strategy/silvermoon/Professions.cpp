#include "playerbot/playerbot.h"
#include "Professions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/silvermoon/GoalScorer.h"
#include "playerbot/strategy/values/CraftValues.h"
#include "Spells/SpellMgr.h"
#include "playerbot/silvermoon/Hubs.h"
#include "playerbot/silvermoon/Recovery.h"
#include "Policies.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include <list>
#include <set>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace ai;

namespace
{
    struct Profession { uint32 skill; uint32 learnSpell; uint32 trainerSpell; uint32 fee; bool gather; const char* name; };
    // apprentice spell learned, the trainer's teaching spell (npc_trainer), the trainer's fee in copper
    const Profession professions[] = {
        { 171, 2259, 2275,  10, false, "Alchemy" },
        { 164, 2018, 2020,  10, false, "Blacksmithing" },
        { 333, 7411, 7414,  10, false, "Enchanting" },
        { 202, 4036, 4039,  10, false, "Engineering" },
        { 182, 2366, 2372,  10, true,  "Herbalism" },
        { 165, 2108, 2155,  10, false, "Leatherworking" },
        { 186, 2575, 2581,  10, true,  "Mining" },
        { 393, 8613, 8615,  10, true,  "Skinning" },
        { 197, 3908, 3911,  10, false, "Tailoring" },
        { 356, 7620, 7733, 100, true,  "Fishing" },
        { 185, 2550, 2551, 100, false, "Cooking" },
        { 129, 3273, 3279, 100, false, "First Aid" } };

    Profession const* Find(uint32 skill)
    {
        for (Profession const& p : professions)
            if (p.skill == skill)
                return &p;
        return nullptr;
    }

    struct Orders { std::deque<uint32> learn; uint32 gather = 0; std::set<uint32> level; };
    std::mutex orderLock;
    std::unordered_map<uint32, Orders> orders;

    struct Spawn { uint32 entry, map; float x, y, z; };
    std::unordered_map<uint32, std::vector<Spawn>>& TrainerSpawns()   // trainer spell -> spawns
    {
        static std::once_flag once;
        static std::unordered_map<uint32, std::vector<Spawn>> spawns;
        std::call_once(once, []()
        {
            for (Profession const& p : professions)
                if (auto result = WorldDatabase.PQuery("SELECT c.id, c.map, c.position_x, c.position_y, c.position_z FROM creature c "
                    "JOIN npc_trainer t ON t.entry = c.id WHERE t.spell = %u", p.trainerSpell))
                {
                    do
                    {
                        Field* f = result->Fetch();
                        spawns[p.trainerSpell].push_back({ f[0].GetUInt32(), f[1].GetUInt32(), f[2].GetFloat(), f[3].GetFloat(), f[4].GetFloat() });
                    } while (result->NextRow());
                }
        });
        return spawns;
    }

    bool Friendly(Player* bot, uint32 entry)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        if (!info)
            return false;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->Faction);
        if (!faction)
            return false;
        return bot->GetTeam() == ALLIANCE ? PlayerbotAI::friendToAlliance(faction) : PlayerbotAI::friendToHorde(faction);
    }

    Spawn const* NearestTrainer(Player* bot, Profession const& p)
    {
        Spawn const* best = nullptr;
        float bestDist = 0.0f;
        WorldPosition here(bot);
        for (Spawn const& s : TrainerSpawns()[p.trainerSpell])
        {
            if (s.map != bot->GetMapId() || !Friendly(bot, s.entry))
                continue;
            float d = here.fDist(WorldPosition(s.map, s.x, s.y, s.z));
            if (!best || d < bestDist)
            {
                best = &s;
                bestDist = d;
            }
        }
        return best;
    }

    // drop learn orders that are done or can't be done now (known, too low, no money, no trainer)
    void Tidy(Player* bot, Orders& o)
    {
        while (!o.learn.empty())
        {
            Profession const* p = Find(o.learn.front());
            if (p && !bot->HasSpell(p->learnSpell) && !bot->HasSkill(p->skill) && bot->GetLevel() >= 5 && bot->GetMoney() >= p->fee &&
                NearestTrainer(bot, *p))
                break;
            o.learn.pop_front();
        }
        if (o.gather && !bot->HasSkill(o.gather))
            o.gather = 0;
    }
}

bool ai::SilvermoonProfCommand(Player* bot, std::string const& param, std::string& reply)
{
    char verb[16] = {};
    uint32 skill = 0;
    int n = sscanf(param.c_str(), "%15s %u", verb, &skill);
    std::string v = n >= 1 ? verb : "";
    std::lock_guard<std::mutex> guard(orderLock);
    Orders& o = orders[bot->GetGUIDLow()];
    if (v == "learn" && n == 2)
    {
        Profession const* p = Find(skill);
        if (!p)
            return false;
        if (bot->GetLevel() < 5)
        {
            reply = "I need to be level 5 to learn a profession.";
            return true;
        }
        if (bot->HasSkill(p->skill))
        {
            reply = std::string("I already know ") + p->name + ".";
            return true;
        }
        if (bot->GetMoney() < p->fee)
        {
            reply = std::string("I can't afford to learn ") + p->name + " right now.";
            return true;
        }
        if (std::find(o.learn.begin(), o.learn.end(), skill) == o.learn.end())
            o.learn.push_back(skill);
        reply = std::string("Off to learn ") + p->name + ".";
        return true;
    }
    if (v == "gather" && n == 2)
    {
        Profession const* p = Find(skill);
        if (!p || !p->gather)
            return false;
        if (!bot->HasSkill(p->skill))
        {
            reply = std::string("I don't know ") + p->name + " yet.";
            return true;
        }
        o.gather = skill;
        reply = std::string("Gathering with ") + p->name + " until you stop me.";
        return true;
    }
    if ((v == "level" || v == "unlevel") && n == 2)
    {
        Profession const* p = Find(skill);
        if (!p)
            return false;
        if (v == "unlevel")
        {
            o.level.erase(skill);
            reply = std::string("I'll stop levelling ") + p->name + ".";
            return true;
        }
        if (!bot->HasSkill(p->skill))
        {
            reply = std::string("I don't know ") + p->name + " yet.";
            return true;
        }
        o.level.insert(skill);
        reply = std::string("Levelling ") + p->name + ": I'll craft what still gives skill and buy vendor materials.";
        return true;
    }
    if (v == "unlearn" && n == 2)
    {
        // S: unlearn from the professions pane - drops every spell of the skill line (ranks, recipes,
        // Find Herbs etc.) and the skill itself, and clears any orders for it
        Profession const* p = Find(skill);
        if (!p)
            return false;
        if (!bot->HasSkill(p->skill))
        {
            reply = std::string("I don't know ") + p->name + ".";
            return true;
        }
        std::vector<uint32> spells;
        for (uint32 j = 0; j < sSkillLineAbilityStore.GetNumRows(); ++j)
            if (SkillLineAbilityEntry const* e = sSkillLineAbilityStore.LookupEntry(j))
                if (e->skillId == p->skill && bot->HasSpell(e->spellId))
                    spells.push_back(e->spellId);
        if (p->learnSpell && bot->HasSpell(p->learnSpell))
            spells.push_back(p->learnSpell);
        for (uint32 spellId : spells)
            bot->removeSpell(spellId, false, false);
        bot->SetSkill(p->skill, 0, 0, 0);
        o.learn.erase(std::remove(o.learn.begin(), o.learn.end(), skill), o.learn.end());
        if (o.gather == skill)
            o.gather = 0;
        o.level.erase(skill);
        reply = std::string("Unlearned ") + p->name + ".";
        return true;
    }
    if (v == "stop")
    {
        if (n == 2)
        {
            o.learn.erase(std::remove(o.learn.begin(), o.learn.end(), skill), o.learn.end());
            if (o.gather == skill)
                o.gather = 0;
            o.level.erase(skill);
        }
        else
            o = Orders();
        reply = "Profession orders cleared.";
        return true;
    }
    return false;
}

bool ai::SilvermoonProfessionGoal(Player* bot, PlayerbotAI* ai, SilvermoonScoredGoal& goal)
{
    Profession const* p = nullptr;
    Spawn const* trainer = nullptr;
    {
        std::lock_guard<std::mutex> guard(orderLock);
        auto it = orders.find(bot->GetGUIDLow());
        if (it == orders.end())
            return false;
        Tidy(bot, it->second);
        if (it->second.learn.empty())
            return false;
        p = Find(it->second.learn.front());
    }
    if (!p || !(trainer = NearestTrainer(bot, *p)))
        return false;
    WorldPosition at(trainer->map, trainer->x, trainer->y, trainer->z);
    TemporaryTravelDestination* destination = new TemporaryTravelDestination(at, int32(trainer->entry));
    goal.destination = destination;
    goal.position = destination->GetPosition();
    goal.kind = silvermoon::GoalKind::Trainer;
    goal.score.value = 1000000.0f;
    goal.score.minutes = 1.0f;
    goal.score.score = 1000000.0f;
    CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(trainer->entry);
    goal.score.reason = std::string("profession: learn ") + p->name + (info ? std::string(" at ") + info->Name : "");
    return true;
}

namespace
{
    // every herb and ore spawn point on the two continents, with the node's skill and the level it
    // needs (from the chest's lock), loaded once
    struct NodeSpot { uint32 map; float x, y, z; uint32 skill; uint32 req; };
    std::vector<NodeSpot>& NodeSpots()
    {
        static std::once_flag once;
        static std::vector<NodeSpot> spots;
        std::call_once(once, []()
        {
            if (auto r = WorldDatabase.Query("SELECT g.map, g.position_x, g.position_y, g.position_z, t.data0 FROM gameobject g "
                "JOIN gameobject_template t ON t.entry = g.id WHERE t.type = 3 AND g.map IN (0, 1)"))
            {
                do
                {
                    Field* f = r->Fetch();
                    LockEntry const* lock = sLockStore.LookupEntry(f[4].GetUInt32());
                    if (!lock)
                        continue;
                    for (int i = 0; i < 8; ++i)
                    {
                        if (lock->Type[i] != LOCK_KEY_SKILL)
                            continue;
                        uint32 skill = SkillByLockType(LockType(lock->Index[i]));
                        if (skill == SKILL_HERBALISM || skill == SKILL_MINING)
                            spots.push_back({ f[0].GetUInt32(), f[1].GetFloat(), f[2].GetFloat(), f[3].GetFloat(), skill, lock->Skill[i] });
                        break;
                    }
                } while (r->NextRow());
            }
            sLog.outBasic("Silvermoon: gather routes know %u herb and ore spawn points", uint32(spots.size()));
        });
        return spots;
    }
    struct GatherRoute { uint32 zone = 0, skill = 0, skillValue = 0; std::vector<WorldPosition> points; size_t next = 0; time_t built = 0; bool remote = false; };
    // a node still raises the skill until the skill is 100 over the node's requirement (grey)
    bool GivesSkill(NodeSpot const& s, uint32 value) { return s.req <= value && value < s.req + 100; }

    std::mutex routeLock;
    std::unordered_map<uint32, GatherRoute> routes;
}

// profession catch-up (S, 1.6.21): a random bot whose gathering skill is far behind its level gets a
// gather session of its own, the same as a Gather order from the panel, until a set time
namespace
{
    // 1.6.22 (S: "more calculated skill-ups"): the clock starts on arrival at the gathering area; the
    // session then runs until the skill reaches its target (the tier its home area needs, or the
    // milestone) or 2 hours there, at most 2.5 hours in all
    struct CatchUp { uint32 skill = 0, target = 0; time_t until = 0, hardUntil = 0; bool arrived = false; };
    std::unordered_map<uint32, CatchUp> catchUp;        // orderLock
    uint32 CatchUpSkillLocked(uint32 guid)
    {
        auto it = catchUp.find(guid);
        if (it == catchUp.end())
            return 0;
        time_t now = time(nullptr);
        if (now >= it->second.until || now >= it->second.hardUntil)
        {
            catchUp.erase(it);
            return 0;
        }
        return it->second.skill;
    }
}

void ai::SilvermoonStartCatchUp(uint32 guid, uint32 skill, uint32 target)
{
    std::lock_guard<std::mutex> guard(orderLock);
    CatchUp c;
    c.skill = skill;
    c.target = target;
    c.until = time(nullptr) + 45 * MINUTE;              // to get there; the 30 minutes start on arrival
    c.hardUntil = time(nullptr) + 75 * MINUTE;
    catchUp[guid] = c;
}

bool ai::SilvermoonCatchUpArrived(uint32 guid)
{
    std::lock_guard<std::mutex> guard(orderLock);
    auto it = catchUp.find(guid);
    if (it == catchUp.end() || it->second.arrived)
        return false;
    it->second.arrived = true;
    it->second.until = std::min(it->second.hardUntil, time(nullptr) + 30 * MINUTE);   // S: a 25-30 minute session, once there
    return true;
}

uint32 ai::SilvermoonCatchUpTarget(uint32 guid)
{
    std::lock_guard<std::mutex> guard(orderLock);
    return CatchUpSkillLocked(guid) ? catchUp[guid].target : 0;
}

void ai::SilvermoonEndCatchUp(uint32 guid)
{
    std::lock_guard<std::mutex> guard(orderLock);
    catchUp.erase(guid);
}

bool ai::SilvermoonGatherRouteRemote(uint32 guid)
{
    std::lock_guard<std::mutex> guard(routeLock);
    auto it = routes.find(guid);
    // no route yet counts as "not there yet"
    return it == routes.end() || it->second.points.empty() || it->second.remote;
}

uint32 ai::SilvermoonHomeNodeReq(Player* bot, uint32 skill, uint32 value)
{
    // the smallest requirement above its skill among the nodes around it (1,200 yards): the tier it
    // has to reach to gather where it is
    uint32 best = 0;
    for (NodeSpot const& s : NodeSpots())
        if (s.skill == skill && s.map == bot->GetMapId() && s.req > value &&
            (s.x - bot->GetPositionX()) * (s.x - bot->GetPositionX()) + (s.y - bot->GetPositionY()) * (s.y - bot->GetPositionY()) < 1200.0f * 1200.0f &&
            (!best || s.req < best))
            best = s.req;
    return best;
}

uint32 ai::SilvermoonCatchUpSkill(uint32 guid)
{
    std::lock_guard<std::mutex> guard(orderLock);
    return CatchUpSkillLocked(guid);
}

bool ai::SilvermoonGatherRouteNext(Player* bot, WorldPosition& next, uint32& index, uint32& total)
{
    uint32 skill = 0;
    {
        std::lock_guard<std::mutex> guard(orderLock);
        auto it = orders.find(bot->GetGUIDLow());
        if (it != orders.end() && (it->second.gather == SKILL_HERBALISM || it->second.gather == SKILL_MINING))
            skill = it->second.gather;
        else
            skill = CatchUpSkillLocked(bot->GetGUIDLow());
        if (skill != SKILL_HERBALISM && skill != SKILL_MINING)
            return false;
    }
    uint32 value = bot->GetSkillValue(skill);
    uint32 zone = bot->GetZoneId();
    std::lock_guard<std::mutex> guard(routeLock);
    GatherRoute& route = routes[bot->GetGUIDLow()];
    if (route.remote && route.zone == zone)
        route.remote = false;                           // arrived in the zone it was sent to
    // (re)build in a new zone (unless on the way to the one it chose), for another skill, every 25
    // skill points (new nodes open up) or hourly
    if (route.points.empty() || (route.zone != zone && !route.remote) || route.skill != skill || value >= route.skillValue + 25 ||
        time(nullptr) - route.built > HOUR)
    {
        // where (S, 1.6.21: "go to the zone with level-appropriate gather nodes"): here, when 8+ nodes
        // within 1,200 yards still raise the skill; otherwise around the friendly quest hub on this
        // continent with the most such nodes within 1,200 yards (a hub the bot isn't under-levelled for,
        // in a zone under 40 player kills an hour), nearer hubs counting more. Distances only - no
        // terrain lookups for the thousands of spawn points.
        bool alliance = bot->GetTeam() == ALLIANCE;
        WorldPosition here(bot);
        auto countNear = [&](float x, float y) -> uint32
        {
            uint32 n = 0;
            for (NodeSpot const& s : NodeSpots())
                if (s.skill == skill && s.map == bot->GetMapId() && GivesSkill(s, value) &&
                    (s.x - x) * (s.x - x) + (s.y - y) * (s.y - y) < 1200.0f * 1200.0f)
                    ++n;
            return n;
        };
        uint32 usefulHere = countNear(here.getX(), here.getY());
        float centerX = here.getX(), centerY = here.getY();
        bool remote = false;
        bool onlyUseful = usefulHere >= 8;
        if (!onlyUseful)
        {
            float bestScore = 0.0f;
            silvermoon::Hub const* best = nullptr;
            uint32 bestCount = 0;
            for (silvermoon::Hub const& h : silvermoon::Hubs::All())
            {
                if (h.map != bot->GetMapId() || !(h.side == 3 || h.side == (alliance ? 1 : 2)))
                    continue;
                if (int32(h.minLevel) > int32(bot->GetLevel()) + 2)
                    continue;                           // mobs there are too strong for it
                if (!SilvermoonZoneSafeFor(bot->GetGUIDLow(), sTerrainMgr.GetZoneId(h.map, h.x, h.y, h.z)))
                    continue;
                uint32 n = countNear(h.x, h.y);
                if (n < 8)
                    continue;
                float d = here.fDist(WorldPosition(h.map, h.x, h.y, h.z));
                // nearer areas count much more (1.6.22): half the score at 800 yards, a fifth at 1,600
                float score = float(n) / (1.0f + (d / 800.0f) * (d / 800.0f));
                if (score > bestScore)
                {
                    bestScore = score;
                    best = &h;
                    bestCount = n;
                }
            }
            if (best)
            {
                centerX = best->x;
                centerY = best->y;
                remote = true;
                onlyUseful = true;
                zone = sTerrainMgr.GetZoneId(best->map, best->x, best->y, best->z);
                sLog.outBasic("Silvermoon: %s gathers skill %u (%u) around %s: %u nodes there still raise it, %u here",
                    bot->GetName(), skill, value, best->name, bestCount, usefulHere);
            }
        }
        route = GatherRoute();
        route.zone = zone;
        route.skill = skill;
        route.skillValue = value;
        route.built = time(nullptr);
        route.remote = remote && zone != bot->GetZoneId();
        std::vector<WorldPosition> pool;
        for (NodeSpot const& s : NodeSpots())
        {
            // only nodes its skill can gather (and, where there are enough, only ones that still raise it)
            if (s.skill != skill || s.req > value || s.map != bot->GetMapId())
                continue;
            if (onlyUseful && !GivesSkill(s, value))
                continue;
            float dx = s.x - centerX, dy = s.y - centerY;
            if (remote ? dx * dx + dy * dy > 1200.0f * 1200.0f : dx * dx + dy * dy > 4000.0f * 4000.0f)
                continue;
            if (!remote && sTerrainMgr.GetZoneId(s.map, s.x, s.y, s.z) != zone)
                continue;
            WorldPosition p(s.map, s.x, s.y, s.z);
            bool dup = false;
            for (WorldPosition const& q : pool)
                if (q.fDist(p) < 20.0f)
                {
                    dup = true;
                    break;
                }
            if (!dup)
                pool.push_back(p);
        }
        // one loop: nearest neighbour from the bot, each point once
        WorldPosition at = here;
        while (!pool.empty())
        {
            size_t best = 0;
            float bestD = at.fDist(pool[0]);
            for (size_t i = 1; i < pool.size(); ++i)
            {
                float d = at.fDist(pool[i]);
                if (d < bestD)
                {
                    best = i;
                    bestD = d;
                }
            }
            at = pool[best];
            route.points.push_back(at);
            pool[best] = pool.back();
            pool.pop_back();
        }
        sLog.outBasic("Silvermoon: %s gather route in zone %u: %u spawn points (skill %u at %u)", bot->GetName(), zone,
            uint32(route.points.size()), skill, value);
        if (route.points.empty())
            return false;
    }
    // the next point, passing over spots where the bot died twice in the last hour
    for (size_t tries = 0; tries < route.points.size(); ++tries)
    {
        size_t at = route.next % route.points.size();
        WorldPosition const p = route.points[at];
        route.next = (route.next + 1) % route.points.size();
        if (SilvermoonDeathsNearPoint(bot->GetGUIDLow(), p.getMapId(), p.getX(), p.getY(), 100.0f, HOUR) >= 2)
            continue;
        next = p;
        index = uint32(at + 1);
        total = uint32(route.points.size());
        return true;
    }
    return false;
}

float ai::SilvermoonGatherOrderFactor(uint32 guid, uint32 skill)
{
    std::lock_guard<std::mutex> guard(orderLock);
    auto it = orders.find(guid);
    if (it == orders.end() || !it->second.gather)
        return CatchUpSkillLocked(guid) == skill ? 20.0f : 1.0f;
    return it->second.gather == skill ? 20.0f : 1.0f;
}

std::string ai::SilvermoonProfPanel(Player* bot)
{
    std::string out = "SMPROF\t";
    std::lock_guard<std::mutex> guard(orderLock);
    Orders const* o = nullptr;
    auto it = orders.find(bot->GetGUIDLow());
    if (it != orders.end())
        o = &it->second;
    bool first = true;
    for (Profession const& p : professions)
    {
        char buf[48];
        char state = '-';
        if (o && std::find(o->learn.begin(), o->learn.end(), p.skill) != o->learn.end())
            state = 'L';
        if (o && o->gather == p.skill)
            state = 'G';
        snprintf(buf, sizeof(buf), "%s%u:%u:%u:%c:%u", first ? "" : ";", p.skill, bot->HasSkill(p.skill) ? uint32(bot->GetSkillValue(p.skill)) : 0,
            bot->HasSkill(p.skill) ? uint32(bot->GetSkillMax(p.skill)) : 0, state, (o && o->level.count(p.skill)) ? 1u : 0u);
        out += buf;
        first = false;
    }
    char lvl[16];
    snprintf(lvl, sizeof(lvl), ";L%u", bot->GetLevel());
    out += lvl;
    return out;
}

namespace
{
    uint32 ToolFor(uint32 skill)
    {
        switch (skill)
        {
            case 186: return 2901;   // Mining Pick
            case 393: return 7005;   // Skinning Knife
            case 356: return 6256;   // Fishing Pole
            default: return 0;
        }
    }

    bool BuyTool(Player* bot, uint32 skill)
    {
        uint32 tool = ToolFor(skill);
        if (!tool || !bot->HasSkill(skill) || bot->HasItemCount(tool, 1, true))
            return false;
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(tool);
        if (!proto || bot->GetMoney() < proto->BuyPrice)
            return false;
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, tool, 1) != EQUIP_ERR_OK)
            return false;
        bot->StoreNewItem(dest, tool, true);
        bot->ModifyMoney(-int32(proto->BuyPrice));
        sLog.outBasic("Silvermoon: %s bought a %s for %u copper (profession tool)", bot->GetName(), proto->Name1, proto->BuyPrice);
        return true;
    }
}

void ai::SilvermoonProfBuyTools(Player* bot)
{
    if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || bot->IsInCombat())
        return;                                   // in a town or inn, like a vendor visit
    for (uint32 skill : { 186u, 393u, 356u })
        BuyTool(bot, skill);
}

namespace
{
    struct GoSpot { uint32 flag; uint32 entry, map; float x, y, z; };
    std::vector<GoSpot>& GoSpots()
    {
        static std::once_flag once;
        static std::vector<GoSpot> spots;
        std::call_once(once, []()
        {
            const uint32 wanted = UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR |
                UNIT_NPC_FLAG_TRAINER;   // 1.6.22: profession trainers (rank trips)
            if (auto r = WorldDatabase.PQuery("SELECT t.NpcFlags, c.id, c.map, c.position_x, c.position_y, c.position_z FROM creature c "
                "JOIN creature_template t ON t.entry = c.id WHERE (t.NpcFlags & %u) <> 0 AND c.map IN (0, 1)", wanted))
            {
                do
                {
                    Field* f = r->Fetch();
                    spots.push_back({ f[0].GetUInt32(), f[1].GetUInt32(), f[2].GetUInt32(), f[3].GetFloat(), f[4].GetFloat(), f[5].GetFloat() });
                } while (r->NextRow());
            }
        });
        return spots;
    }
    // entries: only these NPCs; kind: the panel's name for a Go to order you gave ("ah"), empty for
    // the brain's own trips; arrived: when it got there (a queued order waits a little there first)
    struct GoOrder { uint32 flag = 0; std::string what; std::set<uint32> entries; time_t at = 0; std::string kind; time_t arrived = 0; };
    std::unordered_map<uint32, GoOrder> goOrders;
    // Go to queue (S, 1.7): the places after the current one, in order
    std::unordered_map<uint32, std::deque<GoOrder>> goQueue;
    const size_t GO_QUEUE_MAX = 8;

    // seconds a character stays at a stop before heading to the next queued one, so it gets its
    // business done there (selling, posting auctions, banking)
    uint32 GoDwell(GoOrder const& o)
    {
        if (o.flag & (UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_VENDOR))
            return 45;
        if (o.flag & UNIT_NPC_FLAG_REPAIR)
            return 10;
        return 0;
    }
}

// "SMGO	<current place's panel name or what>;<queued>^<queued>..." for the bag window's queue
std::string ai::SilvermoonGoQueueText(Player* bot)
{
    std::lock_guard<std::mutex> guard(orderLock);
    std::string text = "SMGO	";
    auto it = goOrders.find(bot->GetGUIDLow());
    if (it != goOrders.end())
        text += it->second.kind.empty() ? "~" + it->second.what : it->second.kind;
    text += ";";
    auto q = goQueue.find(bot->GetGUIDLow());
    if (q != goQueue.end())
        for (size_t i = 0; i < q->second.size(); ++i)
            text += (i ? "^" : "") + q->second[i].kind;
    return text;
}

bool ai::SilvermoonGoCommand(Player* bot, std::string const& param, std::string& reply)
{
    static const std::map<std::string, std::pair<uint32, std::string>> kinds = {
        { "inn", { UNIT_NPC_FLAG_INNKEEPER, "the inn" } }, { "flight", { UNIT_NPC_FLAG_FLIGHTMASTER, "the flight master" } },
        { "ah", { UNIT_NPC_FLAG_AUCTIONEER, "the auction house" } }, { "bank", { UNIT_NPC_FLAG_BANKER, "the bank" } },
        { "vendor", { UNIT_NPC_FLAG_VENDOR, "a vendor" } }, { "repair", { UNIT_NPC_FLAG_REPAIR, "a repair vendor" } } };
    std::lock_guard<std::mutex> guard(orderLock);
    uint32 guid = bot->GetGUIDLow();
    if (param == "stop")
    {
        goOrders.erase(guid);
        goQueue.erase(guid);
        reply = "Go-to cancelled.";
        return true;
    }
    // queue (S, 1.7): "clear" empties the queue, "-N" drops its Nth stop (1 = next), "+place" adds one
    if (param == "clear")
    {
        goQueue.erase(guid);
        reply = "Go-to queue cleared.";
        return true;
    }
    if (param.size() > 1 && param[0] == '-')
    {
        size_t n = size_t(atoi(param.c_str() + 1));
        auto q = goQueue.find(guid);
        if (!n || q == goQueue.end() || n > q->second.size())
            return false;
        q->second.erase(q->second.begin() + (n - 1));
        reply = "Removed stop " + std::to_string(n) + " from the queue.";
        return true;
    }
    bool add = !param.empty() && param[0] == '+';
    auto k = kinds.find(add ? param.substr(1) : param);
    if (k == kinds.end())
        return false;
    GoOrder order;
    order.flag = k->second.first;
    order.what = k->second.second;
    order.at = time(nullptr);
    order.kind = k->first;
    auto cur = goOrders.find(guid);
    if (add && cur != goOrders.end() && !cur->second.kind.empty())
    {
        std::deque<GoOrder>& q = goQueue[guid];
        if (q.size() >= GO_QUEUE_MAX)
        {
            reply = "The queue is full.";
            return false;
        }
        q.push_back(order);
        reply = "Then " + order.what + " (" + std::to_string(q.size()) + " queued).";
        return true;
    }
    goOrders[guid] = order;
    if (!add)
        goQueue.erase(guid);        // a plain Go to replaces the whole plan
    reply = "Heading to " + order.what + ".";
    return true;
}

namespace
{
    // NPC entries that sell this item (their own list or their vendor template), cached
    std::set<uint32> const& VendorsSelling(uint32 itemId)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, std::set<uint32>> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(itemId);
        if (it != cache.end())
            return it->second;
        std::set<uint32>& out = cache[itemId];
        if (auto r = WorldDatabase.PQuery("SELECT entry FROM npc_vendor WHERE item = %u UNION "
            "SELECT ct.entry FROM npc_vendor_template vt JOIN creature_template ct ON ct.VendorTemplateId = vt.entry WHERE vt.item = %u", itemId, itemId))
        {
            do { out.insert(r->Fetch()[0].GetUInt32()); } while (r->NextRow());
        }
        return out;
    }

    uint32 SkillOfSpell(uint32 spellId)
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spellId);
        for (auto i = bounds.first; i != bounds.second; ++i)
            if (i->second->skillId)
                return i->second->skillId;
        return 0;
    }
}

void ai::SilvermoonLevelPass(Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai || !bot->IsAlive() || bot->IsInCombat() || bot->IsTaxiFlying())
        return;
    std::set<uint32> skills;
    bool goingSomewhere = false;
    {
        std::lock_guard<std::mutex> guard(orderLock);
        auto it = orders.find(bot->GetGUIDLow());
        if (it == orders.end() || it->second.level.empty())
            return;
        skills = it->second.level;
        goingSomewhere = goOrders.count(bot->GetGUIDLow()) > 0;
    }
    AiObjectContext* context = ai->GetAiObjectContext();
    std::vector<uint32> spells = context->GetValue<std::vector<uint32>>("craft spells")->Get();
    uint32 crafted = 0;
    uint32 wantItem = 0;          // a vendor material it still needs
    for (uint32 spellId : spells)
    {
        if (crafted >= 5)
            break;
        uint32 skill = SkillOfSpell(spellId);
        if (!skill || !skills.count(skill) || !ShouldCraftSpellValue::SpellGivesSkillUp(spellId, bot))
            continue;
        SpellEntry const* spell = sServerFacade.LookupSpellInfo(spellId);
        if (!spell)
            continue;
        uint32 itemId = spell->Effect[0] == SPELL_EFFECT_CREATE_ITEM ? spell->EffectItemType[0] : 0;
        // materials: what's missing, and whether a vendor sells all of it
        std::map<uint32, uint32> missing;
        bool any = false;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i])
                continue;
            any = true;
            uint32 have = bot->GetItemCount(uint32(spell->Reagent[i]));
            if (have < spell->ReagentCount[i])
                missing[uint32(spell->Reagent[i])] = spell->ReagentCount[i] - have;
        }
        if (!any)
            continue;
        if (!missing.empty())
        {
            // buy it from a vendor right here, if one sells it and the money allows (a quarter of it)
            bool allBought = true;
            for (auto const& need : missing)
            {
                ItemPrototype const* rp = sObjectMgr.GetItemPrototype(need.first);
                std::set<uint32> const& sellers = VendorsSelling(need.first);
                if (!rp || sellers.empty())
                {
                    allBought = false;      // not sold by vendors: it has to find or loot it
                    continue;
                }
                Creature* seller = nullptr;
                for (ObjectGuid const& g : context->GetValue<std::list<ObjectGuid>>("nearest npcs")->Get())
                    if (Creature* c = bot->GetMap()->GetCreature(g))
                        if (sellers.count(c->GetEntry()) && !c->IsEnemy(bot) && bot->GetDistance(c) < 30.0f)
                            seller = c;
                if (!seller)
                {
                    allBought = false;
                    if (!wantItem)
                        wantItem = need.first;
                    continue;
                }
                uint32 buyCount = rp->BuyCount ? rp->BuyCount : 1;
                uint32 lots = (need.second + buyCount - 1) / buyCount;
                uint32 cost = rp->BuyPrice * lots;
                ItemPosCountVec dest;
                if (cost * 4 > bot->GetMoney() || bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, need.first, lots * buyCount) != EQUIP_ERR_OK)
                {
                    allBought = false;
                    continue;
                }
                bot->ModifyMoney(-int32(cost));
                bot->StoreNewItem(dest, need.first, true);
                sLog.outBasic("Silvermoon: %s bought %u x %s from %s to level a profession", bot->GetName(), lots * buyCount, rp->Name1, seller->GetName());
            }
            if (!allBought)
                continue;
        }
        if (!itemId)
            continue;
        int32 made = spell->CalculateSimpleValue(EFFECT_INDEX_0);
        uint32 count = uint32(std::max(1, std::min(made, 20)));
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK)
            continue;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (spell->Reagent[i] > 0 && spell->ReagentCount[i])
                bot->DestroyItemCount(uint32(spell->Reagent[i]), spell->ReagentCount[i], true);
        bot->StoreNewItem(dest, itemId, true, Item::GenerateItemRandomPropertyId(itemId));
        bot->UpdateCraftSkill(spellId);
        ++crafted;
        if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId))
            sLog.outBasic("Silvermoon: %s crafted %s to level a profession", bot->GetName(), proto->Name1);
    }
    // nothing to craft right now and a vendor material is missing: go to a vendor that sells it
    if (!crafted && wantItem && !goingSomewhere)
    {
        ItemPrototype const* rp = sObjectMgr.GetItemPrototype(wantItem);
        std::lock_guard<std::mutex> guard(orderLock);
        GoOrder& order = goOrders[bot->GetGUIDLow()];
        order.flag = UNIT_NPC_FLAG_VENDOR;
        order.what = std::string("a vendor for ") + (rp ? rp->Name1 : "materials");
        order.entries = VendorsSelling(wantItem);
        order.at = time(nullptr);
    }
}

// Profession ranks (S, 1.6.22: "hit the threshold, and the #1 priority is to go train the next level,
// as long as level and money permit"): every rank the profession trainers teach - the skill, the skill
// and level it needs, its price, the spell the trainer casts and the one learned - and which trainers
// (creature entries) teach it. Built once.
namespace
{
    struct SmRankInfo { uint32 skill = 0, reqSkill = 0, reqLevel = 0, cost = 0, learned = 0, teach = 0; std::set<uint32> trainers; };
    std::vector<SmRankInfo>& Ranks()
    {
        static std::once_flag once;
        static std::vector<SmRankInfo> ranks;
        std::call_once(once, []()
        {
            std::map<uint32, SmRankInfo> byLearned;
            for (uint32 id = 0; id < sCreatureStorage.GetMaxEntry(); ++id)
            {
                CreatureInfo const* co = sCreatureStorage.LookupEntry<CreatureInfo>(id);
                if (!co || co->TrainerType != TRAINER_TYPE_TRADESKILLS)
                    continue;
                for (TrainerSpellData const* data : { sObjectMgr.GetNpcTrainerSpells(co->Entry), sObjectMgr.GetNpcTrainerTemplateSpells(co->TrainerTemplateId) })
                {
                    if (!data)
                        continue;
                    for (auto const& itr : data->spellList)
                    {
                        TrainerSpell const& t = itr.second;
                        if (!t.reqSkill || !t.reqSkillValue)
                            continue;           // apprentice ranks (learning the profession) are done elsewhere
                        uint32 learned = t.learnedSpell;
                        bool step = false;
                        SpellEntry const* teach = sServerFacade.LookupSpellInfo(t.spell);
                        if (teach)
                            for (int j = 0; j < 3; ++j)
                            {
                                if (teach->Effect[j] == SPELL_EFFECT_LEARN_SPELL && teach->EffectTriggerSpell[j])
                                    learned = teach->EffectTriggerSpell[j];
                                // 1.7 fix (S: Mommy at Herbalism 51 with 16s never went to train): in
                                // classic the skill step sits on the trainer's teaching spell (2373
                                // teaches Journeyman Herbalism 2368 AND raises the cap), not on the
                                // learned spell - so no rank was ever found
                                if (teach->Effect[j] == SPELL_EFFECT_SKILL_STEP)
                                    step = true;
                            }
                        SpellEntry const* info = learned ? sServerFacade.LookupSpellInfo(learned) : nullptr;
                        if (!info)
                            continue;
                        for (int j = 0; j < 3; ++j)
                            if (info->Effect[j] == SPELL_EFFECT_SKILL_STEP)
                                step = true;
                        if (!step)
                            continue;           // recipes aren't ranks
                        SmRankInfo& r = byLearned[learned];
                        if (r.trainers.empty() || t.spellCost < r.cost)
                        {
                            r.skill = t.reqSkill;
                            r.reqSkill = t.reqSkillValue;
                            r.reqLevel = t.reqLevel;
                            r.cost = t.spellCost;
                            r.learned = learned;
                            r.teach = teach ? t.spell : 0;
                        }
                        r.trainers.insert(co->Entry);
                    }
                }
            }
            for (auto const& e : byLearned)
                ranks.push_back(e.second);
        });
        return ranks;
    }

    // the rank this character can train now: its skill reached the requirement, its level allows,
    // it hasn't got it, and the price fits its money above the repair fund
    SmRankInfo const* RankDue(Player* bot)
    {
        SmRankInfo const* best = nullptr;
        for (SmRankInfo const& r : Ranks())
        {
            if (!bot->HasSkill(r.skill) || bot->GetSkillValuePure(r.skill) < r.reqSkill || bot->GetLevel() < r.reqLevel || bot->HasSpell(r.learned))
                continue;
            if (r.cost > SilvermoonSpendable(bot))
                continue;
            if (!best || r.reqSkill < best->reqSkill)
                best = &r;
        }
        return best;
    }
}

bool ai::SilvermoonGoTrainRank(Player* bot)
{
    SmRankInfo const* r = RankDue(bot);
    if (!r)
        return false;
    SpellEntry const* info = sServerFacade.LookupSpellInfo(r->learned);
    std::lock_guard<std::mutex> guard(orderLock);
    if (goOrders.count(bot->GetGUIDLow()))
        return false;                                   // already going somewhere
    GoOrder& order = goOrders[bot->GetGUIDLow()];
    order.flag = UNIT_NPC_FLAG_TRAINER;
    order.what = std::string("a trainer for ") + (info ? info->SpellName[0] : "the next rank");
    order.entries = r->trainers;
    order.at = time(nullptr);
    return true;
}

bool ai::SilvermoonLearnRankHere(Player* bot)
{
    SmRankInfo const* r = RankDue(bot);
    if (!r)
        return false;
    // a trainer who teaches it within 20 yards
    std::list<Unit*> near;
    MaNGOS::AnyUnitInObjectRangeCheck check(bot, 20.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
    Cell::VisitAllObjects(bot, searcher, 20.0f);
    for (Unit* u : near)
        if (u->GetTypeId() == TYPEID_UNIT && u->IsAlive() && r->trainers.count(u->GetEntry()) && !bot->CanAttack(u))
        {
            bot->ModifyMoney(-int32(r->cost));
            // the trainer's teaching spell does both parts: learns the rank and raises the skill cap
            if (r->teach)
                u->CastSpell(bot, r->teach, TRIGGERED_OLD_TRIGGERED);   // as the trainer does
            if (!bot->HasSpell(r->learned))
                bot->learnSpell(r->learned, false);
            SpellEntry const* info = sServerFacade.LookupSpellInfo(r->learned);
            sLog.outBasic("Silvermoon: %s trained %s at %s for %u copper (skill %u at %u)", bot->GetName(), info ? info->SpellName[0] : "a rank",
                u->GetName(), r->cost, r->skill, bot->GetSkillValuePure(r->skill));
            if (SilvermoonIsOwn(bot))
                SilvermoonAwayNote(bot, std::string("Trained ") + (info ? info->SpellName[0] : "a profession rank") + " for " + SilvermoonMoneyText(r->cost));
            return true;
        }
    return false;
}

bool ai::SilvermoonGoBagVendor(Player* bot, uint32 itemId)
{
    std::set<uint32> const& sellers = VendorsSelling(itemId);
    if (sellers.empty())
        return false;
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
    std::lock_guard<std::mutex> guard(orderLock);
    if (goOrders.count(bot->GetGUIDLow()))
        return false;                                   // already going somewhere
    GoOrder& order = goOrders[bot->GetGUIDLow()];
    order.flag = UNIT_NPC_FLAG_VENDOR;
    order.what = std::string("a bag vendor for a ") + (proto ? proto->Name1 : "bigger bag");
    order.entries = sellers;
    order.at = time(nullptr);
    return true;
}

bool ai::SilvermoonGoGoal(Player* bot, SilvermoonScoredGoal& goal)
{
    GoOrder order;
    {
        std::lock_guard<std::mutex> guard(orderLock);
        auto it = goOrders.find(bot->GetGUIDLow());
        if (it == goOrders.end())
            return false;
        order = it->second;
    }
    GoSpot const* best = nullptr;
    float bestDist = 0.0f;
    WorldPosition here(bot);
    for (GoSpot const& s : GoSpots())
    {
        if (!(s.flag & order.flag) || s.map != bot->GetMapId() || !Friendly(bot, s.entry))
            continue;
        if (!order.entries.empty() && !order.entries.count(s.entry))
            continue;
        float d = here.fDist(WorldPosition(s.map, s.x, s.y, s.z));
        if (!best || d < bestDist)
        {
            best = &s;
            bestDist = d;
        }
    }
    // there: within 15 yards (S: Kojimbo stood 10 yards from Ranik, behind his counter in Ratchet,
    // "heading to a vendor" and not moving - 8 yards was closer than the bot could get), or the
    // order is 20 minutes old
    bool there = best && bestDist < 15.0f;
    bool firstArrival = there && !order.arrived;
    if (firstArrival)
    {
        std::lock_guard<std::mutex> guard(orderLock);
        auto it = goOrders.find(bot->GetGUIDLow());
        if (it != goOrders.end())
            it->second.arrived = time(nullptr);
        order.arrived = time(nullptr);
    }
    if (firstArrival && (order.flag & UNIT_NPC_FLAG_REPAIR))
        SilvermoonRepairNearby(bot);           // Go to > Repair (S): repair on arrival
    if (firstArrival && order.what.rfind("a bag vendor", 0) == 0)
        SilvermoonBuyBag(bot);                 // "Buy bigger bags": buy on arrival
    if (firstArrival && order.what.rfind("a trainer for ", 0) == 0)
        SilvermoonLearnRankHere(bot);          // profession rank (1.6.22): train on arrival
    bool promoted = false;
    {
        std::lock_guard<std::mutex> guard(orderLock);
        uint32 guid = bot->GetGUIDLow();
        auto q = goQueue.find(guid);
        bool queued = q != goQueue.end() && !q->second.empty();
        // a queued stop after this one: stay here a little to get the business done (S, 1.7)
        bool dwelling = there && queued && time(nullptr) - order.arrived < time_t(GoDwell(order));
        if (!dwelling && (!best || there || (order.at && time(nullptr) - order.at > 20 * MINUTE)))
        {
            goOrders.erase(guid);              // there (or none on this continent): order done
            if (queued)
            {
                GoOrder next = q->second.front();
                q->second.pop_front();
                if (q->second.empty())
                    goQueue.erase(q);
                next.at = time(nullptr);
                goOrders[guid] = next;
                promoted = true;
            }
            else
                return false;
        }
    }
    if (promoted)
        return SilvermoonGoGoal(bot, goal);    // straight on to the next stop
    std::lock_guard<std::mutex> guard(orderLock);
    TemporaryTravelDestination* destination = new TemporaryTravelDestination(WorldPosition(best->map, best->x, best->y, best->z), int32(best->entry));
    goal.destination = destination;
    goal.position = destination->GetPosition();
    goal.kind = silvermoon::GoalKind::Other;
    goal.score.value = goal.score.score = 2000000.0f;
    goal.score.minutes = 1.0f;
    CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(best->entry);
    goal.score.reason = "profession: go to " + order.what + (info ? std::string(" (") + info->Name + ")" : "");
    return true;
}

bool ProfessionLearnAction::isUseful()
{
    std::lock_guard<std::mutex> guard(orderLock);
    auto it = orders.find(bot->GetGUIDLow());
    if (it == orders.end() || it->second.learn.empty() || bot->IsInCombat())
        return false;
    Profession const* p = Find(it->second.learn.front());
    if (!p)
        return false;
    for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "nearest npcs"))
        if (Unit* unit = ai->GetUnit(guid))
            if (unit->GetTypeId() == TYPEID_UNIT && sServerFacade.GetDistance2d(bot, unit) < INTERACTION_DISTANCE)
                for (Spawn const& s : TrainerSpawns()[p->trainerSpell])
                    if (s.entry == unit->GetEntry())
                        return true;
    return false;
}

bool ProfessionLearnAction::Execute(Event& event)
{
    Profession const* p = nullptr;
    {
        std::lock_guard<std::mutex> guard(orderLock);
        auto it = orders.find(bot->GetGUIDLow());
        if (it == orders.end() || it->second.learn.empty())
            return false;
        p = Find(it->second.learn.front());
        it->second.learn.pop_front();
    }
    if (!p || bot->HasSkill(p->skill) || bot->GetMoney() < p->fee)
        return false;
    bot->learnSpell(p->learnSpell, false);
    bot->ModifyMoney(-int32(p->fee));
    BuyTool(bot, p->skill);   // S: the profession comes with its tool (pick, knife, pole) when it can pay
    sLog.outBasic("Silvermoon: %s learned %s at a trainer for %u copper (profession order)", bot->GetName(), p->name, p->fee);
    if (TravelTarget* target = AI_VALUE(TravelTarget*, "travel target"))
        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);   // next order, or back to normal
    return true;
}

bool SilvermoonProfAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester || requester->GetSession()->GetAccountId() != bot->GetSession()->GetAccountId())
        return false;   // your own characters only
    std::string reply;
    if (!SilvermoonProfCommand(bot, event.getParam(), reply))
        reply = "Usage: prof learn <skill> | prof gather <skill> | prof stop [skill]";
    else if (TravelTarget* target = AI_VALUE(TravelTarget*, "travel target"))
        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);   // re-choose now, with the order in place
    // your character on Auto: the panel shows the result, no whisper back to yourself
    if (requester != bot)
        ai->TellPlayerNoFacing(requester, reply);
    return true;
}
