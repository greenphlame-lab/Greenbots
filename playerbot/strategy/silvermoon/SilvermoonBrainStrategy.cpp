#include "playerbot/playerbot.h"
#include "SilvermoonBrainStrategy.h"

using namespace ai;

float SilvermoonBrainMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    // grouped bots keep the stock travel behaviour in Phase 1
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return 1.0f;

    std::string const& name = action->getName();

    if (name.find("request named travel target") == 0)
    {
        // still allowed: guild events, reagent restocks, pvp and mounts (the brain has no goal for
        // those yet)
        for (char const* keep : { "guild meeting", "reagent vendor", "guild order", "pvp", "mount" })
            if (name.find(keep) != std::string::npos)
                return 1.0f;
        return 0.0f;
    }

    if (name.find("request travel target") == 0 || name == "request quest travel target" ||
        name == "choose group travel target" || name == "refresh travel target")
        return 0.0f;

    return 1.0f;
}

void SilvermoonBrainStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
}

void SilvermoonBrainStrategy::InitNonCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new SilvermoonBrainMultiplier(ai));
}
