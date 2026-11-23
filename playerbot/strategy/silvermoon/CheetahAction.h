#pragma once
#include "playerbot/strategy/actions/GenericSpellActions.h"

namespace ai
{
    // "silvermoon travel cheetah": a solo hunter on a long walk to its goal (not mounted, not in
    // combat, more than 120 yards to go) runs in Aspect of the Cheetah. In combat the hunter's normal
    // aspect triggers (Hawk/Monkey) replace it, so the daze risk is only on the road.
    class TravelCheetahAction : public CastBuffSpellAction
    {
    public:
        TravelCheetahAction(PlayerbotAI* ai) : CastBuffSpellAction(ai, "aspect of the cheetah") { name = "silvermoon travel cheetah"; }
        virtual bool isUseful() override;
    };
}
