#include "playerbot/playerbot.h"
#include "SurviveStrategy.h"
#include "Policies.h"
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "playerbot/RandomPlayerbotMgr.h"
#include "Tools/Formulas.h"
#include "playerbot/TravelMgr.h"
#include "Entities/Creature.h"
#include "Entities/Pet.h"
#include "Server/DBCStores.h"
#include "Spells/SpellMgr.h"

using namespace ai;

void SurviveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon flee outmatched", ACTION_EMERGENCY + 5), NULL)));
    // guards (S, 1.6.20): always run from a guard, above walking past greys, below the lifesavers
    // (Vanish, Blink, Ice Block... still go first when low)
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon flee guard", ACTION_EMERGENCY + 5.8f), NULL)));
    // gathering past grey mobs (S): above the flee step, below the lifesavers
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon walk past greys", ACTION_EMERGENCY + 5.5f), NULL)));
    // lifesavers for every class (S: Mommy, a paladin, died to a level 6 Kobold Miner with Divine
    // Protection, Blessing of Protection and Lay on Hands all ready) - above the flee step
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon lifesaver", ACTION_EMERGENCY + 6), NULL)));
    // rotation (S, 1.7): your priority list, above the class rotations (they top out around
    // ACTION_HIGH + 5), below moving into range, interrupts, heals and the lifesavers
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon rotation", ACTION_HIGH + 8), NULL)));
    // policies (S): your "use a potion below X% health"
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon policy potion", ACTION_EMERGENCY + 4), NULL)));
}

// lifesavers (S audit, 2026-11-24): every class's escape or survival button, in order, below 25%
// health in a fight. It runs above the flee step (which otherwise ran first and the bot died running
// with Intimidating Shout, Vanish, Ice Block, Divine Protection... still ready). "target": cast on
// the enemy instead of on itself.
namespace
{
    struct Saver { char const* spell; bool target; };
    std::vector<Saver> const& SaversFor(uint8 cls)
    {
        static const std::map<uint8, std::vector<Saver>> savers = {
            { CLASS_PALADIN, { { "divine shield", false }, { "divine protection", false }, { "blessing of protection", false }, { "lay on hands", false } } },
            { CLASS_WARRIOR, { { "intimidating shout", false }, { "shield wall", false }, { "last stand", false }, { "retaliation", false } } },
            { CLASS_ROGUE,   { { "vanish", false }, { "evasion", false }, { "blind", true }, { "gouge", true } } },
            { CLASS_MAGE,    { { "ice block", false }, { "frost nova", false }, { "blink", false }, { "mana shield", false } } },
            { CLASS_PRIEST,  { { "power word: shield", false }, { "psychic scream", false }, { "desperate prayer", false }, { "flash heal", false } } },
            { CLASS_WARLOCK, { { "death coil", true }, { "howl of terror", false }, { "fear", true } } },
            { CLASS_HUNTER,  { { "feign death", false }, { "scatter shot", true }, { "wing clip", true } } },
            { CLASS_DRUID,   { { "frenzied regeneration", false }, { "entangling roots", true }, { "regrowth", false }, { "healing touch", false } } },
            { CLASS_SHAMAN,  { { "lesser healing wave", false }, { "healing wave", false }, { "earthbind totem", false } } } };
        static const std::vector<Saver> none;
        auto it = savers.find(cls);
        return it == savers.end() ? none : it->second;
    }
}

bool LifesaverAction::isUseful()
{
    return bot->IsAlive() && bot->IsInCombat() && bot->GetHealthPercent() < 25.0f;
}

bool LifesaverAction::Execute(Event& event)
{
    Unit* enemy = bot->GetVictim();
    // "current target" holds a guid (asking it for a Unit* crashed the server, 2026-11-24)
    if (!enemy)
        enemy = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    for (Saver const& s : SaversFor(bot->getClass()))
    {
        Unit* on = s.target ? enemy : bot;
        if (!on)
            continue;
        // area spells around the caster (Frost Nova, Psychic Scream, Intimidating Shout...) never
        // passed the cast check on the bot itself - 0 uses in the audit with 86 mages and 92 priests
        // knowing them: try them on the enemy too
        if (!s.target && enemy && !ai->CanCastSpell(s.spell, bot, 0))
            on = enemy;
        if (ai->CanCastSpell(s.spell, on, 0) && ai->CastSpell(s.spell, on))
        {
            sLog.outBasic("Silvermoon: lifesaver: %s (level %u, class %u) used %s at %.0f%% health", bot->GetName(), bot->GetLevel(),
                uint32(bot->getClass()), s.spell, bot->GetHealthPercent());
            return true;
        }
    }
    // a Healthstone, any class
    if (AI_VALUE2(uint32, "item count", "healthstone") > 0 && ai->DoSpecificAction("healthstone", Event("silvermoon lifesaver"), true))
    {
        sLog.outBasic("Silvermoon: lifesaver: %s used a healthstone at %.0f%% health", bot->GetName(), bot->GetHealthPercent());
        return true;
    }
    return false;
}

bool PolicyRotationAction::Pick(std::string& name, Unit*& target)
{
    if (!bot->IsAlive() || !bot->IsInCombat() || !SilvermoonIsOwn(bot) || bot->IsNonMeleeSpellCasted(true))
        return false;
    Unit* enemy = AI_VALUE(Unit*, "current target");
    if (!enemy || !enemy->IsAlive() || !bot->CanAttack(enemy))
        return false;
    for (auto const& [spell, cond] : SilvermoonRotationFor(bot->GetGUIDLow()))
    {
        uint32 id = AI_VALUE2(uint32, "spell id", spell);
        if (!id || !bot->HasSpell(id))
            continue;
        bool positive = IsPositiveSpell(id);
        Unit* t = positive ? (Unit*)bot : enemy;
        if (cond == 1 && ai->HasAura(spell, t, false, !positive))
            continue;                       // keep it up: still on
        if (cond == 2 && enemy->GetHealthPercent() >= 20.0f)
            continue;
        if (cond == 3 && bot->GetHealthPercent() >= 50.0f)
            continue;
        if (cond == 4 && enemy->GetHealthPercent() < 90.0f)
            continue;
        if (!ai->CanCastSpell(id, t, 0))
            continue;
        name = spell;
        target = t;
        return true;
    }
    return false;
}

bool PolicyRotationAction::isUseful()
{
    std::string name;
    Unit* target = nullptr;
    return Pick(name, target);
}

bool PolicyRotationAction::Execute(Event& event)
{
    std::string name;
    Unit* target = nullptr;
    if (!Pick(name, target))
        return false;
    return ai->CastSpell(name, target);
}

bool PolicyPotionAction::isUseful()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || !SilvermoonIsOwn(bot))
        return false;
    SilvermoonPolicy p = SilvermoonPolicyFor(bot->GetGUIDLow());
    uint8 at = p.potionAt ? p.potionAt : 20;
    return bot->GetHealthPercent() < float(at) && AI_VALUE2(uint32, "item count", "healing potion") > 0;
}

bool PolicyPotionAction::Execute(Event& event)
{
    return ai->DoSpecificAction("healing potion", Event("silvermoon policy potion"), true);
}

namespace ai { uint8 SilvermoonTravelRules(Player* bot); float SilvermoonGatherOrderFactor(uint32 guid, uint32 skill); }

bool WalkPastGreysAction::isUseful()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || !SilvermoonIsOwn(bot))
        return false;
    // gathering, with "ignore grey mobs" on (travel bit 2 clear = greys ignored)
    if (!(SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 182) > 1.0f || SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 186) > 1.0f))
        return false;
    if (SilvermoonTravelRules(bot) & 2)
        return false;
    if (bot->GetHealthPercent() < 40.0f)
        return false;                     // hurt: fight it out (the flee and lifesaver steps decide)
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->GetPosition() || target->GetPosition()->getMapId() != bot->GetMapId())
        return false;
    uint32 attackers = 0;
    for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "attackers"))
    {
        Unit* u = ai->GetUnit(guid);
        if (!u || !u->IsAlive())
            continue;
        if (u->GetTypeId() != TYPEID_UNIT || MaNGOS::XP::Gain(bot, (Creature*)u))
            return false;                 // a player, or a mob that still gives XP: the normal fight
        ++attackers;
    }
    return attackers > 0;
}

bool WalkPastGreysAction::Execute(Event& event)
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    WorldPosition* to = target ? target->GetPosition() : nullptr;
    if (!to)
        return false;
    bot->AttackStop();
    SET_AI_VALUE(ObjectGuid, "current target", ObjectGuid());
    return MoveTo(to->getMapId(), to->getX(), to->getY(), to->getZ(), false, false, false, true);
}

bool FleeOutmatchedAction::isUseful()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || bot->InBattleGround())
        return false;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return false;
    bool own = SilvermoonIsOwn(bot);
    if (!own && (ai->HasRealPlayerMaster() || !sRandomPlayerbotMgr.IsRandomBot(bot)))
        return false;
    if (sRandomPlayerbotMgr.SilvermoonDungeonOf(bot->GetGUIDLow()))
        return false;
    // policies (S): your flee rules - N or more mobs on it, or health under X%
    if (own)
    {
        // defaults (S): flee from 2 or more mobs, or below 30% health
        SilvermoonPolicy p = SilvermoonPolicyFor(bot->GetGUIDLow());
        if (!p.fleeHealth) p.fleeHealth = 30;
        if (!p.fleeMobs) p.fleeMobs = 2;
        uint32 n = 0;
        for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "attackers"))
            if (Unit* u = ai->GetUnit(guid))
                if (u->IsAlive() && u->GetTypeId() == TYPEID_UNIT)
                    ++n;
        if (bot->GetHealthPercent() < float(p.fleeHealth) || n >= p.fleeMobs)
        {
            // logged at most every 30 seconds, so the policy can be checked (S: "not firing")
            static std::mutex logLock;
            static std::unordered_map<uint32, time_t> lastLog;
            std::lock_guard<std::mutex> guard(logLock);
            time_t& last = lastLog[bot->GetGUIDLow()];
            if (time(nullptr) - last >= 30)
            {
                last = time(nullptr);
                sLog.outBasic("Silvermoon: %s flee policy: %u mobs on it, %.0f%% health (flee from %u, below %u%%)",
                    bot->GetName(), n, bot->GetHealthPercent(), uint32(p.fleeMobs), uint32(p.fleeHealth));
            }
            return true;
        }
    }
    for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "attackers"))
    {
        Unit* attacker = ai->GetUnit(guid);
        if (attacker && attacker->IsAlive() && attacker->GetTypeId() == TYPEID_UNIT &&
            int32(attacker->GetLevel()) >= int32(bot->GetLevel()) + 4)
            return true;
    }
    return false;
}

// guards and the other faction's NPCs (S, 1.6.20): 458 bot deaths in a day were to guards
// (Astranaar Sentinels 241, Hillsbrad Footmen 51, Ghost Walker Braves 46...), and more to enemy
// quest givers and town NPCs the bots had pulled
uint8 ai::SilvermoonFactionNpcKind(Player* bot, Unit* unit)
{
    if (!bot || !unit || unit->GetTypeId() != TYPEID_UNIT || !unit->GetOwnerGuid().IsEmpty())
        return 0;
    Creature* creature = (Creature*)unit;
    if (creature->IsGuard())
        return 1;
    FactionTemplateEntry const* ft = unit->GetFactionTemplateEntry();
    if (!ft)
        return 0;
    uint32 own = bot->GetTeam() == ALLIANCE ? FACTION_GROUP_MASK_ALLIANCE : FACTION_GROUP_MASK_HORDE;
    uint32 other = bot->GetTeam() == ALLIANCE ? FACTION_GROUP_MASK_HORDE : FACTION_GROUP_MASK_ALLIANCE;
    uint32 sides = ft->factionGroupMask | ft->friendGroupMask;
    return ((sides & other) && !(sides & own)) ? 2 : 0;
}

Unit* FleeGuardAction::GuardOnBot()
{
    for (Unit* attacker : bot->getAttackers())
        if (attacker && attacker->IsAlive() && SilvermoonFactionNpcKind(bot, attacker) == 1)
            return attacker;
    return nullptr;
}

bool FleeGuardAction::isUseful()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || bot->InBattleGround())
        return false;
    return GuardOnBot() != nullptr;
}

bool FleeGuardAction::Execute(Event& event)
{
    Unit* guard = GuardOnBot();
    if (!guard)
        return false;
    {
        // logged at most every 30 seconds per bot
        static std::mutex logLock;
        static std::unordered_map<uint32, time_t> lastLog;
        std::lock_guard<std::mutex> lock(logLock);
        time_t& last = lastLog[bot->GetGUIDLow()];
        if (time(nullptr) - last >= 30)
        {
            last = time(nullptr);
            sLog.outBasic("Silvermoon: %s (level %u) runs from guard %s (entry %u) at %.0f%% health", bot->GetName(), bot->GetLevel(),
                guard->GetName(), guard->GetEntry(), bot->GetHealthPercent());
        }
    }
    bot->AttackStop();
    if (Pet* pet = bot->GetPet())
        pet->AttackStop();
    SET_AI_VALUE(ObjectGuid, "current target", ObjectGuid());
    if (Flee(guard))
        return true;
    // no flee point found: run straight away from the guard
    float angle = guard->GetAngle(bot);
    float x = bot->GetPositionX() + cos(angle) * 30.0f;
    float y = bot->GetPositionY() + sin(angle) * 30.0f;
    return MoveTo(bot->GetMapId(), x, y, bot->GetPositionZ(), false, false, false, true);
}
