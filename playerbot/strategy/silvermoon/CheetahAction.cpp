#include "playerbot/playerbot.h"
#include "CheetahAction.h"
#include "playerbot/TravelMgr.h"

using namespace ai;

bool TravelCheetahAction::isUseful()
{
    if (bot->getClass() != CLASS_HUNTER || bot->IsInCombat() || bot->IsMounted() || bot->IsTaxiFlying() || !bot->IsAlive())
        return false;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return false;
    if (!bot->HasSpell(5118) || ai->HasAura("aspect of the cheetah", bot))
        return false;
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->IsActive() || !target->GetPosition())
        return false;
    WorldPosition* to = target->GetPosition();
    if (to->getMapId() == bot->GetMapId() && to->distance(WorldPosition(bot)) < 120.0f)
        return false;
    return CastBuffSpellAction::isUseful();
}
