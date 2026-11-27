#include "playerbot/playerbot.h"
#include "Policies.h"
#include "playerbot/silvermoon/Recovery.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelNode.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "playerbot/ServerFacade.h"
#include "Spells/SpellMgr.h"
#include <algorithm>
#include <map>
#include <tuple>
#include <cctype>
#include "playerbot/strategy/Engine.h"

using namespace ai;

namespace
{
    std::mutex policyLock;
    std::unordered_map<uint32, SilvermoonPolicy> policies;   // loaded on first use
    std::set<uint32> loaded;

    std::string Encode(SilvermoonPolicy const& p)
    {
        std::ostringstream out;
        out << "za=";
        bool first = true;
        for (uint32 z : p.allowedZones) { out << (first ? "" : ",") << z; first = false; }
        out << ";et=" << uint32(p.eatTo) << ";dt=" << uint32(p.drinkTo) << ";tv=" << uint32(p.travel) << ";pt=" << uint32(p.potionAt) << ";ds=" << (p.disenchant ? 1 : 0) << ";tg=" << uint32(p.tradeGoods) << ";sb=";
        bool firstSpell = true;
        for (uint32 sp : p.blockedSpells) { out << (firstSpell ? "" : ",") << sp; firstSpell = false; }
        out << ";it=";
        first = true;
        for (auto const& [item, rule] : p.items) { out << (first ? "" : ",") << item << ":" << uint32(rule.first) << ":" << (rule.second ? 1 : 0); first = false; }
        out << ";ae=" << (p.askEquip ? 1 : 0) << ";bg=" << uint32(p.bags) << ";fm=" << uint32(p.fleeMobs) << ";fh=" << uint32(p.fleeHealth) << ";ah=" << p.ahGold << ";up=" << uint32(p.upgradePct) << ";ws=";
        first = true;
        for (auto const& [skill, mode] : p.weapons) { out << (first ? "" : ",") << skill << ":" << uint32(mode); first = false; }
        out << ";rb=";
        first = true;
        for (auto const& [name, cond] : p.rotation) { out << (first ? "" : ",") << name << ":" << uint32(cond); first = false; }
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
            else if (key == "bg") p.bags = uint8(atoi(value.c_str()));
            else if (key == "ae") p.askEquip = atoi(value.c_str()) != 0;
            else if (key == "fh") p.fleeHealth = uint8(atoi(value.c_str()));
            else if (key == "et") p.eatTo = uint8(atoi(value.c_str()));
            else if (key == "tv") p.travel = uint8(atoi(value.c_str()));
            else if (key == "pt") p.potionAt = uint8(atoi(value.c_str()));
            else if (key == "ds") p.disenchant = atoi(value.c_str()) != 0;
            else if (key == "tg") p.tradeGoods = uint8(atoi(value.c_str()));
            else if (key == "sb")
            {
                std::stringstream vs(value);
                std::string item;
                while (std::getline(vs, item, ','))
                    if (!item.empty())
                        p.blockedSpells.insert(uint32(atoi(item.c_str())));
            }
            else if (key == "dt") p.drinkTo = uint8(atoi(value.c_str()));
            else if (key == "ro" || key == "rb")
            {
                // "ro" (1.7) held one condition 0-5; "rb" (1.7.1) holds condition bits
                static const uint8 legacy[6] = { 0, 1, 2, 4, 8, 16 };
                std::stringstream vs(value);
                std::string entry;
                while (std::getline(vs, entry, ','))
                {
                    size_t c = entry.rfind(':');
                    if (c != std::string::npos && c > 0)
                    {
                        int v = atoi(entry.substr(c + 1).c_str());
                        uint8 cond = key == "ro" ? legacy[std::min(5, std::max(0, v))] : uint8(std::min(127, std::max(0, v)));
                        p.rotation.push_back(std::make_pair(entry.substr(0, c), cond));
                    }
                }
            }
            else if (key == "ah") p.ahGold = uint32(strtoul(value.c_str(), nullptr, 10));
            else if (key == "up") p.upgradePct = uint8(atoi(value.c_str()));
            else if (key == "it")
            {
                std::stringstream vs(value);
                std::string item;
                while (std::getline(vs, item, ','))
                {
                    unsigned a = 0, b = 0, c = 0;
                    if (sscanf(item.c_str(), "%u:%u:%u", &a, &b, &c) == 3 && a && b)
                        p.items[a] = std::make_pair(uint8(b), c != 0);
                }
            }
            else if (key == "za" || key == "ws")
            {
                std::stringstream vs(value);
                std::string item;
                while (std::getline(vs, item, ','))
                {
                    if (item.empty())
                        continue;
                    if (key == "za")
                        p.allowedZones.insert(uint32(atoi(item.c_str())));
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
    std::set<uint32> const& allowed = Get(guid).allowedZones;
    return !allowed.empty() && !allowed.count(zoneId);
}

uint32 ai::SilvermoonAhCarried(std::list<Item*> const& items)
{
    uint32 total = 0;
    for (Item* item : items)
        if (item && item->GetProto())
            total += std::max(item->GetProto()->BuyPrice, item->GetProto()->SellPrice * 4) * item->GetCount();
    return total;
}

bool ai::SilvermoonAhTripDue(Player* bot, std::list<Item*> const& items)
{
    if (!SilvermoonIsOwn(bot) || items.empty())
        return false;
    uint32 gold = SilvermoonPolicyFor(bot->GetGUIDLow()).ahGold;
    return gold ? SilvermoonAhCarried(items) >= gold : true;   // no setting: even one marked item
}

bool ai::SilvermoonSpellBlocked(Player* bot, uint32 learnedSpell)
{
    if (!learnedSpell || !SilvermoonIsOwn(bot))
        return false;
    uint32 first = sSpellMgr.GetFirstSpellInChain(learnedSpell);
    std::lock_guard<std::mutex> guard(policyLock);
    std::set<uint32> const& blocked = Get(bot->GetGUIDLow()).blockedSpells;
    return blocked.count(first ? first : learnedSpell) > 0;
}

std::vector<std::string> ai::SilvermoonSpellListMessages(Player* bot)
{
    // every ability the class trainers teach this class and race: first rank id -> (lowest level, name)
    std::map<uint32, std::pair<uint32, std::string>> abilities;
    // first rank id -> every rank the trainers teach (S, 1.6.21: an ability with nothing left to
    // train - Parry, or the top rank of a spell - is marked 2 and the panel leaves it out)
    std::map<uint32, std::set<uint32>> ranks;
    for (uint32 id = 0; id < sCreatureStorage.GetMaxEntry(); ++id)
    {
        CreatureInfo const* co = sCreatureStorage.LookupEntry<CreatureInfo>(id);
        if (!co || co->TrainerType != TRAINER_TYPE_CLASS || co->TrainerClass != bot->getClass())
            continue;
        for (TrainerSpellData const* data : { sObjectMgr.GetNpcTrainerSpells(co->Entry), sObjectMgr.GetNpcTrainerTemplateSpells(co->TrainerTemplateId) })
        {
            if (!data)
                continue;
            for (auto const& entry : data->spellList)
            {
                uint32 learned = entry.second.spell;
                if (SpellEntry const* teach = sServerFacade.LookupSpellInfo(entry.second.spell))
                    for (int j = 0; j < 3; ++j)
                        if (teach->Effect[j] == SPELL_EFFECT_LEARN_SPELL && teach->EffectTriggerSpell[j])
                            learned = teach->EffectTriggerSpell[j];
                if (!learned || !bot->IsSpellFitByClassAndRace(learned))
                    continue;
                SpellEntry const* spell = sServerFacade.LookupSpellInfo(learned);
                if (!spell)
                    continue;
                uint32 first = sSpellMgr.GetFirstSpellInChain(learned);
                if (!first)
                    first = learned;
                // obsolete trainer entries ("zzOLDHip Shot III") are left out
                if (spell->SpellName[0] && (strncmp(spell->SpellName[0], "zz", 2) == 0 || strncmp(spell->SpellName[0], "ZZ", 2) == 0))
                    continue;
                ranks[first].insert(learned);
                auto it = abilities.find(first);
                if (it == abilities.end())
                    abilities[first] = std::make_pair(entry.second.reqLevel, std::string(spell->SpellName[0]));
                else if (entry.second.reqLevel < it->second.first)
                    it->second.first = entry.second.reqLevel;
            }
        }
    }
    // sorted by level; "id:level:known:name" joined with '^', split into messages under 240 bytes
    std::vector<std::tuple<uint32, std::string, uint32>> sorted;
    for (auto const& [first, info] : abilities)
        sorted.push_back({ info.first, info.second, first });
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::string> out;
    out.push_back("SMSPELL\tBEGIN");
    std::string cur;
    for (auto const& [level, name, first] : sorted)
    {
        std::string clean = name;
        for (char& c : clean)
            if (c == '^' || c == ';' || c == '|' || c == '\t')
                c = ' ';
        // 0 not learned, 1 learned with ranks still to train, 2 everything the trainers teach is known
        bool all = true;
        for (uint32 r : ranks[first])
            if (!bot->HasSpell(r))
                all = false;
        std::string known = !bot->HasSpell(first) ? "0" : (all ? "2" : "1");
        std::string item = std::to_string(first) + ":" + std::to_string(level) + ":" + known + ":" + clean;
        if (cur.size() + item.size() + 1 > 230)
        {
            out.push_back("SMSPELL\t" + cur);
            cur.clear();
        }
        cur += (cur.empty() ? "" : "^") + item;
    }
    if (!cur.empty())
        out.push_back("SMSPELL\t" + cur);
    out.push_back("SMSPELL\tEND");
    return out;
}

uint8 ai::SilvermoonTravelRules(Player* bot)
{
    if (!SilvermoonIsOwn(bot))
        return 0;
    return SilvermoonPolicyFor(bot->GetGUIDLow()).travel;
}

uint8 ai::SilvermoonRestTarget(Player* bot, bool mana)
{
    if (!SilvermoonIsOwn(bot))
        return 0;
    SilvermoonPolicy p = SilvermoonPolicyFor(bot->GetGUIDLow());
    return mana ? p.drinkTo : p.eatTo;
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
    // a random bot spends only what is above a full repair of its gear (S, 1.6.20: repair reserve)
    uint64 money = SilvermoonIsOwn(bot) ? bot->GetMoney() : SilvermoonSpendable(bot);
    return uint32(money * pct / 100);
}

bool ai::SilvermoonWeaponAvoided(uint32 guid, uint32 skill)
{
    std::lock_guard<std::mutex> guard(policyLock);
    auto const& w = Get(guid).weapons;
    auto it = w.find(skill);
    return it != w.end() && it->second == 2;
}

namespace
{
    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }
}

std::vector<SilvermoonRotDefault> ai::SilvermoonDefaultRotation(Player* bot)
{
    std::vector<SilvermoonRotDefault> out;
    PlayerbotAI* ai = bot ? bot->GetPlayerbotAI() : nullptr;
    Engine* engine = ai ? ai->SilvermoonEngine(BotState::BOT_STATE_COMBAT) : nullptr;
    if (!engine)
        return out;
    AiObjectContext* context = ai->GetAiObjectContext();
    std::map<std::string, SilvermoonRotDefault> best;      // by spell name, the highest relevance
    for (auto const& [action, relevance, trigger] : engine->SilvermoonActionList())
    {
        uint32 id = context->GetValue<uint32>("spell id", action)->Get();
        if (!id || !bot->HasSpell(id))
            continue;
        SpellEntry const* sp = sServerFacade.LookupSpellInfo(id);
        if (!sp || !sp->SpellName[0] || IsPassiveSpell(id))
            continue;
        std::string name = sp->SpellName[0];
        if (Lower(name).rfind("zz", 0) == 0)
            continue;
        std::string t = Lower(trigger), a = Lower(action);
        uint8 cond = 16;                 // the class's own rule
        if (t == "default")
            cond = 0;
        else if (t.find("target") != std::string::npos && (t.find("low health") != std::string::npos || t.find("critical health") != std::string::npos))
            cond = 2;
        else if (t.find("execute") != std::string::npos)
            cond = 2;
        else if (t.find("target") == std::string::npos && t.find("party") == std::string::npos &&
            (t == "low health" || t == "critical health" || t == "medium health" || t == "almost dead"))
            cond = 4;
        else if (t == a || t == "no " + a || t == a + " on target" || t == "no " + a + " on target")
            cond = 1;
        else if (t.find("interrupt") != std::string::npos || t.find("casting") != std::string::npos)
            cond = 32;
        SilvermoonRotDefault& e = best[Lower(name)];
        if (e.name.empty() || relevance > e.relevance)
        {
            e.name = name;
            e.cond = cond;
            e.trigger = trigger;
            e.relevance = relevance;
        }
    }
    for (auto const& entry : best)
        out.push_back(entry.second);
    std::stable_sort(out.begin(), out.end(), [](SilvermoonRotDefault const& x, SilvermoonRotDefault const& y) { return x.relevance > y.relevance; });
    return out;
}

std::vector<std::string> ai::SilvermoonDefaultRotationText(Player* bot)
{
    std::vector<std::string> out;
    std::string text = "SMROT\tR;";
    bool first = true;
    for (SilvermoonRotDefault const& e : SilvermoonDefaultRotation(bot))
    {
        std::string trig = e.trigger;
        for (char& ch : trig)
            if (ch == ':' || ch == '^' || ch == ',' || ch == ';')
                ch = ' ';
        std::string part = e.name + ":" + std::to_string(uint32(e.cond)) + ":" + trig;
        if (!first && text.size() + part.size() > 200)
        {
            out.push_back(text);
            text = "SMROT\t+;";
            first = true;
        }
        text += (first ? "" : "^") + part;
        first = false;
    }
    out.push_back(text);
    return out;
}

std::string ai::SilvermoonDefaultTrigger(Player* bot, std::string const& spell)
{
    // cached per character for a minute (the class rotation doesn't change mid-fight)
    static std::mutex lock;
    static std::unordered_map<uint32, std::pair<time_t, std::map<std::string, std::string>>> cache;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(bot->GetGUIDLow());
        if (it != cache.end() && time(nullptr) - it->second.first < 60)
        {
            auto t = it->second.second.find(Lower(spell));
            return t == it->second.second.end() ? std::string() : t->second;
        }
    }
    std::map<std::string, std::string> triggers;
    for (SilvermoonRotDefault const& e : SilvermoonDefaultRotation(bot))
        if (e.trigger != "default")
            triggers[Lower(e.name)] = e.trigger;
    std::lock_guard<std::mutex> guard(lock);
    cache[bot->GetGUIDLow()] = std::make_pair(time(nullptr), triggers);
    auto t = triggers.find(Lower(spell));
    return t == triggers.end() ? std::string() : t->second;
}

bool ai::SilvermoonRotationBlocks(Player* bot, uint32 spellId)
{
    static const std::vector<std::vector<std::string>> groups = {
        { "blessing of might", "blessing of wisdom", "blessing of kings", "blessing of salvation", "blessing of light", "blessing of sanctuary" },
        { "devotion aura", "retribution aura", "concentration aura", "shadow resistance aura", "frost resistance aura", "fire resistance aura", "sanctity aura" },
        { "seal of righteousness", "seal of the crusader", "seal of command", "seal of justice", "seal of light", "seal of wisdom" },
        { "aspect of the hawk", "aspect of the monkey", "aspect of the cheetah", "aspect of the pack", "aspect of the wild", "aspect of the beast" },
        { "demon skin", "demon armor" },
        { "frost armor", "ice armor", "mage armor" },
        { "battle stance", "defensive stance", "berserker stance" },
        { "bear form", "dire bear form", "cat form", "travel form", "aquatic form", "moonkin form" },
        { "rockbiter weapon", "flametongue weapon", "frostbrand weapon", "windfury weapon" },
    };
    if (!bot || !spellId || !SilvermoonIsOwn(bot))
        return false;
    SpellEntry const* sp = sServerFacade.LookupSpellInfo(spellId);
    if (!sp || !sp->SpellName[0])
        return false;
    std::string name = Lower(sp->SpellName[0]);
    for (auto const& group : groups)
    {
        if (std::find(group.begin(), group.end(), name) == group.end())
            continue;
        std::vector<std::pair<std::string, uint8>> list = SilvermoonRotationFor(bot->GetGUIDLow());
        bool listed = false, other = false;
        for (auto const& e : list)
        {
            std::string n = Lower(e.first);
            if (n == name)
                listed = true;
            else if (std::find(group.begin(), group.end(), n) != group.end())
                other = true;
        }
        return other && !listed;
    }
    return false;
}

std::vector<std::pair<std::string, uint8>> ai::SilvermoonRotationFor(uint32 guid)
{
    std::lock_guard<std::mutex> guard(policyLock);
    return Get(guid).rotation;
}

bool ai::SilvermoonPolicyCommand(Player* bot, std::string const& param, std::string& reply)
{
    // rotation (S, 1.7): "rot clear", or "rot Name:cond,Name:cond,..." (names have spaces, so it is
    // read before the word-by-word parsing below)
    // "rot+ ..." adds to the list (a long list comes in several chat lines)
    if (param.rfind("rot ", 0) == 0 || param.rfind("rot+ ", 0) == 0)
    {
        bool append = param[3] == '+';
        std::string rest = param.substr(append ? 5 : 4);
        std::vector<std::pair<std::string, uint8>> list;
        if (rest != "clear")
        {
            std::stringstream vs(rest);
            std::string entry;
            while (std::getline(vs, entry, ',') && list.size() < 24)
            {
                size_t c = entry.rfind(':');
                std::string name = c == std::string::npos ? entry : entry.substr(0, c);
                uint8 cond = c == std::string::npos ? 0 : uint8(std::min(127, std::max(0, atoi(entry.substr(c + 1).c_str()))));
                while (!name.empty() && name[0] == ' ') name.erase(0, 1);
                while (!name.empty() && name.back() == ' ') name.pop_back();
                if (name.empty() || name.find_first_of(";=") != std::string::npos)
                    continue;
                list.push_back(std::make_pair(name, cond));
            }
        }
        std::lock_guard<std::mutex> guard(policyLock);
        SilvermoonPolicy& p = Get(bot->GetGUIDLow());
        if (append)
        {
            for (auto const& e : list)
                if (p.rotation.size() < 24)
                    p.rotation.push_back(e);
        }
        else
            p.rotation = list;
        Save(bot->GetGUIDLow(), p);
        reply = p.rotation.empty() ? "Rotation cleared: I fight with my class's own rotation." :
            "Rotation set: " + std::to_string(p.rotation.size()) + " abilities, in your order.";
        return true;
    }
    char a[16] = {}, b[16] = {}, c[16] = {};
    int n = sscanf(param.c_str(), "%15s %15s %15s", a, b, c);
    std::string k = n >= 1 ? a : "", v1 = n >= 2 ? b : "", v2 = n >= 3 ? c : "";
    std::lock_guard<std::mutex> guard(policyLock);
    SilvermoonPolicy& p = Get(bot->GetGUIDLow());
    if (k == "zone" && n == 2 && v1 == "clear")
        p.allowedZones.clear(), reply = "Zone list off: I pick zones myself.";
    else if (k == "zone" && n == 3)
    {
        uint32 zone = uint32(atoi(v1.c_str()));
        if (v2 == "on") p.allowedZones.insert(zone); else p.allowedZones.erase(zone);
        reply = p.allowedZones.empty() ? "Zone list off: I pick zones myself." : "Zones updated: I'll stay in ticked zones from my next move.";
    }
    else if (k == "spell" && n == 3)
    {
        // "spell <first rank id> on|off": off = never train this ability
        uint32 first = uint32(atoi(v1.c_str()));
        if (!first)
            return false;
        if (v2 == "off") p.blockedSpells.insert(first); else p.blockedSpells.erase(first);
        SpellEntry const* sp = sServerFacade.LookupSpellInfo(first);
        reply = std::string(sp ? sp->SpellName[0] : "That skill") + (v2 == "off" ? ": I won't train it." : ": I'll train it.");
    }
    else if (k == "travel" && n == 3 && (v1 == "avoid" || v1 == "grey"))
    {
        // "travel avoid on|off": no new pulls while travelling
        // "travel grey on|off":  ignore grey mobs while travelling (on is the default)
        if (v1 == "avoid")
            p.travel = v2 == "on" ? (p.travel | 1) : (p.travel & ~1);
        else
            p.travel = v2 == "on" ? (p.travel & ~2) : (p.travel | 2);
        reply = v1 == "avoid" ? (v2 == "on" ? "I'll leave mobs alone while travelling." : "I may fight mobs on the way again.")
                              : (v2 == "on" ? "I'll ignore grey mobs while travelling." : "I may fight grey mobs on the way.");
    }
    else if ((k == "eat" || k == "drink") && n == 2)
    {
        uint8 pct = uint8(std::min(100, std::max(25, atoi(v1.c_str()))));
        (k == "eat" ? p.eatTo : p.drinkTo) = pct;
        reply = k == "eat" ? "Eating until health " + std::to_string(pct) + "%." : "Drinking until mana " + std::to_string(pct) + "%.";
    }
    else if (k == "flee" && n == 3 && v1 == "mobs")
        p.fleeMobs = uint8(std::min(9, std::max(1, atoi(v2.c_str())))), reply = "I'll flee from " + std::to_string(p.fleeMobs) + " or more mobs.";
    else if (k == "flee" && n == 3 && v1 == "health")
        p.fleeHealth = uint8(std::min(90, std::max(5, atoi(v2.c_str())))), reply = "I'll flee below " + std::to_string(p.fleeHealth) + "% health.";
    else if (k == "tradegoods" && n == 2)
    {
        // "tradegoods all|prof|none"
        p.tradeGoods = v1 == "all" ? 1 : v1 == "prof" ? 2 : 0;
        reply = p.tradeGoods == 1 ? "I'll keep all trade goods." : p.tradeGoods == 2 ? "I'll keep trade goods my professions use." : "I'll sell or auction trade goods.";
    }
    else if (k == "disenchant" && n == 2)
        p.disenchant = v1 == "on", reply = p.disenchant ? "I'll disenchant green gear I won't wear (with Enchanting)." : "I won't disenchant gear.";
    else if (k == "potion" && n == 2)
        p.potionAt = uint8(std::min(90, std::max(5, atoi(v1.c_str())))), reply = "I'll drink a healing potion below " + std::to_string(p.potionAt) + "% health.";
    else if (k == "ah" && n == 2)
        p.ahGold = uint32(std::max(0, atoi(v1.c_str()))) * 10000, reply = "Auction house trip updated.";
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
    else if (k == "askequip" && n == 2)
        p.askEquip = v1 == "on", reply = p.askEquip ? "I'll ask before equipping upgrades (an hour, then I decide)." : "I'll equip upgrades myself.";
    else if (k == "equip" && n == 3 && (v1 == "yes" || v1 == "no"))
    {
        // the answer to an upgrade question (see SilvermoonEquipAllowed; its own lock)
        return SilvermoonEquipAnswer(bot, uint32(strtoul(v2.c_str(), nullptr, 10)), v1 == "yes", reply);
    }
    else if (k == "bags" && n == 3)
    {
        uint8 bit = v1 == "buy" ? 1 : v1 == "bigger" ? 2 : 0;
        if (!bit)
            return false;
        if (bit == 1)   // "bags buy on|off": the bit means "don't buy" (1.6.21: buying includes bigger bags)
            p.bags = v2 == "off" ? (p.bags | 1) : (p.bags & ~1);
        else
            p.bags = v2 == "on" ? (p.bags | 2) : (p.bags & ~2);
        reply = "Bag rules updated.";
    }
    else if (k == "item" && n >= 2)
    {
        // pol item <id> sell|ah|destroy|keep|clear [once|always]
        uint32 item = uint32(atoi(v1.c_str()));
        char act[16] = {}, freq[16] = {};
        sscanf(param.c_str(), "%*s %*s %15s %15s", act, freq);
        std::string a(act), f(freq);
        uint8 action = a == "sell" ? 1 : a == "ah" ? 2 : a == "destroy" ? 3 : a == "keep" ? 4 : 0;
        if (!item)
            return false;
        if (!action)
            p.items.erase(item), reply = "Item rule cleared.";
        else
        {
            p.items[item] = std::make_pair(action, f != "once");
            reply = std::string("Item rule set: ") + a + (f == "once" ? " once." : " always.");
        }
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

void ai::SilvermoonWeaponTraining(Player* bot)
{
    static const std::set<uint32> capitals = { 1519, 1537, 1657, 1637, 1638, 1497 };
    if (!capitals.count(bot->GetZoneId()) || bot->IsInCombat() || !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING))
        return;
    // weapon skill -> the spell that teaches it
    static const std::pair<uint32, uint32> skills[] = {
        { 173, 1180 }, { 43, 201 }, { 44, 196 }, { 54, 198 }, { 473, 15590 }, { 55, 202 }, { 172, 197 }, { 160, 199 },
        { 229, 200 }, { 136, 227 }, { 45, 264 }, { 46, 266 }, { 226, 5011 }, { 176, 2567 }, { 228, 5009 } };
    const uint32 fee = 1000;
    for (auto const& [skill, spell] : skills)
    {
        if (bot->HasSpell(spell) || bot->HasSkill(skill) || SilvermoonWeaponAvoided(bot->GetGUIDLow(), skill))
            continue;
        if (!bot->IsSpellFitByClassAndRace(spell))
            continue;
        if (fee > SilvermoonUpgradeBudget(bot, 50))
            return;
        bot->learnSpell(spell, false);
        bot->ModifyMoney(-int32(fee));
        sLog.outBasic("Silvermoon: %s learned weapon skill %u at a weapon master for %u copper (policy)", bot->GetName(), skill, fee);
        return;
    }
}

uint8 ai::SilvermoonItemRule(uint32 guid, uint32 itemId)
{
    std::lock_guard<std::mutex> guard(policyLock);
    auto const& items = Get(guid).items;
    auto it = items.find(itemId);
    return it == items.end() ? 0 : it->second.first;
}

void ai::SilvermoonApplyItemRules(Player* bot)
{
    std::vector<uint32> destroy;
    bool changed = false;
    {
        std::lock_guard<std::mutex> guard(policyLock);
        SilvermoonPolicy& p = Get(bot->GetGUIDLow());
        for (auto it = p.items.begin(); it != p.items.end();)
        {
            uint32 count = bot->GetItemCount(it->first);
            if (it->second.first == 3 && count)
                destroy.push_back(it->first);
            if (!it->second.second && (!count || it->second.first == 3))
            {
                it = p.items.erase(it);                // a "once" rule is done
                changed = true;
            }
            else
                ++it;
        }
        if (changed)
            Save(bot->GetGUIDLow(), p);
    }
    for (uint32 item : destroy)
    {
        uint32 count = bot->GetItemCount(item);
        bot->DestroyItemCount(item, count, true);
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(item);
        sLog.outBasic("Silvermoon: %s destroyed %u x %s (item rule)", bot->GetName(), count, proto ? proto->Name1 : "?");
    }
    // "sell" rules (1.7.8, S: items set to sell always weren't sold on a vendor trip): whenever a vendor
    // is in reach and something with a sell rule is in the bags, it's sold - the stock sell step only
    // runs as the bags fill up
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai || bot->IsInCombat())
        return;
    bool haveSell = false;
    {
        std::lock_guard<std::mutex> guard(policyLock);
        for (auto const& [item, rule] : Get(bot->GetGUIDLow()).items)
            if (rule.first == 1 && bot->GetItemCount(item))
                haveSell = true;
    }
    if (!haveSell)
        return;
    for (ObjectGuid const& g : ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs")->Get())
        if (bot->GetNPCIfCanInteractWith(g, UNIT_NPC_FLAG_VENDOR))
        {
            ai->DoSpecificAction("sell", Event("silvermoon item rules", "legacy all"), true);
            return;
        }
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

// ---------------------------------------------------------------------------------------------------
// Ask before equipping upgrades (S, 1.6.21). An upgrade the character would put on waits for your
// answer: the panel shows Equip / Keep current (addon message "SMASK", repeated every minute while
// you are on that character). No answer in an hour: it decides itself and leaves a "Notification:"
// line in the away log. Keep current is remembered for that item until the server restarts.
namespace
{
    struct EquipAsk { time_t at = 0; time_t sent = 0; uint8 answer = 0; std::string name, old; uint32 itemId = 0; };
    std::mutex askLock;
    std::unordered_map<uint32, std::map<uint32, EquipAsk>> equipAsks;   // character -> item (low guid) -> question

    void SendAsk(Player* bot, uint32 itemGuid, EquipAsk const& q)
    {
        if (!bot->GetSession() || !bot->GetPlayerbotAI() || !bot->GetPlayerbotAI()->IsRealPlayer())
            return;                                     // you are not on this character right now
        int32 left = int32(HOUR) - int32(time(nullptr) - q.at);
        std::string msg = "SMASK\t" + std::to_string(itemGuid) + "^" + std::to_string(q.itemId) + "^" + q.name + "^" + q.old + "^" +
            std::to_string(std::max(0, left / 60));
        WorldPacket packet;
        ChatHandler::BuildChatPacket(packet, CHAT_MSG_PARTY, msg.c_str(), LANG_ADDON, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());
        bot->GetSession()->SendPacket(packet);
    }
}

bool ai::SilvermoonEquipAllowed(Player* bot, Item* item)
{
    if (!bot || !item || !SilvermoonIsOwn(bot) || !SilvermoonPolicyFor(bot->GetGUIDLow()).askEquip)
        return true;
    uint32 itemGuid = item->GetGUIDLow();
    time_t now = time(nullptr);
    std::string note;
    bool allowed = false;
    {
        std::lock_guard<std::mutex> guard(askLock);
        auto& asks = equipAsks[bot->GetGUIDLow()];
        auto it = asks.find(itemGuid);
        if (it == asks.end())
        {
            EquipAsk q;
            q.at = now;
            q.itemId = item->GetEntry();
            q.name = item->GetProto()->Name1;
            uint16 dest = 0;
            if (bot->CanEquipItem(NULL_SLOT, dest, item, true) == EQUIP_ERR_OK)
                if (Item* cur = bot->GetItemByPos(dest))
                    q.old = cur->GetProto()->Name1;
            for (char& c : q.name) if (c == '^' || c == '\t') c = ' ';
            for (char& c : q.old) if (c == '^' || c == '\t') c = ' ';
            it = asks.emplace(itemGuid, q).first;
        }
        EquipAsk& q = it->second;
        if (q.answer == 1)
        {
            asks.erase(it);
            return true;
        }
        if (q.answer == 2)
            return false;                               // you said keep current
        if (now - q.at >= HOUR)
        {
            note = "Notification: equipped " + q.name + (q.old.empty() ? "" : " instead of " + q.old) + " - no answer within an hour, so I decided";
            asks.erase(it);
            allowed = true;
        }
        else if (now - q.sent >= MINUTE)
        {
            q.sent = now;
            SendAsk(bot, itemGuid, q);
        }
    }
    if (!note.empty())
        SilvermoonAwayNote(bot, note);
    return allowed;
}

bool ai::SilvermoonEquipAnswer(Player* bot, uint32 itemGuid, bool equip, std::string& reply)
{
    std::lock_guard<std::mutex> guard(askLock);
    auto& asks = equipAsks[bot->GetGUIDLow()];
    auto it = asks.find(itemGuid);
    if (it == asks.end())
    {
        reply = "That question has already been settled.";
        return true;
    }
    it->second.answer = equip ? 1 : 2;
    reply = equip ? "Equipping " + it->second.name + "." : "Keeping my current gear instead of " + it->second.name + ".";
    return true;
}
