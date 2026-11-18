#include "playerbot/playerbot.h"
#include "ChooseGoalAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "Tools/Formulas.h"
#include <deque>
#include <mutex>

using namespace ai;
using namespace silvermoon;

namespace
{
    const size_t MAX_CANDIDATES = 40;

    // Each bot's total XP over time, to know its own XP per minute (last ~30 minutes).
    std::mutex xpLock;
    std::map<uint32, std::deque<std::pair<time_t, uint64>>> xpSamples;

    uint64 TotalXp(Player* bot)
    {
        uint64 total = bot->GetUInt32Value(PLAYER_XP);
        for (uint32 l = 1; l < bot->GetLevel(); ++l)
            total += sObjectMgr.GetXPForLevel(l);
        return total;
    }

    float XpPerMinute(Player* bot)
    {
        time_t now = time(nullptr);
        uint64 xp = TotalXp(bot);
        std::lock_guard<std::mutex> guard(xpLock);
        auto& samples = xpSamples[bot->GetGUIDLow()];
        if (samples.empty() || now - samples.back().first >= 60)
            samples.push_back({ now, xp });
        while (samples.size() > 1 && now - samples.front().first > 35 * MINUTE)
            samples.pop_front();
        time_t span = now - samples.front().first;
        if (span < 10 * MINUTE || xp < samples.front().second)
            return 0.0f; // unknown yet: the scorer uses its default
        return float(xp - samples.front().second) / (float(span) / 60.0f);
    }

    BotSnapshot Snapshot(Player* bot, PlayerbotAI* ai)
    {
        AiObjectContext* context = ai->GetAiObjectContext();
        BotSnapshot s;
        s.level = bot->GetLevel();
        s.xpPerMinute = XpPerMinute(bot);
        s.mounted = bot->IsMounted();
        s.deathCount = PlayerTravelInfo(bot).GetUint32Value("death count");
        s.urgentRepair = context->GetValue<bool>("should repair")->Get();
        s.urgentVendor = context->GetValue<uint8>("bag space")->Get() > 90;
        return s;
    }

    void MobInfo(int32 entry, int32& level, bool& elite)
    {
        level = 0;
        elite = false;
        if (entry <= 0)
            return;
        if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry))
        {
            level = int32(info->MinLevel + info->MaxLevel) / 2;
            elite = info->Rank == CREATURE_ELITE_ELITE || info->Rank == CREATURE_ELITE_RAREELITE || info->Rank == CREATURE_ELITE_WORLDBOSS;
        }
    }

    float QuestXp(Player* bot, uint32 questId)
    {
        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        if (!quest)
            return 0.0f;
        return quest->XPValue(bot) * sWorld.getConfig(CONFIG_FLOAT_RATE_XP_QUEST);
    }

    uint32 ObjectivesLeft(Player* bot, uint32 questId)
    {
        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        if (!quest)
            return 1;
        auto it = bot->getQuestStatusMap().find(questId);
        if (it == bot->getQuestStatusMap().end())
            return 1;
        QuestStatusData const& status = it->second;
        uint32 left = 0;
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->ReqCreatureOrGOCount[i] && status.m_creatureOrGOcount[i] < quest->ReqCreatureOrGOCount[i])
                ++left;
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->ReqItemCount[i] && status.m_itemcount[i] < quest->ReqItemCount[i])
                ++left;
        return std::max<uint32>(1, left);
    }
}

static SilvermoonScoredGoal ScoreWith(Player* bot, BotSnapshot const& snapshot, TravelDestination* destination, WorldPosition* position, float distance)
{
    SilvermoonScoredGoal goal;
    goal.destination = destination;
    goal.position = position;

    GoalCandidate c;
    c.distance = distance;
    TravelDestinationPurpose purpose = destination->GetPurpose();

    if (QuestRelationTravelDestination* q = dynamic_cast<QuestRelationTravelDestination*>(destination))
    {
        c.kind = purpose == TravelDestinationPurpose::QuestTaker ? GoalKind::QuestTurnIn : GoalKind::QuestGiver;
        c.questXp = QuestXp(bot, q->GetQuestId());
    }
    else if (QuestObjectiveTravelDestination* o = dynamic_cast<QuestObjectiveTravelDestination*>(destination))
    {
        c.kind = GoalKind::QuestObjective;
        c.questXp = QuestXp(bot, o->GetQuestId());
        c.objectivesLeft = ObjectivesLeft(bot, o->GetQuestId());
        MobInfo(o->GetEntry(), c.mobLevel, c.elite);
    }
    else if (dynamic_cast<GrindTravelDestination*>(destination))
    {
        c.kind = GoalKind::Grind;
        MobInfo(destination->GetEntry(), c.mobLevel, c.elite);
        if (c.mobLevel > 0)
            c.killXp = MaNGOS::XP::BaseGain(bot->GetLevel(), uint32(c.mobLevel)) * (c.elite ? 2.5f : 1.0f) * sWorld.getConfig(CONFIG_FLOAT_RATE_XP_KILL);
    }
    else if (dynamic_cast<ExploreTravelDestination*>(destination))
        c.kind = GoalKind::Explore;
    else if (purpose == TravelDestinationPurpose::Vendor)
        c.kind = GoalKind::Vendor;
    else if (purpose == TravelDestinationPurpose::Repair)
        c.kind = GoalKind::Repair;
    else if (purpose == TravelDestinationPurpose::Trainer)
        c.kind = GoalKind::Trainer;

    goal.kind = c.kind;
    goal.score = GoalScorer::Score(snapshot, c, frand(0.9f, 1.1f));
    return goal;
}

SilvermoonScoredGoal ai::SilvermoonScoreOne(Player* bot, PlayerbotAI* ai, TravelDestination* destination, WorldPosition* position, float distance)
{
    return ScoreWith(bot, Snapshot(bot, ai), destination, position, distance);
}

std::vector<SilvermoonScoredGoal> ai::SilvermoonScoreGoals(Player* bot, PlayerbotAI* ai, PartitionedTravelList& list)
{
    std::vector<SilvermoonScoredGoal> goals;
    std::set<TravelDestination*> seen;
    PlayerTravelInfo info(bot);
    BotSnapshot snapshot = Snapshot(bot, ai);

    // partitions are ordered by distance, so this walks nearest first
    for (auto& [partition, points] : list)
    {
        for (auto& [destination, position, distance] : points)
        {
            if (goals.size() >= MAX_CANDIDATES)
                break;
            if (!destination || !seen.insert(destination).second)
                continue;
            if (!destination->IsActive(bot, info))
                continue;
            goals.push_back(ScoreWith(bot, snapshot, destination, position, distance));
        }
        if (goals.size() >= MAX_CANDIDATES)
            break;
    }
    std::sort(goals.begin(), goals.end(), [](SilvermoonScoredGoal const& a, SilvermoonScoredGoal const& b) { return a.score.score > b.score.score; });
    return goals;
}

bool ChooseGoalAction::Execute(Event& event)
{
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    if (travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    FutureDestinations* futureDestinations = AI_VALUE(FutureDestinations*, "future travel destinations");
    if (!futureDestinations->valid())
    {
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
        return false;
    }
    if (futureDestinations->wait_for(std::chrono::seconds(0)) == std::future_status::timeout)
        return false;

    PartitionedTravelList list = futureDestinations->get();
    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);

    std::vector<SilvermoonScoredGoal> goals = SilvermoonScoreGoals(bot, ai, list);
    if (goals.empty() || goals.front().score.score <= 0.0f)
    {
        // nothing worth doing nearby: try again in 30 seconds (the stock reset/grind still run)
        SET_AI_VALUE2(int, "manual int", "silvermoon goal retry at", int(time(nullptr) + 30));
        return false;
    }

    SilvermoonScoredGoal const& best = goals.front();
    TravelTarget newTarget = TravelTarget(ai);
    newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
    newTarget.SetTarget(best.destination, best.position);
    Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
    setNewTarget(requester, &newTarget, travelTarget);

    // remembered for the 5-minute re-check
    SET_AI_VALUE2(int, "manual int", "silvermoon goal score", int(best.score.score * 1000.0f));
    SET_AI_VALUE2(int, "manual int", "silvermoon goal check at", int(time(nullptr) + 5 * MINUTE));
    return true;
}

bool ChooseGoalAction::isUseful()
{
    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;
    if (AI_VALUE2(std::string, "manual string", "future travel purpose") != "goal")
        return false;
    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;
    return true;
}
