#pragma once
#include "playerbot/strategy/Action.h"

namespace ai
{
    // "silvermoon travel speed": on a long solo trip (not mounted, not in combat, 120+ yards to go)
    // a bot uses its class's movement ability (S): hunter Aspect of the Cheetah, shaman Ghost Wolf,
    // druid Travel Form, rogue Sprint (when off cooldown). At most one try a minute, so it doesn't
    // fight other out-of-combat buffs. Combat behaviour takes over as usual once a fight starts.
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
