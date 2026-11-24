
#include "playerbot/playerbot.h"
#include "LootTriggers.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/strategy/values/FreeMoveValues.h"

#include "playerbot/ServerFacade.h"
using namespace ai;

bool LootAvailableTrigger::IsActive()
{
    // Silvermoon (S: Mommy killed mobs on her herb patrol and walked off without looting them):
    // loot within 30 yards counts as long as nothing is attacking - "all targets" (any hostile in
    // sight) was rarely empty in mob-dense zones, so a corpse a few steps away was never looted
    return AI_VALUE(bool, "has available loot") &&
            (
                    sServerFacade.IsDistanceLessOrEqualThan(AI_VALUE2(float, "distance", "loot target"), INTERACTION_DISTANCE) ||
                    AI_VALUE(std::list<ObjectGuid>, "all targets").empty() ||
                    (!AI_VALUE(bool, "has attackers") && sServerFacade.IsDistanceLessOrEqualThan(AI_VALUE2(float, "distance", "loot target"), 30.0f))
            ) &&
            !AI_VALUE2(bool, "combat", "self target") &&
            !AI_VALUE2(bool, "mounted", "self target");
}

bool FarFromCurrentLootTrigger::IsActive()
{
    LootObject loot = AI_VALUE(LootObject, "loot target");

    if (!loot.IsLootPossible(bot))
        return false;

    if (WorldObject* wo = loot.GetWorldObject(bot))
    {
        if (!CanFreeMoveValue::CanFreeMoveTo(ai, WorldPosition(wo)))
            return false;
    }

    return AI_VALUE2(float, "distance", "loot target") > INTERACTION_DISTANCE;
}

bool CanLootTrigger::IsActive()
{
    return AI_VALUE(bool, "can loot");
}
