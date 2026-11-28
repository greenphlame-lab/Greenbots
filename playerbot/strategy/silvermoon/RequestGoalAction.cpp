#include "playerbot/playerbot.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "Policies.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include "RequestGoalAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/actions/ChooseTravelTargetAction.h"
#include "ChooseGoalAction.h"
#include "Professions.h"
#include "ActionQueue.h"
#include "playerbot/silvermoon/Recovery.h"
#include <unordered_map>
#include <mutex>

using namespace ai;

std::vector<SilvermoonGoalFetch> ai::SilvermoonBuildGoalFetches(PlayerbotAI* ai, Player* bot)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    std::vector<SilvermoonGoalFetch> fetches;
    uint32 level = bot->GetLevel();

    // quest givers nearby (same range as the stock quest request)
    fetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, {}, 400.0f + level * 10.0f });

    // turn-ins and objectives for every quest in the log (the stock quest request's loop, for
    // this bot only)
    for (auto& [questId, questStatus] : bot->getQuestStatusMap())
    {
        if (questStatus.m_rewarded)
            continue;
        Quest const* questTemplate = sObjectMgr.GetQuestTemplate(questId);
        if (!questTemplate)
            continue;
        uint32 flag = 0;
        if (bot->CanRewardQuest(questTemplate, false))
            flag = (uint32)TravelDestinationPurpose::QuestTaker;
        else
        {
            for (uint32 objective = 0; objective < 4; objective++)
            {
                std::vector<std::string> qualifier = { std::to_string(questId), std::to_string(objective) };
                if (context->GetValue<bool>("need quest objective", Qualified::MultiQualify(qualifier, ","))->Get())
                    flag |= (uint32)(1 << (objective + 1));
            }
        }
        // 1.8.6 (S: "a distant trip ... will work under the action queue?"): the queued quest's turn-in and
        // objectives are looked for across the whole continent (a level 3 otherwise searched 1,700 yards)
        if (flag)
            fetches.push_back({ flag, { (int32)questId }, questId == SilvermoonQueueFetchQuest(bot) ? 10000.0f : 1000.0f + (level * level) * 75.0f });
    }

    // action queue (S, 1.8): the top step's quest giver and grind spots from much further out, and
    // the class trainer
    if (uint32 q = SilvermoonQueueFetchQuest(bot))
        if (bot->GetQuestStatus(q) != QUEST_STATUS_INCOMPLETE && bot->GetQuestStatus(q) != QUEST_STATUS_COMPLETE)
            fetches.push_back({ (uint32)TravelDestinationPurpose::QuestGiver, { (int32)q }, 10000.0f });
    if (uint32 mob = SilvermoonQueueGrindEntry(bot))
        fetches.push_back({ (uint32)TravelDestinationPurpose::Grind, { (int32)mob }, 8000.0f });
    {
        uint8_t qKind = 0;
        uint32_t qId = 0;
        if (SilvermoonQueueStep(bot, qKind, qId) == 2 && qKind == AQ_TRAIN)
        {
            std::vector<int32> trainers = context->GetValue<std::vector<int32>>("available trainers", std::to_string((uint32)TRAINER_TYPE_CLASS))->Get();
            if (!trainers.empty())
                fetches.push_back({ (uint32)TravelDestinationPurpose::Trainer, trainers, 10000.0f });
        }
    }

    // grind spots, always
    fetches.push_back({ (uint32)TravelDestinationPurpose::Grind, {}, 2000.0f });

    // chores when due
    // only with something a vendor will actually buy: Zugzug's bags were 65%+ full of things it
    // keeps, so "should sell" stayed true and it walked from vendor to vendor in Tarren Mill
    // ...or cashing out (S, 1.6.20): broken gear and no money for a full repair, whatever the bag fill
    if ((context->GetValue<bool>("should sell")->Get() || SilvermoonCashOut(bot)) && context->GetValue<bool>("can sell")->Get())
        fetches.push_back({ (uint32)TravelDestinationPurpose::Vendor, {}, 10000.0f });
    // 1.8.6 (S: "they should sell their loot first then repair"): cashing out, the trip goes to a
    // repairer - they buy loot too - so the money from selling is spent on repairs at the same stop
    if (SilvermoonCashOut(bot) && context->GetValue<bool>("can sell")->Get())
        fetches.push_back({ (uint32)TravelDestinationPurpose::Repair, {}, 10000.0f });
    // only when it can pay: Taxes (level 28, 43 copper) walked between Razor Hill's repairers for an
    // hour ("can repair" checks the cheapest repair against the money it may spend on repairs)
    if (context->GetValue<bool>("should repair")->Get() && context->GetValue<bool>("can repair")->Get())
        fetches.push_back({ (uint32)TravelDestinationPurpose::Repair, {}, 10000.0f });
    // broke with damaged gear: gathering is safe money for a miner or herbalist
    if (context->GetValue<bool>("should repair")->Get() && !context->GetValue<bool>("can repair")->Get())
    {
        if (bot->GetSkillValue(SKILL_MINING) > 0)
            fetches.push_back({ (uint32)TravelDestinationPurpose::GatherMining, {}, 1500.0f });
        if (bot->GetSkillValue(SKILL_HERBALISM) > 0)
            fetches.push_back({ (uint32)TravelDestinationPurpose::GatherHerbalism, {}, 1500.0f });
    }
    // Professions window "Gather" order (S: clicking Gather did nothing - gathering spots were only
    // fetched for a broke bot): gathering spots for the ordered skill, always while the order lasts
    if (SilvermoonGatherOrderFactor(bot->GetGUIDLow(), SKILL_MINING) > 1.0f)
        fetches.push_back({ (uint32)TravelDestinationPurpose::GatherMining, {}, 3000.0f });
    if (SilvermoonGatherOrderFactor(bot->GetGUIDLow(), SKILL_HERBALISM) > 1.0f)
        fetches.push_back({ (uint32)TravelDestinationPurpose::GatherHerbalism, {}, 3000.0f });
    if (SilvermoonGatherOrderFactor(bot->GetGUIDLow(), SKILL_FISHING) > 1.0f)
        fetches.push_back({ (uint32)TravelDestinationPurpose::GatherFishing, {}, 3000.0f });
    // mail: stock bots fetch it after an hour or more (the brain silenced that request), and mail
    // from a real player right away (Velyna never picked up a cloak sent to her)
    if (context->GetValue<bool>("can get mail")->Get() &&
        (SilvermoonHasPlayerMail(bot) || context->GetValue<bool>("should get mail")->Get()))
        fetches.push_back({ (uint32)TravelDestinationPurpose::Mail, {}, 10000.0f });
    if (context->GetValue<bool>("should travel named", "trainer class")->Get())
    {
        std::vector<int32> trainers = context->GetValue<std::vector<int32>>("available trainers", std::to_string((uint32)TRAINER_TYPE_CLASS))->Get();
        if (!trainers.empty())
            fetches.push_back({ (uint32)TravelDestinationPurpose::Trainer, trainers, 10000.0f });
    }

    // auction house (S: ore, herbs and cloth almost never reached the AH - 9,000 Linen Cloth sat in
    // bot bags, 93 auctions in all): the brain never planned an AH trip, bots only posted when they
    // happened to pass an auctioneer. Now: 6+ stacks marked for the AH make it a chore
    std::list<Item*> ahItems = context->GetValue<std::list<Item*>>("inventory items", "usage " + std::to_string((uint8)ItemUsage::ITEM_USAGE_AH))->Get();
    if (SilvermoonIsOwn(bot) ? SilvermoonAhTripDue(bot, ahItems) : ahItems.size() >= 6u)   // your characters: your "AH trip when worth" gold (no setting: even one item)
        fetches.push_back({ (uint32)TravelDestinationPurpose::AH, {}, 10000.0f });

    // a little exploring, short range only (worth little until Phase 2 adds quest hubs)
    fetches.push_back({ (uint32)TravelDestinationPurpose::Explore, {}, 500.0f });

    return fetches;
}

FutureDestinations ai::SilvermoonLaunchGoalFetch(Player* bot, std::vector<SilvermoonGoalFetch> fetches)
{
    WorldPosition center(bot);
    return std::async(std::launch::async, [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center, fetches]()
        {
            silvermoon::BrainTimer brainTimer(silvermoon::BrainAsyncMicros());
            PartitionedTravelList list;
            for (auto const& fetch : fetches)
            {
                PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo, fetch.purpose, fetch.entries, true, fetch.range);
                for (auto& [partition, points] : subList)
                    list[partition].insert(list[partition].end(), points.begin(), points.end());
            }
            return list;
        });
}

bool RequestGoalAction::Execute(Event& event)
{
    silvermoon::BrainTimer brainTimer(silvermoon::BrainMicros());
    // 1.7.4 diagnostic (S: Go trips re-planned every few seconds - 138 times in 9 minutes for one
    // bot - and the bots stood still): what ended the trip, logged at most once a minute per bot
    if (SilvermoonGoActive(bot))
    {
        static std::mutex diagLock;
        static std::unordered_map<uint32, time_t> lastDiag;
        bool log = false;
        {
            std::lock_guard<std::mutex> guard(diagLock);
            time_t& at = lastDiag[bot->GetGUIDLow()];
            if (time(nullptr) - at >= MINUTE)
            {
                at = time(nullptr);
                log = true;
            }
        }
        if (log)
        {
            TravelTarget* old = AI_VALUE(TravelTarget*, "travel target");
            sLog.outBasic("Silvermoon: %s Go trip ended early: status %u, last action %s / %s, %.0f yards to go", bot->GetName(),
                uint32(old->GetStatus()), ai->GetLastAction(BotState::BOT_STATE_NON_COMBAT).c_str(),
                ai->GetLastAction(BotState::BOT_STATE_COMBAT).c_str(),
                old->GetPosition() ? old->GetPosition()->distance(WorldPosition(bot)) : -1.0f);
        }
    }
    *AI_VALUE(FutureDestinations*, "future travel destinations") = SilvermoonLaunchGoalFetch(bot, SilvermoonBuildGoalFetches(ai, bot));

    AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
    SET_AI_VALUE2(std::string, "manual string", "future travel purpose", "goal");
    // No travel condition. The stock requests pass their trigger (e.g. "should sell") so the target
    // ends when the need is gone; ours was "not travel target active", which turns false as soon as
    // the goal is set, so the target died within seconds and the bot chose again in place (32%
    // of decisions came less than 10 seconds apart). A goal ends when its destination does.
    SET_AI_VALUE2(std::string, "manual string", "future travel condition", "");
    SET_AI_VALUE2(int, "manual int", "future travel relevance", relevance * 100);
    return true;
}

bool RequestGoalAction::isUseful()
{
    silvermoon::BrainTimer brainTimer(silvermoon::BrainMicros());
    if (bot->InBattleGround() || !ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;
    // Phase 1 is for solo bots: a group (even one this bot leads) keeps the stock travel logic
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return false;
    if (!AI_VALUE(bool, "can move around"))
        return false;
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (target->IsActive() || target->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;
    // after a fetch that found nothing, wait a little before trying again
    if (time(nullptr) < AI_VALUE2(int, "manual int", "silvermoon goal retry at"))
        return false;
    return true;
}
