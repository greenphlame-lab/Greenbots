#pragma once
#include "playerbot/strategy/Strategy.h"
#include "playerbot/strategy/Multiplier.h"

namespace ai
{
    // Silvermoon brain (Phase 1). While a solo random bot has this strategy, the stock travel
    // cascade (one purpose at a time, dice rolls, nearest first) is switched off and the goal
    // scorer picks the next destination instead. Grouped bots keep the stock behaviour.
    class SilvermoonBrainMultiplier : public Multiplier
    {
    public:
        SilvermoonBrainMultiplier(PlayerbotAI* ai) : Multiplier(ai, "silvermoon brain") {}
        virtual float GetValue(Action* action) override;
    };

    class SilvermoonBrainStrategy : public Strategy
    {
    public:
        SilvermoonBrainStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "silvermoon brain"; }
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatMultipliers(std::list<Multiplier*>& multipliers) override;
    };
}
