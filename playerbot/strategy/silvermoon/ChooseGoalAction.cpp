#include "playerbot/playerbot.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include "ChooseGoalAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "Tools/Formulas.h"
#include "playerbot/silvermoon/DecisionLog.h"
#include "playerbot/silvermoon/QuestIndex.h"
#include "playerbot/silvermoon/Crowd.h"
#include "RelocateGoal.h"
#include "Mails/Mail.h"
#include "playerbot/PlayerbotAIConfig.h"
#include <deque>
#include <mutex>

using namespace ai;
using namespace silvermoon;

namespace
{
    const size_t MAX_CANDIDATES = 40;
    const time_t ABANDONED_WITHIN = 4 * MINUTE;   // a goal replaced this soon was never reached
    const time_t SKIP_FOR = 30 * MINUTE;          // ... and is left out of the next lists for a while

    // Destinations each bot gave up on (chosen, then a new goal was needed within a few minutes:
    // unreachable, no path, wrong side of a wall). Zugzug picked a different far-away vendor every
    // minute for 12 minutes without moving.
    std::mutex goalLock;
    std::map<uint32, std::pair<TravelDestination*, time_t>> lastGoal;   // bot -> last chosen, when
    std::map<uint32, std::map<TravelDestination*, time_t>> skipUntil;   // bot -> destination -> until

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
        float rate = float(xp - samples.front().second) / (float(span) / 60.0f);
        SilvermoonNoteXpRate(bot->GetGUIDLow(), bot->GetLevel(), rate);   // feeds the typical rate per level
        return rate;
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
        s.bagFill = context->GetValue<uint8>("bag space")->Get();
        s.urgentVendor = s.bagFill > 90;
        s.playerMail = SilvermoonHasPlayerMail(bot);
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

bool ai::SilvermoonHasPlayerMail(Player* bot)
{
    time_t now = time(nullptr);
    for (PlayerMails::iterator itr = bot->GetMailBegin(); itr != bot->GetMailEnd(); ++itr)
    {
        Mail const* mail = *itr;
        if (mail->state == MAIL_STATE_DELETED || now < mail->deliver_time || (!mail->has_items && !mail->money))
            continue;
        if (mail->messageType == MAIL_NORMAL && mail->sender && !sRandomPlayerbotMgr.IsRandomBot(uint32(mail->sender)))
            return true;
    }
    return false;
}

void ai::SilvermoonSkipDestination(uint32 guid, TravelDestination* destination, time_t seconds)
{
    std::lock_guard<std::mutex> guard(goalLock);
    skipUntil[guid][destination] = time(nullptr) + seconds;
}

static SilvermoonScoredGoal ScoreWith(Player* bot, BotSnapshot const& snapshot, TravelDestination* destination, WorldPosition* position, float distance)
{
    SilvermoonScoredGoal goal;
    goal.destination = destination;
    goal.position = position;

    GoalCandidate c;
    c.distance = distance;
    TravelDestinationPurpose purpose = destination->GetPurpose();

    // Phase 2a: the quest index adds chain value, avoid flags and the real mob levels and elites
    // of the objective area. Only with AiPlayerbot.SilvermoonSpread = 1.
    bool useIndex = sPlayerbotAIConfig.silvermoonSpread && QuestIndex::Loaded();
    float xpRate = sWorld.getConfig(CONFIG_FLOAT_RATE_XP_QUEST);

    if (QuestRelationTravelDestination* q = dynamic_cast<QuestRelationTravelDestination*>(destination))
    {
        c.kind = purpose == TravelDestinationPurpose::QuestTaker ? GoalKind::QuestTurnIn : GoalKind::QuestGiver;
        c.questXp = QuestXp(bot, q->GetQuestId());
        if (useIndex)
            if (QuestIndexEntry const* entry = QuestIndex::Get(q->GetQuestId()))
            {
                c.chainXp = entry->chainXp * xpRate;
                c.avoid = (entry->flags & QI_AVOID) != 0;
            }
    }
    else if (QuestObjectiveTravelDestination* o = dynamic_cast<QuestObjectiveTravelDestination*>(destination))
    {
        c.kind = GoalKind::QuestObjective;
        c.questXp = QuestXp(bot, o->GetQuestId());
        c.objectivesLeft = ObjectivesLeft(bot, o->GetQuestId());
        MobInfo(o->GetEntry(), c.mobLevel, c.elite);
        if (useIndex)
            if (QuestIndexEntry const* entry = QuestIndex::Get(o->GetQuestId()))
            {
                c.avoid = (entry->flags & QI_AVOID) != 0;
                if (position)
                    if (QuestArea const* area = QuestIndex::NearestArea(o->GetQuestId(), uint8(o->GetObjective() + 1), position->getMapId(), position->getX(), position->getY()))
                    {
                        if (area->maxMobLevel)
                            c.mobLevel = int32(area->minMobLevel + area->maxMobLevel) / 2;
                        // an area counts as elite when at least a third of its spawns are elites
                        c.elite = area->elites && area->elites * 3 >= area->spawns;
                    }
            }
    }
    else if (dynamic_cast<GrindTravelDestination*>(destination))
    {
        c.kind = GoalKind::Grind;
        MobInfo(destination->GetEntry(), c.mobLevel, c.elite);
        // rare spawns (one mob, often not up) are no grind spot either: treated like elites,
        // which the scorer never picks for grinding
        if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(destination->GetEntry()))
            if (info->Rank == CREATURE_ELITE_RARE || info->Rank == CREATURE_ELITE_RAREELITE)
                c.elite = true;
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
    else if (purpose == TravelDestinationPurpose::Mail)
        c.kind = GoalKind::Mail;

    // crowding (Phase 2a): bots already heading for or working in this 100-yard cell. Capacity is
    // 3 bots for a grind camp, and a third of the spawns (2 to 6) for an indexed objective area.
    if (sPlayerbotAIConfig.silvermoonSpread && position && (c.kind == GoalKind::Grind || c.kind == GoalKind::QuestObjective))
    {
        float capacity = 3.0f;
        if (QuestObjectiveTravelDestination* o = dynamic_cast<QuestObjectiveTravelDestination*>(destination))
            if (QuestArea const* area = QuestIndex::NearestArea(o->GetQuestId(), uint8(o->GetObjective() + 1), position->getMapId(), position->getX(), position->getY()))
                capacity = std::min(6.0f, std::max(2.0f, area->spawns / 3.0f));
        uint32 crowd = Crowd::At(position->getMapId(), position->getX(), position->getY());
        c.crowdFactor = std::max(1.0f, (crowd + 1) / capacity);
    }

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

    std::map<TravelDestination*, time_t> skip;
    {
        std::lock_guard<std::mutex> guard(goalLock);
        auto it = skipUntil.find(bot->GetGUIDLow());
        if (it != skipUntil.end())
        {
            time_t now = time(nullptr);
            for (auto i = it->second.begin(); i != it->second.end();)
                i = i->second <= now ? it->second.erase(i) : std::next(i);
            skip = it->second;
        }
    }

    // partitions are ordered by distance, so this walks nearest first
    for (auto& [partition, points] : list)
    {
        for (auto& [destination, position, distance] : points)
        {
            if (goals.size() >= MAX_CANDIDATES)
                break;
            if (!destination || !seen.insert(destination).second || skip.count(destination))
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

void ai::SilvermoonLogDecision(Player* bot, SilvermoonScoredGoal const& chosen, SilvermoonScoredGoal const* runnerUp1,
    SilvermoonScoredGoal const* runnerUp2, std::string const& reasonPrefix)
{
    auto title = [](SilvermoonScoredGoal const* g) -> std::string
    {
        if (!g || !g->destination)
            return "";
        return std::string(GoalScorer::KindName(g->kind)) + ": " + g->destination->GetTitle();
    };
    DecisionRow row;
    row.ts = uint32(time(nullptr));
    row.guid = bot->GetGUIDLow();
    row.goal = GoalScorer::KindName(chosen.kind);
    row.target = chosen.destination ? chosen.destination->GetTitle() : "";
    row.score = chosen.score.score;
    row.runnerUp1 = title(runnerUp1);
    row.runnerUp1Score = runnerUp1 ? runnerUp1->score.score : 0.0f;
    row.runnerUp2 = title(runnerUp2);
    row.runnerUp2Score = runnerUp2 ? runnerUp2->score.score : 0.0f;
    row.reason = reasonPrefix + chosen.score.reason;
    row.map = bot->GetMapId();
    row.x = bot->GetPositionX();
    row.y = bot->GetPositionY();
    DecisionLog::Add(std::move(row));
}

bool ChooseGoalAction::Execute(Event& event)
{
    silvermoon::BrainTimer brainTimer(silvermoon::BrainMicros());
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

    // the previous goal was given up within a few minutes: leave it out for a while
    {
        std::lock_guard<std::mutex> guard(goalLock);
        time_t now = time(nullptr);
        auto last = lastGoal.find(bot->GetGUIDLow());
        if (last != lastGoal.end() && last->second.first && now - last->second.second < ABANDONED_WITHIN)
            skipUntil[bot->GetGUIDLow()][last->second.first] = now + SKIP_FOR;
    }

    std::vector<SilvermoonScoredGoal> goals = SilvermoonScoreGoals(bot, ai, list);

    // Phase 2b: a quest hub elsewhere, when local options are thin (or now and then an unvisited one)
    uint16 relocateHub = 0;
    {
        SilvermoonScoredGoal relocate;
        float bestLocal = goals.empty() ? 0.0f : goals.front().score.score;
        if (SilvermoonBestRelocate(bot, ai, bestLocal, relocate, relocateHub))
            goals.insert(goals.begin(), relocate);
    }

    if (goals.empty() || goals.front().score.score <= 0.0f)
    {
        // nothing worth doing nearby: try again in 30 seconds (the stock reset/grind still run),
        // and let the level-zone check move the bot now if it has outgrown the area (a level 18
        // kept picking grey Elwynn mobs until that check came round 45-90 minutes later)
        SET_AI_VALUE2(int, "manual int", "silvermoon goal retry at", int(time(nullptr) + 30));
        sRandomPlayerbotMgr.SilvermoonCheckLevelZoneNow(bot->GetGUIDLow());
        return false;
    }

    SilvermoonScoredGoal const& best = goals.front();
    {
        std::lock_guard<std::mutex> guard(goalLock);
        lastGoal[bot->GetGUIDLow()] = { best.destination, time(nullptr) };
    }
    TravelTarget newTarget = TravelTarget(ai);
    newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
    newTarget.SetTarget(best.destination, best.position);
    Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
    // The bot keeps one travel target object for its whole life, and SetTarget/CopyTarget never
    // clear its conditions: a finished quest objective's "need quest objective" condition stayed
    // on, so every later goal went straight to cooldown and the bot chose again within seconds.
    travelTarget->SetConditions({});
    setNewTarget(requester, &newTarget, travelTarget);
    if (best.kind == GoalKind::Relocate)
        SilvermoonRelocateStarted(bot->GetGUIDLow(), relocateHub, best.destination);

    SilvermoonLogDecision(bot, best, goals.size() > 1 ? &goals[1] : nullptr, goals.size() > 2 ? &goals[2] : nullptr);

    // remembered for the 5-minute re-check
    SET_AI_VALUE2(int, "manual int", "silvermoon goal score", int(best.score.score * 1000.0f));
    SET_AI_VALUE2(int, "manual int", "silvermoon goal check at", int(time(nullptr) + 5 * MINUTE));
    return true;
}

bool ChooseGoalAction::isUseful()
{
    silvermoon::BrainTimer brainTimer(silvermoon::BrainMicros());
    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;
    if (AI_VALUE2(std::string, "manual string", "future travel purpose") != "goal")
        return false;
    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;
    return true;
}
