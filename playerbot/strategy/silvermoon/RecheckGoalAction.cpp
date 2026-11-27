#include "playerbot/playerbot.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include "RecheckGoalAction.h"
#include "RequestGoalAction.h"
#include "ChooseGoalAction.h"
#include "Professions.h"
#include "playerbot/strategy/values/TravelValues.h"
#include <mutex>
#include <unordered_map>
#include <cmath>

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
    silvermoon::BrainTimer brainTimer(silvermoon::BrainMicros());
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
    // a relocation trip (Phase 2b) is not re-scored against local goals on the way: it would lose
    // every time, since its value is the hub, not the road
    if (dynamic_cast<TemporaryTravelDestination*>(current->GetDestination()))
        return false;   // (also a gather order's spot: S wants the patrol uninterrupted)

    PartitionedTravelList list = future.get();
    std::vector<SilvermoonScoredGoal> goals = SilvermoonScoreGoals(bot, ai, list);
    if (goals.empty())
        return false;

    SilvermoonScoredGoal now = SilvermoonScoreOne(bot, ai, current->GetDestination(), current->GetPosition(), current->Distance(bot));
    SilvermoonScoredGoal const& best = goals.front();
    if (best.destination == current->GetDestination())
        return false;
    // never switch to a goal worth nothing (grey mobs scored 0 and still won against a 0-score
    // current goal, so the bot hopped between grey grind spots)
    if (best.score.score <= 0.0f)
    {
        sRandomPlayerbotMgr.SilvermoonCheckLevelZoneNow(bot->GetGUIDLow());
        return false;
    }

    bool urgent = (best.kind == GoalKind::Repair && AI_VALUE(bool, "should repair") && AI_VALUE(bool, "can repair")) ||
                  (best.kind == GoalKind::Vendor && AI_VALUE(uint8, "bag space") > 90);
    if (!urgent && best.score.score < now.score.score * SWITCH_MARGIN)
        return false;

    // logged as a switch, with the goal it replaced as the first runner-up
    SilvermoonLogDecision(bot, best, &now, goals.size() > 1 ? &goals[1] : nullptr, urgent ? "switch (urgent): " : "switch: ");

    TravelTarget newTarget = TravelTarget(ai);
    newTarget.SetRelevance(current->GetRelevance());
    newTarget.SetTarget(best.destination, best.position);
    Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
    // The bot keeps one travel target object for its whole life, and SetTarget/CopyTarget never
    // clear its conditions: a finished quest objective's "need quest objective" condition stayed
    // on, so every later goal went straight to cooldown and the bot chose again within seconds.
    current->SetConditions({});
    // ...and setNewTarget re-adds the stock "future travel condition" left by the bot's last stock
    // travel request (e.g. "should travel named::reagent vendor"): false for a brain goal, so a
    // relocation trip went straight to cooldown and was re-picked every ~5 seconds. Cleared.
    SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
    setNewTarget(requester, &newTarget, current);
    SET_AI_VALUE2(int, "manual int", "silvermoon goal score", int(best.score.score * 1000.0f));
    SET_AI_VALUE2(int, "manual int", "silvermoon goal kind", int(best.kind) + 1);
    return true;
}

bool RecheckGoalAction::isUseful()
{
    silvermoon::BrainTimer brainTimer(silvermoon::BrainMicros());
    // Resurrection Sickness (S, 1.6.20): whatever goal the bot had (a quest objective it went back to
    // after the spirit healer, a grind spot...) ends, so "choose goal" sends it to an inn to wait
    if (SilvermoonResSickRestApplies(bot) && !bot->IsInCombat() && !SilvermoonCatchUpSkill(bot->GetGUIDLow()))
    {
        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
        // ...but a Go to trip you ordered goes ahead (1.7.6: ending it here made the brain choose it
        // again every few seconds)
        if (target && target->IsActive() && !SilvermoonIsResSickTrip(bot->GetGUIDLow(), target->GetDestination()) &&
            !SilvermoonGoIsYours(bot))
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        return false;
    }
    // Gather order patrol (S: "if nothing at a spawn point, continue on; if a herb is visible, gather
    // it"): at the spot (12 yards) with nothing to gather in the 60-yard scan, out of combat and not
    // casting, the stop is over - the trip ends and the brain picks the next unvisited spot at once.
    // Without this the bot stood at an empty spot until the trip's timer ran out (1.6.11, Mommy).
    // (not while on a Go to trip: 1.7.6, Minzu re-planned her trip to the Skinning trainer 188 times
    // in an hour - near the trainer this took the trip for a finished gather stop and ended it)
    if ((SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 182) > 1.0f || SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 186) > 1.0f) &&
        !SilvermoonGoActive(bot))
    {
        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
        if (target && target->GetPosition() && dynamic_cast<TemporaryTravelDestination*>(target->GetDestination()) &&
            target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED && target->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE &&
            target->GetPosition()->getMapId() == bot->GetMapId() &&
            !bot->IsInCombat() && !bot->IsNonMeleeSpellCasted(false) && !bot->GetLootGuid())
        {
            // something to loot or gather in sight holds the stop - for 45 seconds at most (S, 1.6.21:
            // Mommy stood 4 yards from stop 38 for the full 5 minutes, below a herb on a slope she
            // couldn't climb to)
            bool lootWaiting = AI_VALUE(bool, "has available loot");
            // at the stop: 20 yards on the map (S, 1.6.15: Mommy stood 5 yards below a spawn point on
            // a hilltop - 3D distance kept her "not there" until the 5-minute timer), or 45 seconds
            // within 60 yards of it without getting there (a slope she can't climb). Nodes in sight
            // are picked by the 60-yard gathering scan either way.
            static std::mutex stopLock;
            static std::unordered_map<uint32, std::pair<TravelDestination*, time_t>> stopSince;
            WorldPosition here(bot);
            float dx = target->GetPosition()->getX() - here.getX(), dy = target->GetPosition()->getY() - here.getY();
            float flat = std::sqrt(dx * dx + dy * dy);
            bool done = flat < 20.0f;
            {
                std::lock_guard<std::mutex> guard(stopLock);
                auto& since = stopSince[bot->GetGUIDLow()];
                if (since.first != target->GetDestination() || flat >= 60.0f)
                    since = { target->GetDestination(), 0 };
                if (!since.second && flat < 60.0f)
                    since.second = time(nullptr);
                if (!done && since.second && time(nullptr) - since.second > 45)
                    done = true;
            }
            if (done && lootWaiting)
            {
                std::lock_guard<std::mutex> guard(stopLock);
                auto& since = stopSince[bot->GetGUIDLow()];
                if (!since.second || time(nullptr) - since.second <= 45)
                    done = false;
            }
            if (done)
                target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        }
    }
    if (!ai->AllowActivity(TRAVEL_ACTIVITY) || bot->IsInCombat())
        return false;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return false;
    if (!AI_VALUE(TravelTarget*, "travel target")->IsActive())
        return false;
    // the first check is due 5 minutes after the goal was chosen ("choose goal" sets the time).
    // A bot still on a travel target the brain never chose (set before login, or by stock
    // code) has no check time; check it right away, or it keeps that target forever and
    // never makes a decision (Fyrieth, Velyna).
    int checkAt = AI_VALUE2(int, "manual int", "silvermoon goal check at");
    return !checkAt || time(nullptr) >= checkAt;
}
