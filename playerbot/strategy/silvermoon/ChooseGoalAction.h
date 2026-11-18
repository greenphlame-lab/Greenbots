#pragma once
#include "playerbot/strategy/actions/ChooseTravelTargetAction.h"
#include "playerbot/silvermoon/GoalScorer.h"

namespace ai
{
    // One scored destination.
    struct SilvermoonScoredGoal
    {
        TravelDestination* destination = nullptr;
        WorldPosition* position = nullptr;
        silvermoon::GoalKind kind = silvermoon::GoalKind::Other;
        silvermoon::GoalScore score;
    };

    // Flattens a fetched list nearest-first, keeps active destinations (at most 40, one per
    // destination), and scores them. Sorted best first.
    std::vector<SilvermoonScoredGoal> SilvermoonScoreGoals(Player* bot, PlayerbotAI* ai, PartitionedTravelList& list);

    // Scores a single destination at a given distance (used to re-score the current goal).
    SilvermoonScoredGoal SilvermoonScoreOne(Player* bot, PlayerbotAI* ai, TravelDestination* destination, WorldPosition* position, float distance);

    // "choose goal": picks the best-scoring destination from the goal list (brain step 5).
    class ChooseGoalAction : public ChooseTravelTargetAction
    {
    public:
        ChooseGoalAction(PlayerbotAI* ai) : ChooseTravelTargetAction(ai, "choose goal") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };
}
