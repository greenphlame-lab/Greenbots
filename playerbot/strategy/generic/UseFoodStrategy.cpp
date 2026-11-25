
#include "playerbot/playerbot.h"
#include "UseFoodStrategy.h"

using namespace ai;

void UseFoodStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "critical health",
        NextAction::array(0, new NextAction("food", 3.0f), NULL)));
        
    triggers.push_back(new TriggerNode(
        "low health",
        NextAction::array(0, new NextAction("food", 3.0f), NULL)));

    // Silvermoon patch: top up before the next pull, not halfway into it (deaths were 2,400 an
    // hour, 64% of them to mobs at or below the bot's level)
    triggers.push_back(new TriggerNode(
        "medium health",
        NextAction::array(0, new NextAction("food", 3.0f), NULL)));

    // ...and from 80% (S): the "should eat" check decides; this only wakes the food action up
    triggers.push_back(new TriggerNode(
        "almost full health",
        NextAction::array(0, new NextAction("food", 3.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "high mana",
        NextAction::array(0, new NextAction("drink", 3.0f), NULL)));
}
