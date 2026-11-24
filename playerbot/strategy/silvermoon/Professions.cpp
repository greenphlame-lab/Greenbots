#include "playerbot/playerbot.h"
#include "Professions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/silvermoon/GoalScorer.h"
#include <algorithm>
#include <cstdio>
#include <deque>
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

    struct Orders { std::deque<uint32> learn; uint32 gather = 0; };
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
    if (v == "stop")
    {
        if (n == 2)
        {
            o.learn.erase(std::remove(o.learn.begin(), o.learn.end(), skill), o.learn.end());
            if (o.gather == skill)
                o.gather = 0;
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
        snprintf(buf, sizeof(buf), "%s%u:%u:%u:%c", first ? "" : ";", p.skill, bot->HasSkill(p.skill) ? uint32(bot->GetSkillValue(p.skill)) : 0,
            bot->HasSkill(p.skill) ? uint32(bot->GetSkillMax(p.skill)) : 0, state);
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
