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
        // 1.7.2: out of fights, your list's "if not active" buffs on yourself
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    // below 25% health in a fight: the class escape / survival buttons (see SurviveStrategy.cpp)
    class LifesaverAction : public Action
    {
    public:
        LifesaverAction(PlayerbotAI* ai) : Action(ai, "silvermoon lifesaver") {}
        virtual bool isUseful() override;
        virtual bool Execute(Event& event) override;
    };

    // rotation (S, 1.7): your own priority list of abilities, first one that's ready and whose
    // condition holds; nothing ready = the class's own rotation carries on
    class PolicyRotationAction : public Action
    {
    public:
        PolicyRotationAction(PlayerbotAI* ai) : Action(ai, "silvermoon rotation") {}
        virtual bool isUseful() override;
        virtual bool Execute(Event& event) override;
    private:
        bool Pick(std::string& name, Unit*& target);
    };

    // 1.7.2 (S: Blessing of Might "if not active" on the list): out of fights, keep up the buffs on
    // yourself that your list marks "not active"
    class PolicyRotationBuffAction : public Action
    {
    public:
        PolicyRotationBuffAction(PlayerbotAI* ai) : Action(ai, "silvermoon rotation buff") {}
        virtual bool isUseful() override;
        virtual bool Execute(Event& event) override;
    private:
        std::string Pick();
    };

    // policies (S): drink a healing potion below your set health (default 20%)
    class PolicyPotionAction : public Action
    {
    public:
        PolicyPotionAction(PlayerbotAI* ai) : Action(ai, "silvermoon policy potion") {}
        virtual bool isUseful() override;
        virtual bool Execute(Event& event) override;
    };

    // your character on a Gather order with "ignore grey mobs" (S): when only grey mobs are on it, it
    // doesn't fight - it keeps walking its route and lets them give up
    class WalkPastGreysAction : public MovementAction
    {
    public:
        WalkPastGreysAction(PlayerbotAI* ai) : MovementAction(ai, "silvermoon walk past greys") {}
        virtual bool isUseful() override;
        virtual bool Execute(Event& event) override;
    };

    // 1 = a guard (any side), 2 = an NPC of the bot's opposite faction, 0 = anything else
    uint8 SilvermoonFactionNpcKind(Player* bot, Unit* unit);

    // S (1.6.20): a bot a guard is fighting always runs - it never fights it out
    class FleeGuardAction : public FleeAction
    {
    public:
        FleeGuardAction(PlayerbotAI* ai) : FleeAction(ai, 40.0f) { name = "silvermoon flee guard"; }
        virtual bool isUseful() override;
        virtual bool Execute(Event& event) override;
    private:
        Unit* GuardOnBot();
    };

    class FleeOutmatchedAction : public FleeAction
    {
    public:
        FleeOutmatchedAction(PlayerbotAI* ai) : FleeAction(ai, 40.0f) { name = "silvermoon flee outmatched"; }
        virtual bool isUseful() override;
    };
}
