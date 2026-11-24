#include "playerbot/playerbot.h"
#include "CheetahAction.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/ItemUsageValue.h"

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

bool HearthShortcutAction::isUseful()
{
    if (bot->IsInCombat() || bot->IsTaxiFlying() || !bot->IsAlive() || bot->InBattleGround() || bot->IsBeingTeleported())
        return false;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 1)
        return false;
    if (!bot->HasItemCount(6948, 1) || !AI_VALUE2(bool, "action useful", "hearthstone"))
        return false;
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->IsActive() || !target->GetPosition())
        return false;
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
    uint32 hmap = 0;
    bot->GetHomebindLocation(hx, hy, hz, hmap);
    WorldPosition home(hmap, hx, hy, hz), here(bot);
    WorldPosition* to = target->GetPosition();
    if (to->getMapId() != hmap || to->fDist(home) > 150.0f)
        return false;                                    // the goal isn't by the inn
    bool farFromGoal = to->getMapId() != here.getMapId() || to->fDist(here) > 450.0f;
    bool farFromHome = hmap != here.getMapId() || home.fDist(here) > 450.0f;
    return farFromGoal && farFromHome;
}

bool HearthShortcutAction::Execute(Event& event)
{
    return ai->DoSpecificAction("hearthstone", Event("silvermoon hearth shortcut"), true);
}

bool ErrandDoneAction::isUseful()
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (!target || !target->IsActive() || !target->GetDestination())
        return false;
    // only once it got there (a vendor goal picked with fairly empty bags would otherwise end at once)
    WorldPosition* at = target->GetPosition();
    if (!at || at->getMapId() != bot->GetMapId() || at->distance(WorldPosition(bot)) > 60.0f)
        return false;
    TravelDestinationPurpose purpose = target->GetDestination()->GetPurpose();
    if (purpose == TravelDestinationPurpose::Vendor)
        return AI_VALUE(uint8, "bag space") < 40;
    if (purpose == TravelDestinationPurpose::Repair)
        return AI_VALUE(uint8, "durability") > 90;
    if (purpose == TravelDestinationPurpose::AH)
        return AI_VALUE2(std::list<Item*>, "inventory items", "usage " + std::to_string((uint8)ItemUsage::ITEM_USAGE_AH)).size() < 2;
    return false;
}

bool ErrandDoneAction::Execute(Event& event)
{
    if (TravelTarget* target = AI_VALUE(TravelTarget*, "travel target"))
        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
    return true;
}
