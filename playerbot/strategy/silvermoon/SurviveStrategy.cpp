#include "playerbot/playerbot.h"
#include "SurviveStrategy.h"
#include "Policies.h"
#include <map>
#include <vector>
#include "playerbot/RandomPlayerbotMgr.h"

using namespace ai;

void SurviveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon flee outmatched", ACTION_EMERGENCY + 5), NULL)));
    // lifesavers for every class (S: Mommy, a paladin, died to a level 6 Kobold Miner with Divine
    // Protection, Blessing of Protection and Lay on Hands all ready) - above the flee step
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon lifesaver", ACTION_EMERGENCY + 6), NULL)));
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
    if (!enemy)
        enemy = AI_VALUE(Unit*, "current target");
    for (Saver const& s : SaversFor(bot->getClass()))
    {
        Unit* on = s.target ? enemy : bot;
        if (!on)
            continue;
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
        if (bot->GetHealthPercent() < float(p.fleeHealth))
            return true;
        {
            uint32 n = 0;
            for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "attackers"))
                if (Unit* u = ai->GetUnit(guid))
                    if (u->IsAlive() && u->GetTypeId() == TYPEID_UNIT)
                        ++n;
            if (n >= p.fleeMobs)
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
