#include "playerbot/playerbot.h"
#include "CheetahAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"

using namespace ai;

bool TravelSpeedAction::Pick(std::string& action, uint32& spell, std::string& aura)
{
    switch (bot->getClass())
    {
        case CLASS_HUNTER: action = "aspect of the cheetah"; spell = 5118; aura = "aspect of the cheetah"; return true;
        case CLASS_SHAMAN: action = "ghost wolf"; spell = 2645; aura = "ghost wolf"; return true;
        case CLASS_DRUID:  action = "travel form"; spell = 783; aura = "travel form"; return true;
        case CLASS_ROGUE:  action = "sprint"; spell = 2983; aura = "sprint"; return true;
        default: return false;
    }
}

bool TravelSpeedAction::isUseful()
{
    std::string action, aura;
    uint32 spell = 0;
    if (!Pick(action, spell, aura))
        return false;
    if (bot->IsInCombat() || bot->IsMounted() || bot->IsTaxiFlying() || !bot->IsAlive() || bot->InBattleGround())
        return false;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return false;
    if (!bot->HasSpell(spell) || ai->HasAura(aura, bot) || !sServerFacade.IsSpellReady(bot, spell))
        return false;
    if (AI_VALUE2(int, "manual int", "silvermoon travel speed at") > int(time(nullptr)) - 60)
        return false;
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->IsActive() || !target->GetPosition())
        return false;
    WorldPosition* to = target->GetPosition();
    if (to->getMapId() == bot->GetMapId() && to->distance(WorldPosition(bot)) < 120.0f)
        return false;
    return true;
}

bool TravelSpeedAction::Execute(Event& event)
{
    std::string action, aura;
    uint32 spell = 0;
    if (!Pick(action, spell, aura))
        return false;
    SET_AI_VALUE2(int, "manual int", "silvermoon travel speed at", int(time(nullptr)));
    return ai->DoSpecificAction(action, Event("silvermoon travel speed"), true);
}
