#pragma once
#include "playerbot/strategy/Action.h"

namespace ai
{
    // "silvermoon travel speed": on a long solo trip (not mounted, not in combat, 120+ yards to go)
    // a bot uses its class's movement ability (S): hunter Aspect of the Cheetah, shaman Ghost Wolf,
    // druid Travel Form, rogue Sprint (when off cooldown). At most one try a minute, so it doesn't
    // fight other out-of-combat buffs. Combat behaviour takes over as usual once a fight starts.
    // "silvermoon hearth shortcut" (S: Mommy walked from Goldshire back to Northshire with her
    // hearthstone ready and bound there): when the goal is near the bot's hearthstone inn and the bot
    // is far from both (450+ yards, or another map), it hearths instead of walking.
    // "silvermoon errand done" (S: Mommy kept "sell items to Godric" after selling and wandered around
    // the vendor until the goal timed out): a vendor goal ends once the bags are under 40% full, a
    // repair goal once the gear is above 90% durability - the brain then picks the next goal at once
    class ErrandDoneAction : public Action
    {
    public:
        ErrandDoneAction(PlayerbotAI* ai) : Action(ai, "silvermoon errand done") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };

    class HearthShortcutAction : public Action
    {
    public:
        HearthShortcutAction(PlayerbotAI* ai) : Action(ai, "silvermoon hearth shortcut") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };

    class TravelSpeedAction : public Action
    {
    public:
        TravelSpeedAction(PlayerbotAI* ai) : Action(ai, "silvermoon travel speed") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;

    private:
        bool Pick(std::string& action, uint32& spell, std::string& aura);
    };
}
