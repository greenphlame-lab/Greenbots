#pragma once
#include "playerbot/strategy/actions/MovementActions.h"

namespace ai
{
    // "silvermoon take quests": the brain chose a quest giver as the goal; once the bot is near
    // that NPC it walks up and accepts what the NPC offers (and hands in finished quests). The
    // stock path (wander-and-chat "rpg start quest") often never fired: new bots visited quest
    // giver after quest giver and stayed at 0 quests (Grashnak, Fizzlewick).
    class TakeQuestsAction : public MovementAction
    {
    public:
        TakeQuestsAction(PlayerbotAI* ai) : MovementAction(ai, "silvermoon take quests") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;

    private:
        Unit* FindGiver();
    };
}
