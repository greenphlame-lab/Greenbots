#include "playerbot/playerbot.h"
#include "SurviveStrategy.h"
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
    if (!bot->IsAlive() || !bot->IsInCombat() || bot->InBattleGround() || ai->HasRealPlayerMaster())
        return false;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return false;
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) || sRandomPlayerbotMgr.SilvermoonDungeonOf(bot->GetGUIDLow()))
        return false;
    for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "attackers"))
    {
        Unit* attacker = ai->GetUnit(guid);
        if (attacker && attacker->IsAlive() && attacker->GetTypeId() == TYPEID_UNIT &&
            int32(attacker->GetLevel()) >= int32(bot->GetLevel()) + 4)
            return true;
    }
    return false;
}
