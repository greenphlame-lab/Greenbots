#pragma once
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/values/TravelValues.h"

namespace ai
{
    // One fetch in a goal list: a destination purpose (flags), optional entries, and a range.
    struct SilvermoonGoalFetch
    {
        uint32 purpose;
        std::vector<int32> entries;
        float range;
    };

    // Everything a solo bot could reasonably do next, as one list of fetches: its quests' turn-ins
    // and objectives, quest givers nearby, grind spots, and chores (vendor, repair, class trainer)
    // when they are due, plus a little exploring.
    std::vector<SilvermoonGoalFetch> SilvermoonBuildGoalFetches(PlayerbotAI* ai, Player* bot);

    // Runs the fetches off the world thread and merges them into one partitioned list.
    FutureDestinations SilvermoonLaunchGoalFetch(Player* bot, std::vector<SilvermoonGoalFetch> fetches);

    // "request goal destinations": starts the async fetch for the goal scorer (brain step 4).
    class RequestGoalAction : public Action
    {
    public:
        RequestGoalAction(PlayerbotAI* ai) : Action(ai, "request goal destinations") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };
}
