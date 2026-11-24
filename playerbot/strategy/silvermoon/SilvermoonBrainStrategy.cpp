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
    // no destination: fetch everything worth doing in one go (above every stock request)
    triggers.push_back(new TriggerNode(
        "val::not::travel target active",
        NextAction::array(0, new NextAction("request goal destinations", 6.97f), NULL)));

    // the goal list is ready: score it and pick (only for lists fetched by the brain)
    triggers.push_back(new TriggerNode(
        "val::not::travel target active",
        NextAction::array(0, new NextAction("choose goal", 6.985f), NULL)));

    // the goal is a quest giver and the bot is near it: walk up and take its quests
    triggers.push_back(new TriggerNode(
        "val::travel target active",
        NextAction::array(0, new NextAction("silvermoon take quests", 6.995f), NULL)));

    // long trips with the class's movement ability (Cheetah, Ghost Wolf, Travel Form, Sprint)
    triggers.push_back(new TriggerNode(
        "val::travel target active",
        NextAction::array(0, new NextAction("silvermoon travel speed", 6.996f), NULL)));

    // goal by the hearthstone inn and far away: hearth instead of walking (before the speed buff)
    triggers.push_back(new TriggerNode(
        "val::travel target active",
        NextAction::array(0, new NextAction("silvermoon hearth shortcut", 6.997f), NULL)));

    // vendor / repair errand done: end the goal now instead of wandering by the NPC until it times out
    triggers.push_back(new TriggerNode(
        "val::travel target active",
        NextAction::array(0, new NextAction("silvermoon errand done", 6.998f), NULL)));

    // every 5 minutes with a goal: is something clearly better now? (switch at 1.25x, chores sooner)
    triggers.push_back(new TriggerNode(
        "val::travel target active",
        NextAction::array(0, new NextAction("recheck goal", 6.99f), NULL)));
}

void SilvermoonBrainStrategy::InitNonCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new SilvermoonBrainMultiplier(ai));
}
