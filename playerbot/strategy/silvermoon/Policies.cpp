#include "playerbot/playerbot.h"
#include "Policies.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelNode.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>

using namespace ai;

namespace
{
    std::mutex policyLock;
    std::unordered_map<uint32, SilvermoonPolicy> policies;   // loaded on first use
    std::set<uint32> loaded;

    std::string Encode(SilvermoonPolicy const& p)
    {
        std::ostringstream out;
        out << "z=";
        bool first = true;
        for (uint32 z : p.blockedZones) { out << (first ? "" : ",") << z; first = false; }
        out << ";fm=" << uint32(p.fleeMobs) << ";fh=" << uint32(p.fleeHealth) << ";ah=" << p.ahGold << ";up=" << uint32(p.upgradePct) << ";ws=";
        first = true;
        for (auto const& [skill, mode] : p.weapons) { out << (first ? "" : ",") << skill << ":" << uint32(mode); first = false; }
        return out.str();
    }

    SilvermoonPolicy Decode(std::string const& data)
    {
        SilvermoonPolicy p;
        std::stringstream ss(data);
        std::string part;
        while (std::getline(ss, part, ';'))
        {
            size_t eq = part.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = part.substr(0, eq), value = part.substr(eq + 1);
            if (key == "fm") p.fleeMobs = uint8(atoi(value.c_str()));
            else if (key == "fh") p.fleeHealth = uint8(atoi(value.c_str()));
            else if (key == "ah") p.ahGold = uint32(strtoul(value.c_str(), nullptr, 10));
            else if (key == "up") p.upgradePct = uint8(atoi(value.c_str()));
            else if (key == "z" || key == "ws")
            {
                std::stringstream vs(value);
                std::string item;
                while (std::getline(vs, item, ','))
                {
                    if (item.empty())
                        continue;
                    if (key == "z")
                        p.blockedZones.insert(uint32(atoi(item.c_str())));
                    else
                    {
                        size_t c = item.find(':');
                        if (c != std::string::npos)
                            p.weapons[uint32(atoi(item.substr(0, c).c_str()))] = uint8(atoi(item.substr(c + 1).c_str()));
                    }
                }
            }
        }
        return p;
    }

    // with policyLock held
    SilvermoonPolicy& Get(uint32 guid)
    {
        if (!loaded.count(guid))
        {
            loaded.insert(guid);
            static std::once_flag table;
            std::call_once(table, []()
            {
                CharacterDatabase.Execute("CREATE TABLE IF NOT EXISTS silvermoon_policy (guid INT UNSIGNED NOT NULL PRIMARY KEY, data TEXT NOT NULL, updated_at INT UNSIGNED NOT NULL DEFAULT 0)");
            });
            if (auto r = CharacterDatabase.PQuery("SELECT data FROM silvermoon_policy WHERE guid = %u", guid))
                policies[guid] = Decode(r->Fetch()[0].GetCppString());
        }
        return policies[guid];
    }

    void Save(uint32 guid, SilvermoonPolicy const& p)
    {
        std::string data = Encode(p);
        CharacterDatabase.escape_string(data);
        CharacterDatabase.PExecute("REPLACE INTO silvermoon_policy (guid, data, updated_at) VALUES (%u, '%s', UNIX_TIMESTAMP())", guid, data.c_str());
    }
}

bool ai::SilvermoonIsOwn(Player* bot)
{
    PlayerbotAI* ai = bot ? bot->GetPlayerbotAI() : nullptr;
    if (!ai)
        return false;
    return ai->IsRealPlayer() || (sPlayerbotAIConfig.IsFreeAltBot(bot->GetGUIDLow()) && SilvermoonAutoOn(bot->GetGUIDLow()));
}

SilvermoonPolicy ai::SilvermoonPolicyFor(uint32 guid)
{
    std::lock_guard<std::mutex> guard(policyLock);
    return Get(guid);
}

bool ai::SilvermoonZoneBlocked(uint32 guid, uint32 zoneId)
{
    std::lock_guard<std::mutex> guard(policyLock);
    return Get(guid).blockedZones.count(zoneId) > 0;
}

uint32 ai::SilvermoonUpgradeBudget(Player* bot, uint32 defaultPct)
{
    uint32 pct = defaultPct;
    if (SilvermoonIsOwn(bot))
    {
        std::lock_guard<std::mutex> guard(policyLock);
        if (uint8 up = Get(bot->GetGUIDLow()).upgradePct)
            pct = up;
    }
    return uint32(uint64(bot->GetMoney()) * pct / 100);
}

bool ai::SilvermoonWeaponAvoided(uint32 guid, uint32 skill)
{
    std::lock_guard<std::mutex> guard(policyLock);
    auto const& w = Get(guid).weapons;
    auto it = w.find(skill);
    return it != w.end() && it->second == 2;
}

bool ai::SilvermoonPolicyCommand(Player* bot, std::string const& param, std::string& reply)
{
    char a[16] = {}, b[16] = {}, c[16] = {};
    int n = sscanf(param.c_str(), "%15s %15s %15s", a, b, c);
    std::string k = n >= 1 ? a : "", v1 = n >= 2 ? b : "", v2 = n >= 3 ? c : "";
    std::lock_guard<std::mutex> guard(policyLock);
    SilvermoonPolicy& p = Get(bot->GetGUIDLow());
    if (k == "zone" && n == 3)
    {
        uint32 zone = uint32(atoi(v1.c_str()));
        if (v2 == "off") p.blockedZones.insert(zone); else p.blockedZones.erase(zone);
        reply = "Zones updated.";
    }
    else if (k == "flee" && n == 3 && v1 == "mobs")
        p.fleeMobs = uint8(std::min(9, std::max(0, atoi(v2.c_str())))), reply = "Flee rule updated.";
    else if (k == "flee" && n == 3 && v1 == "health")
        p.fleeHealth = uint8(std::min(90, std::max(0, atoi(v2.c_str())))), reply = "Flee rule updated.";
    else if (k == "ah" && n == 2)
        p.ahGold = uint32(std::max(0, atoi(v1.c_str()))) * 10000, reply = "Auction house threshold updated.";
    else if (k == "upgrade" && n == 2)
        p.upgradePct = uint8(std::min(100, std::max(0, atoi(v1.c_str())))), reply = "Upgrade budget updated.";
    else if (k == "weapon" && n == 3)
    {
        uint32 skill = uint32(atoi(v1.c_str()));
        if (v2 == "train") p.weapons[skill] = 1;
        else if (v2 == "avoid") p.weapons[skill] = 2;
        else p.weapons.erase(skill);
        reply = "Weapon skills updated.";
    }
    else if (k == "reset")
        p = SilvermoonPolicy(), reply = "Policies reset.";
    else
        return false;
    Save(bot->GetGUIDLow(), p);
    return true;
}

std::string ai::SilvermoonPolicyPanel(Player* bot)
{
    std::lock_guard<std::mutex> guard(policyLock);
    return "SMPOL\t" + Encode(Get(bot->GetGUIDLow()));
}

bool SilvermoonPolicyAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester || requester->GetSession()->GetAccountId() != bot->GetSession()->GetAccountId())
        return false;   // your own characters only
    std::string reply;
    if (!SilvermoonPolicyCommand(bot, event.getParam(), reply))
        reply = "Usage: pol zone <id> on|off | pol flee mobs <n> | pol flee health <pct> | pol ah <gold> | pol upgrade <pct> | pol weapon <skill> train|avoid|auto | pol reset";
    else if (TravelTarget* target = AI_VALUE(TravelTarget*, "travel target"))
        if (target->IsActive())
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);   // re-choose with the new rules
    if (requester != bot)
        ai->TellPlayerNoFacing(requester, reply);
    return true;
}
