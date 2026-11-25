#include "playerbot/strategy/Action.h"
#include "ChooseTargetActions.h"
#include "MotionGenerators/MovementGenerator.h"
#include "AI/BaseAI/CreatureAI.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/silvermoon/GoalScorer.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/values/FreeMoveValues.h"

bool DpsAssistAction::isUseful()
{
    // if carry flag, do not start fight
    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return false;

    return true;
}

bool AttackAnythingAction::isUseful() 
{
    if (!ai->AllowActivity(GRIND_ACTIVITY)) //Bot not allowed to be active
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    Unit* target = GetTarget();

    if (!target || !ai->IsSafe(target))
        return false;

    bool pinnedByOrder = ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT);
    if (pinnedByOrder && target->GetDistance(bot) >= ai->GetRange("spell"))
        return false;

    if (ai->ContainsStrategy(STRATEGY_TYPE_HEAL) && !ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT))
        return false;

    if(!target->IsPlayer() && bot->isInFront(target,target->GetAttackDistance(bot)*1.5f, M_PI_F*0.5f) && target->CanAttackOnSight(bot) && target->GetLevel() < bot->GetLevel() + 3.0) //Attack before being attacked.
        return true;

    // Silvermoon patch: a brain bot on an errand (quest giver, turn-in, vendor, repair, trainer,
    // mail, relocation trip) doesn't stop to grind whatever is near (Grashnak spent his first
    // level killing scorpids on the way to a quest giver). Mobs about to attack it are still
    // fought (above), and grind/quest-objective/gather goals keep grinding.
    if (ai->HasStrategy("silvermoon brain", BotState::BOT_STATE_NON_COMBAT) && AI_VALUE(TravelTarget*, "travel target")->IsActive())
    {
        int kind = AI_VALUE2(int, "manual int", "silvermoon goal kind") - 1;
        switch (kind)
        {
            case int(silvermoon::GoalKind::QuestTurnIn): case int(silvermoon::GoalKind::QuestGiver):
            case int(silvermoon::GoalKind::Vendor): case int(silvermoon::GoalKind::Repair):
            case int(silvermoon::GoalKind::Trainer): case int(silvermoon::GoalKind::Mail):
            case int(silvermoon::GoalKind::Relocate):
                return false;
            default:
                break;
        }
    }

    if (AI_VALUE(bool, "travel target traveling") && CanFreeMoveValue::CanFreeMoveTo(ai, *AI_VALUE(TravelTarget*,"travel target")->GetPosition()) &&
        sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "travel target"), sPlayerbotAIConfig.sightDistance)) 
        return false;

    return true;
}

bool ai::AttackAnythingAction::isPossible()
{
    return AttackAction::isPossible() && GetTarget();
}

bool ai::AttackAnythingAction::Execute(Event& event)
{
    bool result = AttackAction::Execute(event);
    if (result)
    {
        Unit* grindTarget = GetTarget();
        if (grindTarget)
        {
            std::string grindName = grindTarget->GetName();
            if (!grindName.empty())
            {
                sPlayerbotAIConfig.logEvent(ai, "AttackAnythingAction", grindName, std::to_string(grindTarget->GetEntry()));

                // Silvermoon (S: melee levels fall behind): warriors and rogues pull with their
                // ranged weapon - bow, gun or crossbow with ammo, or a thrown weapon - but only when
                // other enemies stand within 15 yards of the target, so a lone mob is still charged
                bool meleePuller = bot->getClass() == CLASS_WARRIOR || bot->getClass() == CLASS_ROGUE;
                bool crowdedTarget = false;
                bool thrown = false;
                if (meleePuller)
                {
                    if (Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
                        thrown = ranged->GetProto() && ranged->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_THROWN;
                    for (ObjectGuid const& g : AI_VALUE(std::list<ObjectGuid>, "possible targets"))
                        if (g != grindTarget->GetObjectGuid())
                            if (Unit* u = ai->GetUnit(g))
                                if (u->IsAlive() && u->GetDistance(grindTarget) < 15.0f)
                                {
                                    crowdedTarget = true;
                                    break;
                                }
                }
                if (ai->HasStrategy("pull", BotState::BOT_STATE_COMBAT) && (!meleePuller || crowdedTarget))
                {
                    if (PullStrategy* strategy = PullStrategy::Get(ai))
                    {
                        if (strategy->CanDoPullAction(grindTarget) && (ai->GetBot()->getClass() == CLASS_DRUID || ai->GetBot()->getClass() == CLASS_PALADIN || thrown || AI_VALUE2(uint32, "item count", "ammo")))
                        {
                            Event pullEvent("attack anything", grindTarget->GetObjectGuid());
                            bool doAction = ai->DoSpecificAction("pull my target", pullEvent, true);

                            if (doAction)
                            {
                                return true;
                            }
                        }
                    }
                }

                context->GetValue<ObjectGuid>("attack target")->Set(grindTarget->GetObjectGuid());
                ai->StopMoving();
            }
        }
    }

    return result;
}

bool AttackEnemyPlayerAction::isUseful()
{
    return !sPlayerbotAIConfig.IsInPvpProhibitedZone(sServerFacade.GetAreaId(bot));
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Unit* target = ai->GetUnit(context->GetValue<ObjectGuid>("enemy flag carrier")->Get());
    return target && sServerFacade.IsDistanceLessOrEqualThan(sServerFacade.GetDistance2d(bot, target), 75.0f) && (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976));
}

bool SelectNewTargetAction::Execute(Event& event)
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (target && sServerFacade.UnitIsDead(target))
    {
        // Save the dead target for later looting
        ObjectGuid guid = target->GetObjectGuid();
        if (guid)
        {
            AI_VALUE(LootObjectStack*, "available loot")->Add(guid);
        }
    }

    // Clear the target variables
    ObjectGuid attackTarget = AI_VALUE(ObjectGuid, "attack target");
    std::list<ObjectGuid> possible = AI_VALUE(std::list<ObjectGuid>, "possible targets no los");
    if (attackTarget && find(possible.begin(), possible.end(), attackTarget) == possible.end())
    {
        SET_AI_VALUE(ObjectGuid, "attack target", ObjectGuid());
    }

    // Save the old target and clear the current target
    if(target)
    {
        SET_AI_VALUE(ObjectGuid, "old target", target->GetObjectGuid());
        SET_AI_VALUE(ObjectGuid, "current target", ObjectGuid());
    }
    
    // Stop attacking
    bot->SetSelectionGuid(ObjectGuid());
    ai->InterruptSpell();
    bot->AttackStop();
    // Stop pet attacking
    Pet* pet = bot->GetPet();
    if (pet)
    {
        UnitAI* creatureAI = ((Creature*)pet)->AI();
        if (creatureAI)
        {
            // Send pet action packet
            const ObjectGuid& petGuid = pet->GetObjectGuid();
            const ObjectGuid& targetGuid = ObjectGuid();
            const uint8 flag = ACT_COMMAND;
            const uint32 spellId = COMMAND_FOLLOW;
            const uint32 command = (flag << 24) | spellId;

            WorldPacket data(CMSG_PET_ACTION);
            data << petGuid;
            data << command;
            data << targetGuid;
            bot->GetSession()->HandlePetAction(data);
        }
    }

    bool moreAttackers = false;
    // Check if there is any enemy targets available to attack
    if (AI_VALUE(bool, "has attackers"))
    {
        if (ai->HasStrategy("pvp", BotState::BOT_STATE_COMBAT) ||
            ai->HasStrategy("duel", BotState::BOT_STATE_COMBAT))
        {
            // Check if there is an enemy player nearby
            if (AI_VALUE(bool, "has enemy player targets"))
            {
                moreAttackers = true;
                return ai->DoSpecificAction("attack enemy player", event, true);
            }
        }

        // Let the dps/tank assist pick a target to attack
        if (ai->HasStrategy("dps assist", BotState::BOT_STATE_NON_COMBAT))
        {
            moreAttackers = true;
            return ai->DoSpecificAction("dps assist", event, true);
        }
        else if (ai->HasStrategy("tank assist", BotState::BOT_STATE_NON_COMBAT))
        {
            moreAttackers = true;
            return ai->DoSpecificAction("tank assist", event, true);
        }
    }

    return false;
}