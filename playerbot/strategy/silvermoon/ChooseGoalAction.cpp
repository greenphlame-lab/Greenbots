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
#include "Professions.h"
#include "Policies.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "Mails/Mail.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/silvermoon/Hubs.h"
#include "playerbot/silvermoon/Recovery.h"
#include "Spells/SpellAuras.h"
#include <deque>
#include <set>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include "ActionQueue.h"

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
    // Objective demand (S, Nov 24: "something bigger than competition" in the Barrens): Egg Hunt was
    // 14,707 of 33,449 Barrens objective choices in a day - 276 bots held it for 20 Silithid
    // Mounds that respawn every 5 minutes, about 480 eggs an hour for 3,300 needed; 224 of them had
    // no egg at all. Bots per quest objective (chosen in the last 15 minutes), rebuilt every 30s.
    std::mutex demandLock;
    std::unordered_map<uint32, std::pair<uint32, time_t>> objectiveChoice;   // bot -> quest*8+objective, when
    std::unordered_map<uint32, uint32> demandCounts;
    time_t demandBuilt = 0;
    uint32 ObjectiveDemand(uint32 key)
    {
        std::lock_guard<std::mutex> guard(demandLock);
        time_t now = time(nullptr);
        if (now - demandBuilt >= 30)
        {
            demandBuilt = now;
            demandCounts.clear();
            for (auto it = objectiveChoice.begin(); it != objectiveChoice.end();)
            {
                if (now - it->second.second > 15 * MINUTE)
                    it = objectiveChoice.erase(it);
                else
                {
                    ++demandCounts[it->second.first];
                    ++it;
                }
            }
        }
        auto it = demandCounts.find(key);
        return it == demandCounts.end() ? 0 : it->second;
    }
    void NoteObjectiveChoice(uint32 guid, uint32 key)
    {
        std::lock_guard<std::mutex> guard(demandLock);
        if (key)
            objectiveChoice[guid] = { key, time(nullptr) };
        else
            objectiveChoice.erase(guid);
    }

    // trainer trips (S, Nov 25: Grashnak walked to Kaplak in Razor Hill four times in an hour): a bot
    // that chose a trainer trip at this level waits an hour before choosing another, and after two
    // at the same level waits for its next level
    std::mutex trainerLock;
    struct TrainerTrips { uint32 level = 0; uint32 count = 0; time_t last = 0; };
    std::unordered_map<uint32, TrainerTrips> trainerTrips;
    bool TrainerTripAllowed(Player* bot)
    {
        std::lock_guard<std::mutex> guard(trainerLock);
        auto it = trainerTrips.find(bot->GetGUIDLow());
        if (it == trainerTrips.end() || it->second.level != bot->GetLevel())
            return true;
        return it->second.count < 2 && time(nullptr) - it->second.last >= HOUR;
    }
    void NoteTrainerTrip(Player* bot)
    {
        std::lock_guard<std::mutex> guard(trainerLock);
        TrainerTrips& t = trainerTrips[bot->GetGUIDLow()];
        if (t.level != bot->GetLevel())
            t = TrainerTrips{ bot->GetLevel(), 0, 0 };
        ++t.count;
        t.last = time(nullptr);
    }

    std::mutex goalLock;
    std::map<uint32, std::pair<TravelDestination*, time_t>> lastGoal;   // bot -> last chosen, when
    std::map<uint32, WorldPosition> lastGoalAt;                          // bot -> where that goal was
    std::map<uint32, bool> lastGoalGather;                               // bot -> that goal was a gathering spot
    std::map<uint32, bool> lastGoalObject;                               // bot -> that goal was a quest object (chest, keg, mound)
    std::map<uint32, std::map<TravelDestination*, time_t>> skipUntil;   // bot -> destination -> until
    // Gather orders: spawn spots a bot already stood on lately (herbs and ore respawn on their own
    // clock - an empty spot is not a success, the bot moves on to the next spot)
    std::map<uint32, std::vector<std::pair<WorldPosition, time_t>>> gatherVisited;

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
        s.cashOut = SilvermoonCashOut(bot);
        s.sellValue = s.cashOut ? SilvermoonBagSellValue(bot) : 0;
        s.struggling = SilvermoonStruggling(bot->GetGUIDLow());
        s.canGather = bot->GetSkillValue(SKILL_MINING) > 0 || bot->GetSkillValue(SKILL_HERBALISM) > 0;
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
        // not for this class or race (S: Guile, a rogue, walked to Deathguard Dillinger again and
        // again for the warrior quest "Ulag the Cleaver"), or dropped before: worth nothing
        if ((quest->GetRequiredClasses() && !(quest->GetRequiredClasses() & bot->getClassMask())) ||
            (quest->GetRequiredRaces() && !(quest->GetRequiredRaces() & bot->getRaceMask())) ||
            SilvermoonQuestDropped(bot->GetGUIDLow(), questId))
        {
            tag = "not for me: ";
            return 0.0f;
        }
        if (quest->GetRequiredClasses())
        {
            // 1.8.2 (S: Nynitiz, a level 22 warrior, kept going back to Coldridge Valley): a class quest
            // far below the bot (the level 1 trainer letters) isn't worth a trip
            int32 ql = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : int32(bot->GetLevel());
            if (int32(bot->GetLevel()) - ql > 10)
            {
                tag = "class quest, far below me: ";
                return 0.0f;
            }
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
    else if (purpose == TravelDestinationPurpose::Vendor || purpose == TravelDestinationPurpose::AH)
        c.kind = GoalKind::Vendor;
    else if (purpose == TravelDestinationPurpose::Repair)
        c.kind = GoalKind::Repair;
    else if (purpose == TravelDestinationPurpose::Trainer)
        c.kind = GoalKind::Trainer;
    else if (purpose == TravelDestinationPurpose::Mail)
        c.kind = GoalKind::Mail;
    else if (purpose == TravelDestinationPurpose::GatherMining || purpose == TravelDestinationPurpose::GatherHerbalism ||
             (purpose == TravelDestinationPurpose::GatherFishing && SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 356) > 1.0f))
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
    // Professions tab: a "gather" order makes that kind of gathering the top priority (skinning: the
    // skinnable beasts' grind spots)
    if (c.kind == GoalKind::Gather)
    {
        uint32 skill = purpose == TravelDestinationPurpose::GatherMining ? 186 : purpose == TravelDestinationPurpose::GatherHerbalism ? 182 :
            purpose == TravelDestinationPurpose::GatherFishing ? 356 : 0;
        float f = skill ? SilvermoonGatherOrderFactor(bot->GetGUIDLow(), skill) : 1.0f;
        if (f > 1.0f)
        {
            // the order comes first (S): ahead of quests, like a learn order; the nearest
            // spot it can gather wins among the gathering spots
            goal.score.score = 500000.0f / std::max(1.0f, goal.score.minutes);
            goal.score.reason = "gather order";
        }
    }
    if (c.kind == GoalKind::Grind && destination->GetEntry() > 0 && SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 393) > 1.0f)
        if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(destination->GetEntry()))
            if (info->SkinningLootId)
            {
                goal.score.score *= 5.0f;
                goal.score.reason = "gather order: skinning";
            }
    // auction house trip: worth 1 minute of XP per stack marked for the AH, up to 25 (a vendor chore
    // is worth 3-20); the label says where it's going
    if (purpose == TravelDestinationPurpose::AH && bot->GetPlayerbotAI())
    {
        std::list<Item*> ahItems = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<Item*>>("inventory items", "usage " + std::to_string((uint8)ItemUsage::ITEM_USAGE_AH))->Get();
        uint32 stacks = (uint32)ahItems.size();
        float minutes = float(std::min<uint32>(25, stacks));
        // your characters: due at your "AH trip at" gold, or with any marked item when it isn't set
        // (Guile, no setting, carried 45 herbs marked AH and never went)
        bool policyTrip = SilvermoonIsOwn(bot) && SilvermoonAhTripDue(bot, ahItems);
        if (policyTrip)
            minutes = 25.0f;      // your "AH trip when worth" is reached: worth the trip
        goal.score.value = GoalScorer::XpPerMinute(snapshot) * minutes;
        goal.score.score = goal.score.minutes > 0.0f ? goal.score.value / goal.score.minutes : 0.0f;
        goal.score.reason = "auction house";
        // only when already in or near a town with an auction house (600 yards) - S, Nov 25:
        // Grashnak picked three Orgrimmar auctioneers in a row from the Crossroads. Your "AH trip at
        // X gold" policy still sends your character from anywhere.
        // 1.7.7 (S: "there's like no auctions right now"): that rule cut AH trips from ~350 to ~30
        // every 4 hours, and new listings dried up. A trip from further out (up to 3,000 yards) is
        // back when the bags hold 8 or more stacks for the AH; the trip's length still weighs
        // against it in the score
        if (!policyTrip && distance > 600.0f && (stacks < 8 || distance > 3000.0f))
        {
            goal.score.score = 0.0f;
            goal.score.reason = "auction house: too far";
        }
    }
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
    if (c.kind == GoalKind::Gather && position && !(purpose == TravelDestinationPurpose::GatherMining && SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 186) > 1.0f) &&
        !(purpose == TravelDestinationPurpose::GatherHerbalism && SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 182) > 1.0f))
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
    if (c.kind == GoalKind::Trainer && !TrainerTripAllowed(bot))
    {
        goal.score.score = 0.0f;
        goal.score.reason = "trainer: visited at this level";
    }
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
    // a quest objective on a herb or ore node needs the gathering skill for that node (S, Nov 25:
    // Grashnak, Herbalism 13, walked to Mageroyal (50) and Briarthorn (70) for Root Samples, could
    // not pick them, and died twice out there)
    if (c.kind == GoalKind::QuestObjective && destination->GetEntry() < 0)
        if (GameObjectInfo const* go = ObjectMgr::GetGameObjectInfo(uint32(-destination->GetEntry())))
            if (LockEntry const* lock = sLockStore.LookupEntry(go->GetLockId()))
                for (int i = 0; i < 8; ++i)
                {
                    if (lock->Type[i] != LOCK_KEY_SKILL)
                        continue;
                    uint32 skill = SkillByLockType(LockType(lock->Index[i]));
                    if (skill != SKILL_HERBALISM && skill != SKILL_MINING)
                        skill = 0;
                    if (skill && bot->GetSkillValue(skill) < lock->Skill[i])
                    {
                        goal.score.score = 0.0f;
                        goal.score.reason = "needs more gathering skill";
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
    // oversubscribed objective: more than one bot per two spawns working it scores down in
    // proportion (to a tenth), so the bot does something else or goes looking for a quieter hub
    if (c.kind == GoalKind::QuestObjective && position)
        if (QuestObjectiveTravelDestination* o = dynamic_cast<QuestObjectiveTravelDestination*>(destination))
        {
            uint32 key = o->GetQuestId() * 8 + o->GetObjective();
            uint32 supply = destination->GetEntry() > 0 ? SpawnCount(uint32(destination->GetEntry())) : 0;
            if (!supply)
                if (QuestArea const* area = QuestIndex::NearestArea(o->GetQuestId(), uint8(o->GetObjective() + 1), position->getMapId(), position->getX(), position->getY()))
                    supply = area->spawns;
            float room = std::max(2.0f, float(supply) / 2.0f);
            float demand = float(ObjectiveDemand(key));
            if (demand > room)
            {
                goal.score.score *= std::max(0.1f, room / demand);
                goal.score.reason = "oversubscribed (" + std::to_string(uint32(demand)) + " bots): " + goal.score.reason;
            }
        }
    uint32 colourQuest = 0;
    if (c.kind == GoalKind::QuestGiver)
        if (QuestRelationTravelDestination* q = dynamic_cast<QuestRelationTravelDestination*>(destination))
        {
            colourQuest = q->GetQuestId();
            // 1.8.2 (S: Nynitiz walked to Sten Stoutarm every 5 minutes for "Simple Memorandum", done long
            // ago): the giver counts as "has a quest" when it has any quest for the bot (here a grey
            // one it never takes), but the goal is scored for this quest - which must be takeable
            Quest const* qt = sObjectMgr.GetQuestTemplate(colourQuest);
            if (qt && (bot->GetQuestRewardStatus(colourQuest) || bot->GetQuestStatus(colourQuest) != QUEST_STATUS_NONE || !bot->CanTakeQuest(qt, false)))
            {
                goal.score.score = 0.0f;
                goal.score.reason = "quest not takeable: " + goal.score.reason;
                colourQuest = 0;
            }
        }
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
    // quests first (S, Nov 25: "bump up quest priority across the board"): plain grinding, exploring
    // and gathering without an order count two thirds, so quest pick-ups, objectives and hand-ins win
    // more often. Grinding a quest's own mobs ("quest mob") and gather orders keep their value. Done
    // by lowering the others, not raising quests, so a zone with only grinding left still looks thin
    // enough to relocate from.
    if ((c.kind == GoalKind::Grind && goal.score.reason != "quest mob") || c.kind == GoalKind::Explore ||
        (c.kind == GoalKind::Gather && goal.score.reason.rfind("gather order", 0) != 0))
        goal.score.score *= 0.67f;
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
    bool policyZones = SilvermoonIsOwn(bot) && !SilvermoonPolicyFor(bot->GetGUIDLow()).allowedZones.empty();
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
    // Gather order (S: Mommy left her herb order for a quest hand-in): the candidate list is cut at
    // 40 nearest-first, and quests nearby filled it before any herb spot made the list. With an
    // order on, gathering spots are always considered (the cap still applies to everything else).
    bool gatherOrder = SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 186) > 1.0f || SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 182) > 1.0f ||
        SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 356) > 1.0f;
    // Gather order as a patrol (S): every spawn spot is its own stop (not one stop per herb type),
    // the spot the bot is standing on now counts as visited, and spots visited in the last 15 minutes
    // are passed over - so it walks spot to spot, and the gathering step loots any node it sees on
    // the way (60 yards, see AddLootAction)
    std::vector<WorldPosition> recentSpots;
    if (gatherOrder)
    {
        std::lock_guard<std::mutex> guard(goalLock);
        auto& visited = gatherVisited[bot->GetGUIDLow()];
        time_t now = time(nullptr);
        visited.erase(std::remove_if(visited.begin(), visited.end(), [now](std::pair<WorldPosition, time_t> const& v) { return now - v.second > 15 * MINUTE; }), visited.end());
        visited.push_back({ WorldPosition(bot), now });
        for (auto const& v : visited)
            recentSpots.push_back(v.first);
    }
    uint32 gatherStops = 0;
    // 1.8.4 (S: Chino on Auto, "Road to Salvation" queued, did nothing - its turn-in is in Ironforge, a
    // zone his Policies leave out, and far enough to fall past the 40 nearest candidates): the queued
    // quest's own giver, objectives and turn-in pass the zone list and the cap - you asked for them
    uint32 queueQuest = SilvermoonQueueFetchQuest(bot);
    auto forQueue = [&](TravelDestination* d) -> bool
    {
        if (!queueQuest || !d)
            return false;
        QuestTravelDestination* qd = dynamic_cast<QuestTravelDestination*>(d);
        return qd && qd->GetQuestId() == queueQuest;
    };
    // partitions are ordered by distance, so this walks nearest first
    for (auto& [partition, points] : list)
    {
        for (auto& [destination, position, distance] : points)
        {
            if (goals.size() >= MAX_CANDIDATES && !gatherOrder && !queueQuest)
                break;
            if (goals.size() >= MAX_CANDIDATES && queueQuest && !gatherOrder && !forQueue(destination))
                continue;
            if (goals.size() >= MAX_CANDIDATES && (!destination ||
                (destination->GetPurpose() != TravelDestinationPurpose::GatherHerbalism && destination->GetPurpose() != TravelDestinationPurpose::GatherMining &&
                 destination->GetPurpose() != TravelDestinationPurpose::GatherFishing)))
                continue;
            bool gatherSpot = gatherOrder && destination && position &&
                (destination->GetPurpose() == TravelDestinationPurpose::GatherHerbalism || destination->GetPurpose() == TravelDestinationPurpose::GatherMining ||
                 destination->GetPurpose() == TravelDestinationPurpose::GatherFishing);
            if (gatherSpot)
            {
                bool recent = false;
                for (WorldPosition const& v : recentSpots)
                    if (v.getMapId() == position->getMapId() && v.fDist(*position) < 40.0f)
                        recent = true;
                if (recent || gatherStops >= 12)
                    continue;
            }
            else if (!destination || !seen.insert(destination).second || skip.count(destination))
                continue;
            // a gathering spot under a gather order only has to be possible (skill): "active" means a
            // node of that kind in sight whenever the bot stands inside the herb's area, so in the
            // middle of Elwynn every Peacebloom and Silverleaf spot went inactive at once and Mommy
            // idled "nowhere to travel" (S, 1.6.9). The patrol's visited list does the rest.
            if (gatherSpot ? !destination->IsPossible(info) : !destination->IsActive(bot, info))
                continue;
            if (position && EnemyGround(bot, *position))
                continue;
            // another map (continent): only relocation crosses an ocean - a quest objective on the other
            // continent scored like a local one and the bot wandered, killing grey mobs (Guile, in
            // Tirisfal, "doing" Kron's Amulet in Durotar)
            if (position && position->getMapId() != bot->GetMapId())
                continue;
            // policies (S): zones ticked off in the Policies window are never goals
            if (position && policyZones && !forQueue(destination))
            {
                uint32 zoneId = 0, areaId = 0;
                sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, position->getMapId(), position->getX(), position->getY(), position->getZ());
                if (SilvermoonZoneBlocked(bot->GetGUIDLow(), zoneId))
                    continue;
            }
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
            if (gatherSpot)
                ++gatherStops;   // counted once it passed every check (S: Mommy idled "nowhere to travel"
                                 // after a Peacebloom - 12 stops were used up by spots then thrown out)
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
        if (goals.size() >= MAX_CANDIDATES && !gatherOrder)
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

namespace
{
    std::mutex restLock;
    std::unordered_map<uint32, TravelDestination*> restTrip;   // bot -> its inn trip (compared only)
    std::unordered_map<uint32, time_t> restLogged;             // bot -> last "waiting it out" log row
}

bool ai::SilvermoonResSickRestApplies(Player* bot)
{
    return bot && bot->IsAlive() && bot->HasAura(15007) && !bot->InBattleGround() && !bot->GetMap()->IsDungeon() &&
        !bot->IsTaxiFlying() && !bot->GetTransport() && (!bot->GetGroup() || bot->GetGroup()->GetMembersCount() < 2);
}

bool ai::SilvermoonIsResSickTrip(uint32 guid, TravelDestination* destination)
{
    std::lock_guard<std::mutex> guard(restLock);
    auto it = restTrip.find(guid);
    return it != restTrip.end() && it->second == destination;
}

bool ChooseGoalAction::Execute(Event& event)
{
    silvermoon::BrainTimer brainTimer(silvermoon::BrainMicros());
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    if (travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    // profession rank due (S, 1.6.22: "the #1 priority is to train the next level, as long as level and
    // money permit"): a trip to the nearest trainer who teaches it, through the Go to step below (a
    // trainer within 20 yards trains it on the spot). Checked at most every 2 minutes; out of combat,
    // not with resurrection sickness.
    if (!bot->IsInCombat() && !bot->HasAura(15007))
    {
        static std::mutex rankLock;
        static std::unordered_map<uint32, time_t> lastRank;
        bool due = false;
        {
            std::lock_guard<std::mutex> guard(rankLock);
            time_t& at = lastRank[bot->GetGUIDLow()];
            if (time(nullptr) - at >= 2 * MINUTE)
            {
                at = time(nullptr);
                due = true;
            }
        }
        if (due && !SilvermoonLearnRankHere(bot))
            SilvermoonGoTrainRank(bot);
    }

    // Go to order (S, 1.6.21: "should immediately override whatever task the bot is on"): ahead of
    // everything - gather routes, resting, shopping - and without waiting for the destination search
    // 1.7.6 (S: bots re-planned a training trip every 2-3 seconds): with Resurrection Sickness the
    // brain's own trips wait - the res-sick rest ended them every tick and they were chosen again
    if (SilvermoonGoIsYours(bot) || !SilvermoonResSickRestApplies(bot))
    {
        SilvermoonScoredGoal go;
        bool hadOrder = SilvermoonGoIsYours(bot);
        bool gotGo = SilvermoonGoGoal(bot, go);
        // 1.8.2 diagnostic: your order there, but no trip came of it
        if (hadOrder && (!gotGo || !go.destination || !go.position))
            sLog.outBasic("Silvermoon: %s Go to order gave no trip (%s)", bot->GetName(),
                !gotGo ? (SilvermoonGoIsYours(bot) ? "no spot found" : "order finished") : "no destination");
        if (gotGo && go.destination && go.position)
        {
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            TravelTarget newTarget = TravelTarget(ai);
            newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
            newTarget.SetTarget(go.destination, go.position);
            Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
            travelTarget->SetConditions({});
            SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
            setNewTarget(requester, &newTarget, travelTarget);
            travelTarget->SetExpireIn(uint32(30 * MINUTE * IN_MILLISECONDS));
            travelTarget->SetForced(true);
            SilvermoonLogDecision(bot, go, nullptr, nullptr);
            SET_AI_VALUE2(int, "manual int", "silvermoon goal kind", int(go.kind) + 1);
            SET_AI_VALUE2(int, "manual int", "silvermoon goal check at", int(time(nullptr) + 5 * MINUTE));
            return true;
        }
    }

    // action queue (S, 1.8): Go and Wait steps (and "queue only" with nothing queued) are handled
    // here; quest, grind and train steps filter the scored goals below
    {
        uint8_t qKind = 0;
        uint32_t qId = 0;
        if (SilvermoonQueueStep(bot, qKind, qId) == 1)
        {
            SET_AI_VALUE2(int, "manual int", "silvermoon goal retry at", int(time(nullptr) + 10));
            return false;
        }
    }

    // "Buy bigger bags" (S, 1.6.21): a bigger bag it can afford and no vendor selling it nearby - a
    // trip to the nearest one (at most once an hour); the Go to step below takes it from there
    if (!bot->IsInCombat() && !SilvermoonCashOut(bot) && !bot->HasAura(15007))
    {
        static std::mutex bagLock;
        static std::unordered_map<uint32, time_t> lastBagTrip;
        bool due = false;
        {
            std::lock_guard<std::mutex> guard(bagLock);
            time_t& last = lastBagTrip[bot->GetGUIDLow()];
            if (time(nullptr) - last >= HOUR)
            {
                last = time(nullptr);
                due = true;
            }
        }
        if (due && !SilvermoonBuyBag(bot))
            if (uint32 bag = SilvermoonBagWanted(bot))
                if (SilvermoonGoBagVendor(bot, bag))
                {
                    SilvermoonScoredGoal go;
                    if (SilvermoonGoGoal(bot, go) && go.destination && go.position)
                    {
                        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                        TravelTarget newTarget = TravelTarget(ai);
                        newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
                        newTarget.SetTarget(go.destination, go.position);
                        Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
                        travelTarget->SetConditions({});
                        SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
                        setNewTarget(requester, &newTarget, travelTarget);
                        travelTarget->SetExpireIn(uint32(20 * MINUTE * IN_MILLISECONDS));
                        travelTarget->SetForced(true);
                        go.score.reason = "bags: buy a bigger bag at " + go.score.reason.substr(go.score.reason.find("go to ") == std::string::npos ? 0 : go.score.reason.find("go to ") + 6);
                        SilvermoonLogDecision(bot, go, nullptr, nullptr);
                        SET_AI_VALUE2(int, "manual int", "silvermoon goal kind", int(go.kind) + 1);
                        return true;
                    }
                }
    }

    // Resurrection Sickness (S, 1.6.20): no fighting, questing or gathering until it wears off. The
    // bot walks to the nearest friendly inn when it gets there before the sickness ends (within 1,200
    // yards, running about 7 yards a second) and waits there; otherwise it sits down where it is. It
    // asks again every 30 seconds, so it goes back to its normal goals as soon as the sickness is gone.
    // (a gather session takes the place of resting while the bot has a gathering profession - S, 1.6.22)
    if (SilvermoonResSickRestApplies(bot) && !bot->IsInCombat() && !SilvermoonCatchUpSkill(bot->GetGUIDLow()))
    {
        uint32 guid = bot->GetGUIDLow();
        SpellAuraHolder* holder = bot->GetSpellAuraHolder(15007);
        float leftSeconds = holder ? holder->GetAuraDuration() / 1000.0f : 0.0f;
        bool alliance = bot->GetTeam() == ALLIANCE;
        Hub const* inn = nullptr;
        float innDist = 0.0f;
        for (Hub const& h : Hubs::All())
        {
            if (!h.innkeeper || h.map != bot->GetMapId() || !(h.side == 3 || h.side == (alliance ? 1 : 2)))
                continue;
            float d = bot->GetDistance2d(h.innX, h.innY);
            if (!inn || d < innDist)
            {
                inn = &h;
                innDist = d;
            }
        }
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
        if (inn && innDist > 25.0f && innDist < 1200.0f && innDist / 7.0f < leftSeconds)
        {
            TemporaryTravelDestination* spot = new TemporaryTravelDestination(WorldPosition(inn->map, inn->innX, inn->innY, inn->innZ));
            TravelTarget newTarget = TravelTarget(ai);
            newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
            newTarget.SetTarget(spot, spot->GetPosition());
            Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
            travelTarget->SetConditions({});
            SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
            setNewTarget(requester, &newTarget, travelTarget);
            travelTarget->SetExpireIn(uint32((leftSeconds + 120.0f) * IN_MILLISECONDS));
            travelTarget->SetForced(true);
            {
                std::lock_guard<std::mutex> guard(restLock);
                restTrip[guid] = spot;
            }
            SilvermoonScoredGoal g;
            g.destination = spot;
            g.position = spot->GetPosition();
            g.kind = GoalKind::Other;
            g.score.score = 1.0f;
            g.score.reason = std::string("res sickness: rest at the inn, ") + inn->name;
            SilvermoonLogDecision(bot, g, nullptr, nullptr);
            SET_AI_VALUE2(int, "manual int", "silvermoon goal kind", int(GoalKind::Other) + 1);
            return true;
        }
        // at the inn, or no inn worth the walk: sit and wait it out
        if (!bot->IsSitState() && !bot->IsMoving())
            bot->SetStandState(UNIT_STAND_STATE_SIT);
        bool log = false;
        {
            std::lock_guard<std::mutex> guard(restLock);
            time_t& last = restLogged[guid];
            if (time(nullptr) - last > 10 * MINUTE)
            {
                last = time(nullptr);
                log = true;
            }
        }
        if (log)
        {
            SilvermoonScoredGoal g;
            g.kind = GoalKind::Other;
            g.score.score = 1.0f;
            g.score.reason = (inn && innDist <= 25.0f) ? std::string("res sickness: waiting at the inn, ") + inn->name :
                "res sickness: waiting it out here (no inn in reach)";
            SilvermoonLogDecision(bot, g, nullptr, nullptr);
        }
        SET_AI_VALUE2(int, "manual int", "silvermoon goal retry at", int(time(nullptr) + 30));
        return false;
    }

    // Food and water (S, 1.6.21): nearly out (under 5 food, or 5 water for a mana user) and able to pay
    // from its shopping budget (20% of its money, never the repair fund): a trip to the nearest friendly
    // inn within 800 yards - innkeepers sell both, and the buy step runs when a vendor is in reach. At
    // most one trip every 30 minutes.
    if (!bot->IsInCombat() && !SilvermoonCashOut(bot) && !SilvermoonIsOwn(bot))   // random bots (your characters: their Policies)
    {
        SilvermoonSupplyNeed need = SilvermoonSupplies(bot);
        uint32 lvl = bot->GetLevel();
        if (need.veryLow && SilvermoonSupplyBudget(bot) >= std::max<uint32>(100, lvl * lvl))
        {
            static std::mutex shopLock;
            static std::unordered_map<uint32, time_t> lastShop;
            bool due = false;
            {
                std::lock_guard<std::mutex> guard(shopLock);
                time_t& last = lastShop[bot->GetGUIDLow()];
                if (time(nullptr) - last >= 30 * MINUTE)
                    due = true;
            }
            bool alliance = bot->GetTeam() == ALLIANCE;
            Hub const* inn = nullptr;
            float innDist = 0.0f;
            if (due)
                for (Hub const& h : Hubs::All())
                {
                    if (!h.innkeeper || h.map != bot->GetMapId() || !(h.side == 3 || h.side == (alliance ? 1 : 2)))
                        continue;
                    float d = bot->GetDistance2d(h.innX, h.innY);
                    if (!inn || d < innDist)
                    {
                        inn = &h;
                        innDist = d;
                    }
                }
            if (inn && innDist > 25.0f && innDist < 800.0f)
            {
                {
                    std::lock_guard<std::mutex> guard(shopLock);
                    lastShop[bot->GetGUIDLow()] = time(nullptr);
                }
                travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
                TemporaryTravelDestination* spot = new TemporaryTravelDestination(WorldPosition(inn->map, inn->innX, inn->innY, inn->innZ));
                TravelTarget newTarget = TravelTarget(ai);
                newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
                newTarget.SetTarget(spot, spot->GetPosition());
                Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
                travelTarget->SetConditions({});
                SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
                setNewTarget(requester, &newTarget, travelTarget);
                travelTarget->SetExpireIn(uint32(10 * MINUTE * IN_MILLISECONDS));
                travelTarget->SetForced(true);
                SilvermoonScoredGoal g;
                g.destination = spot;
                g.position = spot->GetPosition();
                g.kind = GoalKind::Vendor;
                g.score.score = 1.0f;
                g.score.reason = std::string("supplies: buy food and water at the inn, ") + inn->name + " (" +
                    std::to_string(need.food) + " food, " + std::to_string(need.water) + " water)";
                SilvermoonLogDecision(bot, g, nullptr, nullptr);
                SET_AI_VALUE2(int, "manual int", "silvermoon goal kind", int(GoalKind::Vendor) + 1);
                return true;
            }
        }
    }

    // Gather order patrol (S, Nov 25: "idles far too long, thinks too much - point to point across
    // the entire zone, one big loop"): straight to the next point of the zone's route, without waiting
    // for the destination search or scoring anything. Only full bags go the normal way (to a vendor).
    if (AI_VALUE(uint8, "bag space") < 100 && !bot->IsInCombat())
    {
        WorldPosition next;
        uint32 index = 0, total = 0;
        if (SilvermoonGatherRouteNext(bot, next, index, total))
        {
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
            TemporaryTravelDestination* spot = new TemporaryTravelDestination(next);
            TravelTarget newTarget = TravelTarget(ai);
            newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
            newTarget.SetTarget(spot, spot->GetPosition());
            Player* requester = event.getOwner() ? event.getOwner() : (GetMaster() ? GetMaster() : bot);
            travelTarget->SetConditions({});
            SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
            setNewTarget(requester, &newTarget, travelTarget);
            // a stop it can't reach is skipped after 5 minutes; a stop in another zone (1.6.21: a zone with
            // nodes that still raise the skill) gets the walk there at about 6 yards a second, plus 2 minutes
            float far = next.getMapId() == bot->GetMapId() ? next.fDist(WorldPosition(bot)) : 0.0f;
            travelTarget->SetExpireIn(uint32(std::max(5.0f * MINUTE, far / 6.0f + 2.0f * MINUTE) * IN_MILLISECONDS));
            travelTarget->SetForced(true);
            SilvermoonScoredGoal g;
            g.destination = spot;
            g.position = spot->GetPosition();
            g.kind = GoalKind::Gather;
            g.score.score = 1.0f;
            g.score.reason = "gather route " + std::to_string(index) + "/" + std::to_string(total);
            SilvermoonLogDecision(bot, g, nullptr, nullptr);
            SET_AI_VALUE2(int, "manual int", "silvermoon goal kind", int(GoalKind::Gather) + 1);
            return true;
        }
    }

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
        // ...unless the bot got there: a goal that ended at its spot (S: Mommy walked to the Echo
        // Ridge laborers for Skirmish at Echo Ridge, the goal ended there, and the quest was then left
        // out for 30 minutes while she ground Kobold Workers next door)
        auto at = lastGoalAt.find(bot->GetGUIDLow());
        bool reached = at != lastGoalAt.end() && at->second.getMapId() == bot->GetMapId() && at->second.fDist(WorldPosition(bot)) < 120.0f;
        // ...and not a gathering spot: an empty or picked-over spot ends the goal at once, which
        // put the whole herb type on the 30-minute list - Mommy gathered for a minute, then had no
        // herb spots for the next ten and went questing (the patrol's visited-spot list handles it)
        // (remembered when the goal was chosen: the stored destination may be gone by now - reading
        // it here crashed the server twice on 2026-11-24)
        bool gatherDest = lastGoalGather[bot->GetGUIDLow()];
        // ...but a quest object the bot reached and still wants isn't there (S: Kojimbo stuck on
        // Chen's Empty Keg - 14 bots chose it 1,179 times; it has 9 spawn points but few spawned at
        // a time, so they kept walking to empty ones): that one goes on the list even when reached
        bool objectDest = lastGoalObject[bot->GetGUIDLow()];
        if (last != lastGoal.end() && last->second.first && now - last->second.second < ABANDONED_WITHIN && (!reached || objectDest) && !gatherDest)
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

    // Gather order = one continuous task (S: "cease everything and path point to point to herbs,
    // only stopping to fight if in danger"): nothing but gathering spots is left in the list. When
    // every spot nearby was visited in the last 15 minutes, the visited list starts over (spawns
    // have had time to come back) and the bot tries again in a moment rather than going questing.
    if (SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 186) > 1.0f || SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 182) > 1.0f ||
        SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 356) > 1.0f)
    {
        // ...except a vendor trip when the bags are completely full (S); the order carries on after
        bool bagsFull = AI_VALUE(uint8, "bag space") >= 100;
        goals.erase(std::remove_if(goals.begin(), goals.end(), [bagsFull](SilvermoonScoredGoal const& g)
            { return g.kind != GoalKind::Gather && !(bagsFull && g.kind == GoalKind::Vendor); }), goals.end());
        if (bagsFull)
            std::stable_sort(goals.begin(), goals.end(), [](SilvermoonScoredGoal const& a, SilvermoonScoredGoal const& b)
                { return a.kind == GoalKind::Vendor && b.kind != GoalKind::Vendor; });
        if (goals.empty())
        {
            {
                std::lock_guard<std::mutex> guard(goalLock);
                gatherVisited[bot->GetGUIDLow()].clear();
            }
            // try again at once with the list cleared (was 10 seconds of "nowhere to travel")
            SET_AI_VALUE2(int, "manual int", "silvermoon goal retry at", int(time(nullptr) + 1));
            return false;
        }
    }

    // settling in at a hub it just relocated to: leftover work back in the old zone counts a fifth
    {
        bool any = false;
        for (SilvermoonScoredGoal& g : goals)
        {
            if (!g.position || !(g.kind == GoalKind::QuestObjective || g.kind == GoalKind::QuestTurnIn ||
                g.kind == GoalKind::QuestGiver || g.kind == GoalKind::Grind || g.kind == GoalKind::Explore || g.kind == GoalKind::Gather))
                continue;
            float f = SilvermoonSettleFactor(bot->GetGUIDLow(), *g.position);
            if (f < 1.0f)
            {
                g.score.score *= f;
                g.score.reason = "away from new hub: " + g.score.reason;
                any = true;
            }
        }
        if (any)
            std::stable_sort(goals.begin(), goals.end(), [](SilvermoonScoredGoal const& a, SilvermoonScoredGoal const& b) { return a.score.score > b.score.score; });
    }

    // Professions tab: a learn order goes ahead of everything
    {
        SilvermoonScoredGoal prof;
        if (SilvermoonProfessionGoal(bot, ai, prof))
            goals.insert(goals.begin(), prof);
        SilvermoonScoredGoal go;
        if (SilvermoonGoGoal(bot, go))
            goals.insert(goals.begin(), go);
    }

    // Phase 2b: a quest hub elsewhere, when local options are thin (or now and then an unvisited one)
    uint16 relocateHub = 0;
    bool resumedTrip = false;
    {
        SilvermoonScoredGoal relocate;
        float bestLocal = goals.empty() ? 0.0f : goals.front().score.score;
        // a trip still on wins again, unless the bot needs a repair it can get or its bags are
        // nearly full (those come first, then the trip carries on)
        bool urgent = (AI_VALUE(bool, "should repair") && AI_VALUE(bool, "can repair")) || AI_VALUE(uint8, "bag space") > 90;
        if (!urgent && SilvermoonResumeTrip(bot, relocate, relocateHub))
        {
            goals.insert(goals.begin(), relocate);
            resumedTrip = true;
        }
        else if (SilvermoonBestRelocate(bot, ai, bestLocal, relocate, relocateHub))
            goals.insert(goals.begin(), relocate);
    }

    // action queue (S, 1.8): a quest, grind or train step keeps only the goals that serve it - the
    // quest's giver, objectives and turn-in; that mob's grind spots; the class trainer
    {
        uint8_t qKind = 0;
        uint32_t qId = 0;
        if (SilvermoonQueueStep(bot, qKind, qId) == 2)
        {
            std::vector<SilvermoonScoredGoal> fitting;
            for (SilvermoonScoredGoal const& g : goals)
            {
                TravelDestination* d = g.destination;
                if (!d)
                    continue;
                bool ok = false;
                if (qKind == AQ_QUEST)
                {
                    QuestTravelDestination* qd = dynamic_cast<QuestTravelDestination*>(d);
                    ok = qd && qd->GetQuestId() == qId;
                }
                else if (qKind == AQ_GRIND)
                    ok = d->GetPurpose() == TravelDestinationPurpose::Grind && d->GetEntry() == int32(qId);
                else if (qKind == AQ_TRAIN)
                    ok = d->GetPurpose() == TravelDestinationPurpose::Trainer;
                if (ok)
                {
                    fitting.push_back(g);
                    fitting.back().score.score = std::max(fitting.back().score.score, 1.0f) * 1000.0f;
                    fitting.back().score.reason = "queue: " + fitting.back().score.reason;
                }
            }
            if (!fitting.empty())
                goals = fitting;
            else if (SilvermoonQueueOnly(bot))
            {
                // queue only, and nothing found for the step yet: wait and look again (10 s, 1.8.2: S saw
                // the bot stand idle after reordering the queue)
                SET_AI_VALUE2(int, "manual int", "silvermoon goal retry at", int(time(nullptr) + 10));
                return false;
            }
        }
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
        lastGoalGather[bot->GetGUIDLow()] = best.kind == GoalKind::Gather;
        if (best.kind == GoalKind::Trainer)
            NoteTrainerTrip(bot);
        lastGoalObject[bot->GetGUIDLow()] = best.kind == GoalKind::QuestObjective && best.destination && best.destination->GetEntry() < 0;
        uint32 objectiveKey = 0;
        if (best.kind == GoalKind::QuestObjective)
            if (QuestObjectiveTravelDestination* o = dynamic_cast<QuestObjectiveTravelDestination*>(best.destination))
                objectiveKey = o->GetQuestId() * 8 + o->GetObjective();
        NoteObjectiveChoice(bot->GetGUIDLow(), objectiveKey);
        if (best.position)
            lastGoalAt[bot->GetGUIDLow()] = *best.position;
        else
            lastGoalAt.erase(bot->GetGUIDLow());
    }
    TravelTarget newTarget = TravelTarget(ai);
    newTarget.SetRelevance(AI_VALUE2(int, "manual int", "future travel relevance"));
    // gather order: the bot walks to the spot itself (a plain point), not to the herb type's travel
    // destination - that one counts as inactive whenever no node of the type is in sight, and the
    // movement layer then never starts the walk (S, 1.6.10: Mommy re-chose the same Peacebloom
    // spot every minute without moving). The 60-yard gathering scan picks up whatever is there.
    TravelDestination* targetDestination = best.destination;
    WorldPosition* targetPosition = best.position;
    if (best.kind == GoalKind::Gather && best.position && best.destination &&
        (SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 186) > 1.0f || SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 182) > 1.0f ||
         SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 356) > 1.0f))
    {
        TemporaryTravelDestination* spot = new TemporaryTravelDestination(*best.position, best.destination->GetEntry());
        targetDestination = spot;
        targetPosition = spot->GetPosition();
    }
    newTarget.SetTarget(targetDestination, targetPosition);
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
        // trip time allowed (S, Nov 24 audit: 1,024 trips were abandoned as "over twice the route
        // time" after a median of 7 minutes, still moving - 1,800 yards on average - because the
        // route estimate was often under a minute: hearthstone links and flight hops cost seconds
        // in the route search even when the stone is on cooldown). At least the straight-line walk
        // at running speed, and never under 10 minutes.
        float tripMinutes = best.score.minutes;
        if (best.position && best.position->getMapId() == bot->GetMapId())
            tripMinutes = std::max(tripMinutes, best.position->fDist(WorldPosition(bot)) / (7.0f * 60.0f));
        tripMinutes = std::max(tripMinutes, 10.0f);
        goals.front().score.minutes = tripMinutes;   // best is goals.front()
        SilvermoonRelocateStarted(bot->GetGUIDLow(), relocateHub, best.destination, best.score.minutes, resumedTrip);
        // the stock trip timer is twice the straight-line walk; a trip by road, boat or flight can
        // take longer, so give it twice the route estimate plus 5 minutes (the abandon rule's limit)
        travelTarget->SetExpireIn(uint32((2.0f * best.score.minutes + 5.0f) * MINUTE * IN_MILLISECONDS));
        travelTarget->SetForced(true);   // forced targets don't drop into cooldown on the way
    }
    if (best.score.reason.rfind("profession:", 0) == 0)
    {
        travelTarget->SetExpireIn(uint32(30 * MINUTE * IN_MILLISECONDS));
        travelTarget->SetForced(true);
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
