
#include "playerbot/playerbot.h"
#include "NearestFriendlyPlayersValue.h"

#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

using namespace ai;
using namespace MaNGOS;

void NearestFriendlyPlayersValue::FindUnits(std::list<Unit*> &targets)
{
    AnyFriendlyUnitInObjectRangeCheck u_check(bot, range);
    UnitListSearcher<AnyFriendlyUnitInObjectRangeCheck> searcher(targets, u_check);
    Cell::VisitAllObjects(bot, searcher, range);
}

bool NearestFriendlyPlayersValue::AcceptUnit(Unit* unit)
{
    ObjectGuid guid = unit->GetObjectGuid();
    // Silvermoon patch: a player in GM mode (.gm on), visible or not, is not noticed: no
    // greetings, emotes, roleplay or chat aimed at them. Commands and whispers still work.
    if (guid.IsPlayer() && ((Player*)unit)->IsGameMaster())
        return false;
    return guid.IsPlayer() && guid != ai->GetBot()->GetObjectGuid();
}
