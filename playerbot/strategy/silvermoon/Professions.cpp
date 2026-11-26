#include "playerbot/playerbot.h"
#include "Professions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/silvermoon/GoalScorer.h"
#include "playerbot/strategy/values/CraftValues.h"
#include "Spells/SpellMgr.h"
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
    struct GatherRoute { uint32 zone = 0, skill = 0, skillValue = 0; std::vector<WorldPosition> points; size_t next = 0; time_t built = 0; };
    std::mutex routeLock;
    std::unordered_map<uint32, GatherRoute> routes;
}

bool ai::SilvermoonGatherRouteNext(Player* bot, WorldPosition& next, uint32& index, uint32& total)
{
    uint32 skill = 0;
    {
        std::lock_guard<std::mutex> guard(orderLock);
        auto it = orders.find(bot->GetGUIDLow());
        if (it == orders.end() || (it->second.gather != SKILL_HERBALISM && it->second.gather != SKILL_MINING))
            return false;
        skill = it->second.gather;
    }
    uint32 value = bot->GetSkillValue(skill);
    uint32 zone = bot->GetZoneId();
    std::lock_guard<std::mutex> guard(routeLock);
    GatherRoute& route = routes[bot->GetGUIDLow()];
    // (re)build in a new zone, for another skill, every 25 skill points (new nodes open up) or hourly
    if (route.points.empty() || route.zone != zone || route.skill != skill || value >= route.skillValue + 25 || time(nullptr) - route.built > HOUR)
    {
        route = GatherRoute();
        route.zone = zone;
        route.skill = skill;
        route.skillValue = value;
        route.built = time(nullptr);
        WorldPosition here(bot);
        std::vector<WorldPosition> pool;
        for (NodeSpot const& s : NodeSpots())
        {
            if (s.skill != skill || s.req > value || s.map != bot->GetMapId())
                continue;
            float dx = s.x - here.getX(), dy = s.y - here.getY();
            if (dx * dx + dy * dy > 4000.0f * 4000.0f)
                continue;
            if (sTerrainMgr.GetZoneId(s.map, s.x, s.y, s.z) != zone)
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
        return 1.0f;
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
            const uint32 wanted = UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR;
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
    struct GoOrder { uint32 flag = 0; std::string what; std::set<uint32> entries; time_t at = 0; };   // entries: only these NPCs
    std::unordered_map<uint32, GoOrder> goOrders;
}

bool ai::SilvermoonGoCommand(Player* bot, std::string const& param, std::string& reply)
{
    static const std::map<std::string, std::pair<uint32, std::string>> kinds = {
        { "inn", { UNIT_NPC_FLAG_INNKEEPER, "the inn" } }, { "flight", { UNIT_NPC_FLAG_FLIGHTMASTER, "the flight master" } },
        { "ah", { UNIT_NPC_FLAG_AUCTIONEER, "the auction house" } }, { "bank", { UNIT_NPC_FLAG_BANKER, "the bank" } },
        { "vendor", { UNIT_NPC_FLAG_VENDOR, "a vendor" } }, { "repair", { UNIT_NPC_FLAG_REPAIR, "a repair vendor" } } };
    std::lock_guard<std::mutex> guard(orderLock);
    if (param == "stop")
    {
        goOrders.erase(bot->GetGUIDLow());
        reply = "Go-to cancelled.";
        return true;
    }
    auto k = kinds.find(param);
    if (k == kinds.end())
        return false;
    goOrders[bot->GetGUIDLow()] = { k->second.first, k->second.second, {}, time(nullptr) };
    reply = "Heading to " + k->second.second + ".";
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
    if (there && (order.flag & UNIT_NPC_FLAG_REPAIR))
        SilvermoonRepairNearby(bot);           // Go to > Repair (S): repair on arrival
    std::lock_guard<std::mutex> guard(orderLock);
    if (!best || there || (order.at && time(nullptr) - order.at > 20 * MINUTE))
    {
        goOrders.erase(bot->GetGUIDLow());     // there (or none on this continent): order done
        return false;
    }
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
