#include "playerbot/playerbot.h"
#include "TakeQuestsAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"

using namespace ai;

namespace
{
    const float NEAR_GIVER = 40.0f;      // the goal NPC this close: walk up and take its quests
    const int RETRY_SECONDS = 60;        // the same NPC at most once a minute
}

Unit* TakeQuestsAction::FindGiver()
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->IsActive())
        return nullptr;
    int32 entry = target->GetEntry();
    if (entry <= 0)
        return nullptr;
    for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "nearest npcs"))
    {
        Unit* unit = ai->GetUnit(guid);
        if (unit && int32(unit->GetEntry()) == entry && unit->IsAlive() && bot->GetDistance(unit) < NEAR_GIVER &&
            unit->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            return unit;
    }
    return nullptr;
}

bool TakeQuestsAction::isUseful()
{
    if (bot->IsInCombat() || !bot->IsAlive() || AI_VALUE(uint8, "free quest log slots") == 0)
        return false;
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->IsActive() || target->GetEntry() <= 0)
        return false;
    int32 entry = target->GetEntry();
    if (!AI_VALUE2(bool, "can accept quest npc", entry) && !AI_VALUE2(bool, "can turn in quest npc", entry))
        return false;
    if (AI_VALUE2(int, "manual int", "silvermoon take quests " + std::to_string(entry)) > int(time(nullptr)))
        return false;
    return FindGiver() != nullptr;
}

bool TakeQuestsAction::Execute(Event& event)
{
    Unit* giver = FindGiver();
    if (!giver)
        return false;
    if (sServerFacade.GetDistance2d(bot, giver) > INTERACTION_DISTANCE - 1.0f)
        return MoveNear(giver, INTERACTION_DISTANCE - 2.0f);

    SET_AI_VALUE2(int, "manual int", "silvermoon take quests " + std::to_string(giver->GetEntry()), int(time(nullptr) + RETRY_SECONDS));
    bool done = ai->DoSpecificAction("talk to quest giver", Event("silvermoon take quests", giver->GetObjectGuid()), true);
    done |= ai->DoSpecificAction("accept all quests", Event("silvermoon take quests", giver->GetObjectGuid()), true);
    // errand done: end the goal so the brain picks the next one now instead of the bot idling
    // (and grinding) by the quest giver until the travel target times out
    if (done)
        AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
    return done;
}
