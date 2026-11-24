#pragma once
#include "playerbot/strategy/actions/GenericActions.h"

namespace ai
{
    // Silvermoon auto mode, layer 1 (S, 2026-11-23): whisper "auto on", "auto off" or "auto" to one of
    // your own always-online characters. With auto on (and not grouped with you) it plays itself on
    // the same brain as the random bots; off, it goes back to its normal behaviour. Only the owner of
    // the account can switch it. Stored in characters.silvermoon_auto.
    class SilvermoonAutoAction : public ChatCommandAction
    {
    public:
        SilvermoonAutoAction(PlayerbotAI* ai) : ChatCommandAction(ai, "auto") {}
        virtual bool Execute(Event& event) override;
    };

    // "why": the bot whispers its current goal, the score, the main reason, and the two runners-up
    // it passed on. Works for any bot that runs the brain.
    class SilvermoonWhyAction : public ChatCommandAction
    {
    public:
        SilvermoonWhyAction(PlayerbotAI* ai) : ChatCommandAction(ai, "why") {}
        virtual bool Execute(Event& event) override;
    };
}
