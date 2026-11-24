#pragma once
#include "playerbot/strategy/Strategy.h"
#include "playerbot/strategy/actions/MovementActions.h"

namespace ai
{
    // "silvermoon survive" (combat, solo random bots): run from a fight with a mob 4 or more
    // levels above the bot (S: deaths were 2,400 an hour; Grashnak died to level 5 scorpids at 1)
    class SurviveStrategy : public Strategy
    {
    public:
        SurviveStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "silvermoon survive"; }
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    // policies (S): drink a healing potion below your set health (default 20%)
    class PolicyPotionAction : public Action
    {
    public:
        PolicyPotionAction(PlayerbotAI* ai) : Action(ai, "silvermoon policy potion") {}
        virtual bool isUseful() override;
        virtual bool Execute(Event& event) override;
    };

    class FleeOutmatchedAction : public FleeAction
    {
    public:
        FleeOutmatchedAction(PlayerbotAI* ai) : FleeAction(ai, 40.0f) { name = "silvermoon flee outmatched"; }
        virtual bool isUseful() override;
    };
}
