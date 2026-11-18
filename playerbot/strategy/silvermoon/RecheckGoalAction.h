#pragma once
#include "playerbot/strategy/actions/ChooseTravelTargetAction.h"

namespace ai
{
    // "recheck goal" (brain step 6): every 5 minutes while a goal is active, fetch and score a
    // fresh list and switch only if the best option scores at least 1.25x the current goal.
    // Urgent chores (needs repair, bags over 90%) switch without the margin.
    class RecheckGoalAction : public ChooseTravelTargetAction
    {
    public:
        RecheckGoalAction(PlayerbotAI* ai) : ChooseTravelTargetAction(ai, "recheck goal") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };
}
