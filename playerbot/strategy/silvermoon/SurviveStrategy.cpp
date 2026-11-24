#include "playerbot/playerbot.h"
#include "SurviveStrategy.h"
#include "Policies.h"
#include "playerbot/RandomPlayerbotMgr.h"

using namespace ai;

void SurviveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("silvermoon flee outmatched", ACTION_EMERGENCY + 5), NULL)));
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
        SilvermoonPolicy p = SilvermoonPolicyFor(bot->GetGUIDLow());
        if (p.fleeHealth && bot->GetHealthPercent() < float(p.fleeHealth))
            return true;
        if (p.fleeMobs)
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
