#include "playerbot/playerbot.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include "ChooseGoalAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "Tools/Formulas.h"
#include "playerbot/silvermoon/DecisionLog.h"
#include "playerbot/silvermoon/QuestIndex.h"
#include "playerbot/silvermoon/Crowd.h"
#include "RelocateGoal.h"
#include "Mails/Mail.h"
#include "playerbot/PlayerbotAIConfig.h"
#include <deque>
#include <set>
#include <algorithm>
#include <mutex>
#include <unordered_map>

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

    // Chores that keep coming back without effect (a broke bot's repairs, a trainer it can't reach,
    // a vendor that doesn't take what's in the bags): chosen 3 times in 15 minutes, that kind of
    // chore is left out for 30 minutes. 392 bots made 8+ repair decisions in 2 hours, 73 trainer.
    std::mutex choreLock;
    std::map<uint32, std::map<GoalKind, std::deque<time_t>>> choreChoices;   // bot -> kind -> when chosen
    std::map<uint32, std::map<GoalKind, time_t>> choreBlockedUntil;

    bool IsChore(GoalKind kind)
    {
        return kind == GoalKind::Repair || kind == GoalKind::Vendor || kind == GoalKind::Trainer || kind == GoalKind::Mail;
    }

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
        s.broke = s.urgentRepair && !context->GetValue<bool>("can repair")->Get();
        uint32 broken = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && !item->GetUInt32Value(ITEM_FIELD_DURABILITY))
                {
                    ++broken;
                    if (slot == EQUIPMENT_SLOT_MAINHAND)
                        broken += 3;
                }
        s.brokenGear = broken >= 3;
        if (Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            s.destitute = s.broke && weapon->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && !weapon->GetUInt32Value(ITEM_FIELD_DURABILITY);
        s.resSick = bot->HasAura(15007);
        s.recentDeaths = SilvermoonRecentDeaths(bot->GetGUIDLow(), 30 * MINUTE);
        s.greyLevel = int32(MaNGOS::XP::GetGrayLevel(bot->GetLevel()));
        s.repairCooldown = silvermoon::RepairOnCooldown(bot->GetGUIDLow(), bot->GetMoney());
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

namespace
{
    // follow-up priority (S): right after taking a quest, its objective - if close and level
    // appropriate - comes first, so taking Sarkoth leads to killing Sarkoth, not another errand
    const time_t FOLLOW_UP_SECONDS = 20 * MINUTE;
    const float FOLLOW_UP_RANGE = 600.0f;
    const float FOLLOW_UP_BOOST = 2.0f;
    // quests right next to the bot are picked up first (S: an orc that spawns in should take
    // "Cutting Teeth" from Gornek at once, not walk past him to a better quest farther out)
    // creature spawns per entry, counted once from the world's creature data
    uint32 SpawnCount(uint32 entry)
    {
        static std::once_flag once;
        static std::unordered_map<uint32, uint32> counts;
        std::call_once(once, []()
        {
            auto worker = [](CreatureDataPair const& dataPair)
            {
                ++counts[dataPair.second.id];
                return false;
            };
            sObjectMgr.DoCreatureData(worker);
        });
        auto it = counts.find(entry);
        return it == counts.end() ? 0 : it->second;
    }

    // quest colour (S): grey quests are ignored except class quests; class > yellow > green >
    // orange > red
    float QuestColourFactor(Player* bot, uint32 questId, std::string& tag)
    {
        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        if (!quest)
            return 1.0f;
        if (quest->GetRequiredClasses())
        {
            tag = "class quest: ";
            return 2.0f;
        }
        int32 level = bot->GetLevel();
        int32 questLevel = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : level;
        int32 diff = questLevel - level;
        if (diff >= 5)  { tag = "red quest: ";    return 0.2f; }
        // starting-area quests are often 3-4 levels above a fresh character (Sarkoth is level 5,
        // offered at level 1): under level 10 orange counts as yellow
        if (diff >= 3 && level < 10) { tag = ""; return 1.5f; }
        if (diff >= 3)  { tag = "orange quest: "; return 0.6f; }
        if (diff >= -2) { tag = "";               return 1.5f; }   // yellow
        if (questLevel > int32(MaNGOS::XP::GetGrayLevel(uint32(level)))) { tag = ""; return 1.0f; }   // green
        tag = "grey quest: ";
        return 0.0f;
    }

    // goals inside enemy ground are never chosen: the same rule as the enemy-zone watchdog
    // (enemy capitals and starter zones at any level, any enemy zone under level 20). Alliance
    // bots kept picking quest objectives in Durotar and were sent home again and again.
    bool EnemyGround(Player* bot, WorldPosition const& pos)
    {
        uint32 zoneId = 0, areaId = 0;
        sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, pos.getMapId(), pos.getX(), pos.getY(), pos.getZ());
        bool enemy = false, capital = false;
        for (uint32 id : { zoneId, areaId })
        {
            AreaTableEntry const* entry = id ? GetAreaEntryByAreaID(id) : nullptr;
            if (!entry)
                continue;
            if ((entry->team == AREATEAM_ALLY && bot->GetTeam() == HORDE) || (entry->team == AREATEAM_HORDE && bot->GetTeam() == ALLIANCE))
                enemy = true;
            if (entry->flags & AREA_FLAG_CAPITAL)
                capital = true;
        }
        if (!enemy)
            return false;
        static const std::set<uint32> earlyZones = { 12, 1, 141, 40, 38, 148, 14, 215, 85, 130 };
        return bot->GetLevel() < 20 || capital || earlyZones.count(zoneId);
    }

    const float NEARBY_GIVER_RANGE = 80.0f;
    const float NEARBY_GIVER_BOOST = 6.0f;
    std::map<uint32, std::map<uint32, time_t>> newQuests;   // bot -> quest -> taken at (guarded by goalLock)

    bool IsFollowUp(uint32 guid, uint32 questId)
    {
        std::lock_guard<std::mutex> guard(goalLock);
        auto it = newQuests.find(guid);
        if (it == newQuests.end())
            return false;
        auto q = it->second.find(questId);
        return q != it->second.end() && time(nullptr) - q->second < FOLLOW_UP_SECONDS;
    }
}

void ai::SilvermoonNoteNewQuest(uint32 guid, uint32 questId)
{
    std::lock_guard<std::mutex> guard(goalLock);
    auto& quests = newQuests[guid];
    time_t now = time(nullptr);
    for (auto i = quests.begin(); i != quests.end();)
        i = now - i->second >= FOLLOW_UP_SECONDS ? quests.erase(i) : std::next(i);
    quests[questId] = now;
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
    else if (purpose == TravelDestinationPurpose::GatherMining || purpose == TravelDestinationPurpose::GatherHerbalism)
        c.kind = GoalKind::Gather;

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
    // doable quests close by come before grinding (S: Mommy ground Kobold Workers with quest
    // objectives 200-250 yards away): quest objectives within 250 yards that aren't elite or 3+
    // levels up are worth half again
    if (c.kind == GoalKind::QuestObjective && distance < 250.0f && !c.elite && (c.mobLevel <= 0 || c.mobLevel < int32(bot->GetLevel()) + 3))
    {
        goal.score.score *= 1.5f;
        goal.score.reason = "nearby quest: " + goal.score.reason;
    }
    // a grind spot on mobs an active quest needs is quest work (S: Mommy "grind fallback"-ed Kobold
    // Vermin for Kobold Camp Cleanup - grind 38 against the objective's 36): worth a third more and
    // labelled as such
    if (c.kind == GoalKind::Grind && destination->GetEntry() > 0 && bot->GetPlayerbotAI() &&
        bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<bool>("need for quest", std::to_string(destination->GetEntry()))->Get())
    {
        goal.score.score *= 1.33f;
        goal.score.reason = "quest mob";
    }
    // gathering stays near the bot's level: no nodes in an area more than 8 levels below it
    // (broke level 16-24 bots went back to Elwynn for Peacebloom, Silverleaf and copper)
    if (c.kind == GoalKind::Gather && position)
    {
        uint32 zoneId = 0, areaId = 0;
        sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, position->getMapId(), position->getX(), position->getY(), position->getZ());
        for (uint32 id : { zoneId, areaId })
            if (AreaTableEntry const* entry = id ? GetAreaEntryByAreaID(id) : nullptr)
                if (entry->area_level > 0)
                {
                    if (int32(entry->area_level) + 8 < int32(bot->GetLevel()))
                        goal.score.score = 0.0f;
                    break;
                }
    }
    // a bot under level 6 doesn't leave its starting area for a trainer: the only trainer with its
    // next rank was out in Razor Hill and Grashnak (level 1-2) died to the scorpids on the way.
    // The spell waits until the bot outgrows the valley anyway.
    if (c.kind == GoalKind::Trainer && bot->GetLevel() < 6 && distance > 300.0f)
        goal.score.score = 0.0f;
    if (c.kind == GoalKind::QuestTurnIn)
    {
        uint32 finished = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (uint32 id = bot->GetQuestSlotQuestId(slot))
                if (bot->GetQuestStatus(id) == QUEST_STATUS_COMPLETE)
                    ++finished;
        // finished quests waiting to be handed in are money and XP sitting idle (the tracked bots
        // carried about 12 each)
        if (finished > 1)
        {
            goal.score.score *= std::min(3.0f, 1.0f + 0.25f * finished);
            goal.score.reason = "hand-in (" + std::to_string(finished) + " done): " + goal.score.reason;
        }
    }
    // a quest objective on a creature with only a few spawns waits behind one with many (Kron's
    // Amulet drops from 49 Dreadmaw Crocolisks and 2 Corrupted ones; the bot kept walking to the
    // 2, found them dead or camped and gave up after 3 minutes)
    if (c.kind == GoalKind::QuestObjective && destination->GetEntry() > 0)
    {
        uint32 spawns = SpawnCount(uint32(destination->GetEntry()));
        if (spawns < 6)
        {
            goal.score.score *= 0.4f + 0.1f * float(spawns);
            goal.score.reason = "few spawns: " + goal.score.reason;
        }
    }
    uint32 colourQuest = 0;
    if (c.kind == GoalKind::QuestGiver)
        if (QuestRelationTravelDestination* q = dynamic_cast<QuestRelationTravelDestination*>(destination))
            colourQuest = q->GetQuestId();
    if (c.kind == GoalKind::QuestObjective)
        if (QuestObjectiveTravelDestination* o = dynamic_cast<QuestObjectiveTravelDestination*>(destination))
            colourQuest = o->GetQuestId();
    if (colourQuest)
    {
        std::string tag;
        goal.score.score *= QuestColourFactor(bot, colourQuest, tag);
        if (!tag.empty())
            goal.score.reason = tag + goal.score.reason;
    }
    if (c.kind == GoalKind::QuestGiver && distance < NEARBY_GIVER_RANGE && !c.avoid)
    {
        goal.score.score *= NEARBY_GIVER_BOOST;
        goal.score.reason = "next door: " + goal.score.reason;
    }
    if (c.kind == GoalKind::QuestObjective && distance < FOLLOW_UP_RANGE && !c.elite && !c.avoid &&
        (c.mobLevel <= 0 || c.mobLevel <= int32(bot->GetLevel()) + 2))
        if (QuestObjectiveTravelDestination* o = dynamic_cast<QuestObjectiveTravelDestination*>(destination))
            if (IsFollowUp(bot->GetGUIDLow(), o->GetQuestId()))
            {
                goal.score.score *= FOLLOW_UP_BOOST;
                goal.score.reason = "follow-up: " + goal.score.reason;
            }
    // a just-taken quest that is turned in nearby (talk-to quests like "Your Place In The World":
    // take it from Kaltunk, hand it to Gornek) is a follow-up too
    if (c.kind == GoalKind::QuestTurnIn && distance < FOLLOW_UP_RANGE)
        if (QuestRelationTravelDestination* q = dynamic_cast<QuestRelationTravelDestination*>(destination))
            if (IsFollowUp(bot->GetGUIDLow(), q->GetQuestId()))
            {
                goal.score.score *= FOLLOW_UP_BOOST;
                goal.score.reason = "follow-up: " + goal.score.reason;
            }
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

    uint32 avoidedZone = SilvermoonAvoidedZone(bot->GetGUIDLow());
    // starting area first (S: a level 4 in Northshire walked to Goldshire to hand in Rest and
    // Relaxation with Kobold Camp Cleanup and Garrick Padfoot still open): while the bot stands in
    // its starting valley and has quests for it in the log, goals outside the valley count 15%
    // (the valley is "within 400 yards of the race's starting spot": the valleys' sub-areas have
    // their own area ids, so area ids alone miss half of Northshire)
    const float STARTER_RADIUS = 400.0f;
    bool starterPull = false;
    WorldPosition starterSpot;
    {
        static const std::set<int32> starterAreas = { 9, 132, 188, 363, 154, 220 };   // Northshire, Coldridge, Shadowglen, Valley of Trials, Deathknell, Camp Narache
        if (PlayerInfo const* start = sObjectMgr.GetPlayerInfo(bot->getRace(), bot->getClass()))
        {
            starterSpot = WorldPosition(start->mapId, start->positionX, start->positionY, start->positionZ);
            if (starterSpot.getMapId() == bot->GetMapId() && starterSpot.fDist(WorldPosition(bot)) < STARTER_RADIUS)
                for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE && !starterPull; ++slot)
                    if (Quest const* q = sObjectMgr.GetQuestTemplate(bot->GetQuestSlotQuestId(slot)))
                        if (starterAreas.count(q->GetZoneOrSort()) && bot->GetQuestStatus(q->GetQuestId()) == QUEST_STATUS_INCOMPLETE)
                            starterPull = true;
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
            if (position && EnemyGround(bot, *position))
                continue;
            // another map (continent): only relocation crosses an ocean - a quest objective on the other
            // continent scored like a local one and the bot wandered, killing grey mobs (Guile, in
            // Tirisfal, "doing" Kron's Amulet in Durotar)
            if (position && position->getMapId() != bot->GetMapId())
                continue;
            // world PvP option A: no goals in a zone where the bot died to players twice lately
            if (position && avoidedZone)
            {
                uint32 zoneId = 0, areaId = 0;
                sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, position->getMapId(), position->getX(), position->getY(), position->getZ());
                if (zoneId == avoidedZone)
                    continue;
            }
            // death-loop guard: no goal within 250 yards of a spot where the bot died twice in
            // the last hour (10 bots died 61-95 times in one night, going back to the same place)
            if (position && SilvermoonDeathsNearPoint(bot->GetGUIDLow(), position->getMapId(), position->getX(), position->getY(), 250.0f, HOUR) >= 2)
                continue;
            goals.push_back(ScoreWith(bot, snapshot, destination, position, distance));
            if (starterPull && position)
            {
                if (position->getMapId() != starterSpot.getMapId() || starterSpot.fDist(*position) >= STARTER_RADIUS)
                {
                    goals.back().score.score *= 0.15f;
                    goals.back().score.reason = "outside starting area: " + goals.back().score.reason;
                }
            }
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

    // drop chore kinds that are on their 30-minute break
    {
        std::lock_guard<std::mutex> guard(choreLock);
        auto it = choreBlockedUntil.find(bot->GetGUIDLow());
        if (it != choreBlockedUntil.end())
        {
            time_t now = time(nullptr);
            goals.erase(std::remove_if(goals.begin(), goals.end(), [&](SilvermoonScoredGoal const& g)
            {
                auto b = it->second.find(g.kind);
                return b != it->second.end() && now < b->second;
            }), goals.end());
        }
    }

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
    if (IsChore(best.kind))
    {
        std::lock_guard<std::mutex> guard(choreLock);
        time_t now = time(nullptr);
        auto& times = choreChoices[bot->GetGUIDLow()][best.kind];
        times.push_back(now);
        while (!times.empty() && now - times.front() > 15 * MINUTE)
            times.pop_front();
        if (times.size() >= 3)
        {
            choreBlockedUntil[bot->GetGUIDLow()][best.kind] = now + 30 * MINUTE;
            times.clear();
        }
    }
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
    // ...and setNewTarget re-adds the stock "future travel condition" left by the bot's last stock
    // travel request (e.g. "should travel named::reagent vendor"): false for a brain goal, so a
    // relocation trip went straight to cooldown and was re-picked every ~5 seconds. Cleared.
    SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
    setNewTarget(requester, &newTarget, travelTarget);
    if (best.kind == GoalKind::Relocate)
    {
        SilvermoonRelocateStarted(bot->GetGUIDLow(), relocateHub, best.destination, best.score.minutes);
        // the stock trip timer is twice the straight-line walk; a trip by road, boat or flight can
        // take longer, so give it twice the route estimate plus 5 minutes (the abandon rule's limit)
        travelTarget->SetExpireIn(uint32((2.0f * best.score.minutes + 5.0f) * MINUTE * IN_MILLISECONDS));
        travelTarget->SetForced(true);   // forced targets don't drop into cooldown on the way
    }

    SilvermoonLogDecision(bot, best, goals.size() > 1 ? &goals[1] : nullptr, goals.size() > 2 ? &goals[2] : nullptr);

    // remembered for the 5-minute re-check
    SET_AI_VALUE2(int, "manual int", "silvermoon goal score", int(best.score.score * 1000.0f));
    SET_AI_VALUE2(int, "manual int", "silvermoon goal kind", int(best.kind) + 1);
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
