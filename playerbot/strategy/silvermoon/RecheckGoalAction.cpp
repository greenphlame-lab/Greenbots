#include "playerbot/playerbot.h"
#include "RecheckGoalAction.h"
#include "RequestGoalAction.h"
#include "ChooseGoalAction.h"
#include "playerbot/strategy/values/TravelValues.h"
#include <mutex>

using namespace ai;
using namespace silvermoon;

namespace
{
    const float SWITCH_MARGIN = 1.25f;
    const int RECHECK_SECONDS = 5 * MINUTE;

    // one pending re-check fetch per bot (kept apart from the bot's normal travel fetch)
    std::mutex pendingLock;
    std::map<uint32, FutureDestinations> pending;
}

bool RecheckGoalAction::Execute(Event& event)
{
    uint32 guid = bot->GetGUIDLow();
    FutureDestinations future;
    {
        std::lock_guard<std::mutex> guard(pendingLock);
        auto it = pending.find(guid);
        if (it == pending.end())
        {
            pending[guid] = SilvermoonLaunchGoalFetch(bot, SilvermoonBuildGoalFetches(ai, bot));
            return true;
        }
        if (!it->second.valid() || it->second.wait_for(std::chrono::seconds(0)) == std::future_status::timeout)
            return false;
        future = std::move(it->second);
        pending.erase(it);
    }

    SET_AI_VALUE2(int, "manual int", "silvermoon goal check at", int(time(nullptr) + RECHECK_SECONDS));

    TravelTarget* current = AI_VALUE(TravelTarget*, "travel target");
    if (!current->IsActive() || !current->GetDestination() || !current->GetPosition())
        return false;

    PartitionedTravelList list = future.get();
    std::vector<SilvermoonScoredGoal> goals = SilvermoonScoreGoals(bot, ai, list);
    if (goals.empty())
        return false;

    SilvermoonScoredGoal now = SilvermoonScoreOne(bot, ai, current->GetDestination(), current->GetPosition(), current->Distance(bot));
    SilvermoonScoredGoal const& best = goals.front();
    if (best.destination == current->GetDestination())
        return false;

    bool urgent = (best.kind == GoalKind::Repair && AI_VALUE(bool, "should repair")) ||
                  (best.kind == GoalKind::Vendor && AI_VALUE(uint8, "bag space") > 90);
    if (!urgent && best.score.score < now.score.score * SWITCH_MARGIN)
        return false;

    // logged as a switch, with the goal it replaced as the first runner-up
    SilvermoonLogDecision(bot, best, &now, goals.size() > 1 ? &goals[1] : nullptr, urgent ? "switch (urgent): " : "switch: ");

    TravelTarget newTarget = TravelTarget(ai);
    newTarget.SetRelevance(current->GetRelevance());
    newTarget.SetTarget(best.destination, best.position);
    Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
    setNewTarget(requester, &newTarget, current);
    SET_AI_VALUE2(int, "manual int", "silvermoon goal score", int(best.score.score * 1000.0f));
    return true;
}

bool RecheckGoalAction::isUseful()
{
    if (!ai->AllowActivity(TRAVEL_ACTIVITY) || bot->IsInCombat())
        return false;
    if (!AI_VALUE(TravelTarget*, "travel target")->IsActive())
        return false;
    // the first check is due 5 minutes after the goal was chosen ("choose goal" sets the time)
    int checkAt = AI_VALUE2(int, "manual int", "silvermoon goal check at");
    return checkAt && time(nullptr) >= checkAt;
}
