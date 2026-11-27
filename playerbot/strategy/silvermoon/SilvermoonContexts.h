#pragma once
#include "playerbot/strategy/NamedObjectContext.h"
#include "SilvermoonBrainStrategy.h"
#include "RequestGoalAction.h"
#include "ChooseGoalAction.h"
#include "RecheckGoalAction.h"
#include "TakeQuestsAction.h"
#include "SurviveStrategy.h"
#include "CheetahAction.h"
#include "Professions.h"

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
            creators["silvermoon survive"] = [](PlayerbotAI* ai) { return new SurviveStrategy(ai); };
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
            creators["silvermoon take quests"] = [](PlayerbotAI* ai) { return new TakeQuestsAction(ai); };
            creators["silvermoon flee outmatched"] = [](PlayerbotAI* ai) { return new FleeOutmatchedAction(ai); };
            creators["silvermoon flee guard"] = [](PlayerbotAI* ai) { return new FleeGuardAction(ai); };
            creators["silvermoon walk past greys"] = [](PlayerbotAI* ai) { return new WalkPastGreysAction(ai); };
            creators["silvermoon policy potion"] = [](PlayerbotAI* ai) { return new PolicyPotionAction(ai); };
            creators["silvermoon rotation"] = [](PlayerbotAI* ai) { return new PolicyRotationAction(ai); };
            creators["silvermoon lifesaver"] = [](PlayerbotAI* ai) { return new LifesaverAction(ai); };
            creators["silvermoon travel speed"] = [](PlayerbotAI* ai) { return new TravelSpeedAction(ai); };
            creators["silvermoon hearth shortcut"] = [](PlayerbotAI* ai) { return new HearthShortcutAction(ai); };
            creators["silvermoon errand done"] = [](PlayerbotAI* ai) { return new ErrandDoneAction(ai); };
            creators["silvermoon profession learn"] = [](PlayerbotAI* ai) { return new ProfessionLearnAction(ai); };
        }
    };
}
