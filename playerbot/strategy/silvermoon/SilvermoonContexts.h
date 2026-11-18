#pragma once
#include "playerbot/strategy/NamedObjectContext.h"
#include "SilvermoonBrainStrategy.h"
#include "RequestGoalAction.h"
#include "ChooseGoalAction.h"
#include "RecheckGoalAction.h"

namespace ai
{
    // Silvermoon strategies, actions and values, added next to the stock contexts in
    // AiObjectContext's constructor.
    class SilvermoonStrategyContext : public NamedObjectContext<Strategy>
    {
    public:
        SilvermoonStrategyContext()
        {
            creators["silvermoon brain"] = [](PlayerbotAI* ai) { return new SilvermoonBrainStrategy(ai); };
        }
    };

    class SilvermoonActionContext : public NamedObjectContext<Action>
    {
    public:
        SilvermoonActionContext()
        {
            creators["request goal destinations"] = [](PlayerbotAI* ai) { return new RequestGoalAction(ai); };
            creators["choose goal"] = [](PlayerbotAI* ai) { return new ChooseGoalAction(ai); };
            creators["recheck goal"] = [](PlayerbotAI* ai) { return new RecheckGoalAction(ai); };
        }
    };
}
