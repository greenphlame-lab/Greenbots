
#include "playerbot/playerbot.h"
#include "GrindTargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "AttackersValue.h"
#include "PossibleAttackTargetsValue.h"
#include "playerbot/strategy/actions/ChooseTargetActions.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "Tools/Formulas.h"
#include "playerbot/TravelNode.h"
#include "playerbot/strategy/silvermoon/RelocateGoal.h"
#include "playerbot/silvermoon/Recovery.h"
#include "playerbot/strategy/silvermoon/ActionQueue.h"

using namespace ai;
// Silvermoon (S): grey mobs (no XP) are only worth it for a destitute bot - weapon at 0 durability -
// which farms them for coin; everyone else leaves them alone unless a quest needs them. (Stock let 1
// in 50 through and, on long trips, bots spent their time killing grey mobs.)
namespace ai { uint8 SilvermoonTravelRules(Player* bot); bool SilvermoonIsOwn(Player* bot); float SilvermoonGatherOrderFactor(uint32 guid, uint32 skill); uint8 SilvermoonFactionNpcKind(Player* bot, Unit* unit); bool SilvermoonGoActive(Player* bot); uint32_t SilvermoonQueueGrindEntry(Player* bot); }

static bool SilvermoonGreyAllowed(Player* bot)
{
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    return weapon && weapon->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && !weapon->GetUInt32Value(ITEM_FIELD_DURABILITY);
}


ObjectGuid GrindTargetValue::Calculate()
{
    // Silvermoon patch: no new fights while resurrection sickness lasts (it is still
    // defended when attacked); a weakened bot pulling mobs just dies again.
    if (ai->HasAura(SPELL_ID_PASSIVE_RESURRECTION_SICKNESS, bot) && !ai->HasActivePlayerMaster())
        return ObjectGuid();

    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = NULL;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target ? target->GetObjectGuid() : ObjectGuid();
}

Unit* GrindTargetValue::FindTargetForGrinding(int assistCount)
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() || !master->GetPlayerbotAI()))
        master = nullptr;

    std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
    for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); i++)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit || !sServerFacade.IsAlive(unit))
            continue;

        // S (1.6.20): a guard on the bot is run from (silvermoon flee guard), never picked to fight
        if (!bot->InBattleGround() && SilvermoonFactionNpcKind(bot, unit) == 1)
            continue;

        if (!bot->InBattleGround() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit)))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + "(hostile) ignored (out of free range).");
            continue;
        }

        if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) +"(hostile) selected.");
       
        return unit;
    }

    std::list<ObjectGuid> targets = *context->GetValue<std::list<ObjectGuid> >("possible targets");

    if (targets.empty())
        return NULL;

    float distance = 0;
    Unit* result = NULL;

    bool travelTargetWorking = AI_VALUE(bool, "travel target working");
    bool travelTargetTraveling = AI_VALUE(bool, "travel target traveling");
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    bool isGrindTravelDest = travelTarget && typeid(travelTarget->GetDestination()) == typeid(GrindTravelDestination);

    struct MemberInfo {
        Player* player;
        float x, y;
    };
    std::vector<MemberInfo> groupMembers;
    if (group)
    {
        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        groupMembers.reserve(groupSlot.size());
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* member = sObjectMgr.GetPlayer(itr->guid);
            if (member && sServerFacade.IsAlive(member))
                groupMembers.push_back({ member, member->GetPositionX(), member->GetPositionY() });
        }
    }

    std::unordered_map<uint32, bool> needForQuestCache;

    uint32 queueGrind = SilvermoonQueueGrindEntry(bot);
    int careful = -1;   // Silvermoon careful mode, worked out on first use
    for (std::list<ObjectGuid>::iterator tIter = targets.begin(); tIter != targets.end(); tIter++)
    {
        Unit* unit = ai->GetUnit(*tIter);
        if (!unit)
            continue;

        // action queue (S, 1.8): a "grind <mob>" step pulls only that mob
        if (queueGrind && unit->GetEntry() != queueGrind)
            continue;

        // S (1.6.20): leave the other faction's NPCs alone. A guard (either side, or a neutral town's)
        // is never pulled; another opposite-faction NPC (quest giver, vendor, farmer...) only when an
        // active quest needs it killed.
        if (!bot->InBattleGround())
        {
            uint8 kind = SilvermoonFactionNpcKind(bot, unit);
            if (kind == 1 || (kind == 2 && !AI_VALUE2(bool, "need for quest", std::to_string(unit->GetEntry()))))
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (guard or other faction's NPC).");
                continue;
            }
        }

#ifdef MANGOSBOT_TWO 
        if (bot->GetMapId() == 609)
        {
            switch (unit->GetEntry())
            {
            case 28605: //Havenshire Stallion
            case 28606: //Havenshire Mare
            case 28607: //Havenshire Cotl
            case 28891: //Scarlet Miner
            case 28819: //Scarlet Miner
            case 28769: //Shadowy tormentor
            case 28768: //Dark Rider of Archeus
                continue;
            case 29080: //Scarlet Champion
            case 29029: //Scarlet Inquisitor
            case 29000: //Scarlet Commander Rodrick
            case 28940: //Scarlet Crusader
            case 28939: //Scarlet Preacher
            case 28936: //Scarlet Commander
            case 28898: //Scarlet Captain
            case 28896: //Scarlet Infantryman
            case 28895: //Scarlet Medic
            case 28892: //Scarlet Peasant
            case 28610: //Scarlet Marksman
                if (AI_VALUE2(bool, "need quest objective", "12680,0"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12701"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12727"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12754"))
                    continue;                
                break;
            case 29102: //HearthglenCrusader
            case 29103: //Tirisfal Crusader
            case 29104: //Scarlet Ballista
                if (AI_VALUE2(bool, "need quest objective", "12779"))
                {
                    if (AI_VALUE2(bool, "trigger active", "in vehicle") && bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.sightDistance))
                        return unit;
                    continue;
                }
                break;
            case 28834: //Scarlet Fleet Defender
            case 28850: //Scarlet Land Canon
            case 28856: //Scarlet Fleet Guardian
                if (AI_VALUE2(bool, "need quest objective", "12701,0"))
                {
                    if (AI_VALUE2(bool, "trigger active", "in vehicle::Scarlet Cannon") && !urand(0, 5))
                        return unit;
                    continue;
                }
            }
        }
#endif

        if (careful < 0)
        {
            uint32 broken = 0;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && !item->GetUInt32Value(ITEM_FIELD_DURABILITY))
                        broken += slot == EQUIPMENT_SLOT_MAINHAND ? 4 : 1;
            // ...or struggling (S, 1.6.20: dying and losing money over the last hour)
            careful = (broken >= 3 || SilvermoonRecentDeaths(bot->GetGUIDLow(), 30 * MINUTE) >= 3 || SilvermoonStruggling(bot->GetGUIDLow())) ? 1 : 0;
            // action queue, "queue only" (1.8.2, S: Chino on Coldridge Valley Mail Delivery ran around
            // killing wolves for other quests): on a queued quest, new fights only at that quest's own
            // objective - on the way to its giver or turn-in, and anywhere else, only what attacks
            uint32 queueQuest = SilvermoonQueueOnly(bot) ? SilvermoonQueueFetchQuest(bot) : 0;
            QuestObjectiveTravelDestination* atObjective = queueQuest ?
                dynamic_cast<QuestObjectiveTravelDestination*>(AI_VALUE(TravelTarget*, "travel target")->GetDestination()) : nullptr;
            if (bot->HasAura(15007))
                careful = 2;    // Resurrection Sickness: no new fights until it wears off
            else if (queueQuest && !(atObjective && atObjective->GetQuestId() == queueQuest))
                careful = 2;
            else if (SilvermoonOnRelocateTrip(bot->GetGUIDLow()))
                careful = 2;    // on a relocation trip: keep moving, fight only what attacks
            // Gather order (S): node to node, no fights on the way unless something attacks
            // (mining, herbalism, fishing - a skinning order is about killing beasts)
            else if (SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 186) > 1.0f || SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 182) > 1.0f ||
                     SilvermoonGatherOrderFactor(bot->GetGUIDLow(), 356) > 1.0f)
                careful = 2;
            // policy (S): "avoid mobs while travelling" - no new pulls on the way anywhere. 1.7.1 (S:
            // Mommy fought her way to the herbalism trainer): a Go to trip counts the whole way, and
            // so does any trip still being set up (not only while the status reads "travelling")
            else if ((SilvermoonTravelRules(bot) & 1) &&
                     (AI_VALUE(bool, "travel target traveling") || SilvermoonGoActive(bot) ||
                      AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE ||
                      AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_READY))
                careful = 2;
        }
        // Silvermoon patch: a random bot on its own (no real player leading it) leaves alone
        // mobs 3 or more levels above it and elites it cannot handle alone.
        // (also your own characters playing on the brain: Auto, and always-online alts with auto on)
        if (((sRandomPlayerbotMgr.IsRandomBot(bot) && !ai->HasRealPlayerMaster()) || ai->IsRealPlayer() ||
             (sPlayerbotAIConfig.IsFreeAltBot(bot) && SilvermoonAltAuto(bot->GetGUIDLow()))) &&
            (!bot->GetGroup() || bot->GetGroup()->GetMembersCount() < 2) &&
            !bot->InBattleGround() && unit->GetTypeId() == TYPEID_UNIT)
        {
            if (int32(unit->GetLevel()) >= int32(bot->GetLevel()) + 3)
                continue;
            if (((Creature*)unit)->IsElite() && int32(unit->GetLevel()) >= int32(bot->GetLevel()) - 2)
                continue;
            // careful mode (S): with broken gear (weapon or 3+ items at 0 durability) or 3+ deaths
            // in 30 minutes, only green mobs (3+ levels below) are picked as new targets
            if (careful == 2)
                continue;
            // destitute bots pick grey mobs too (coin and vendor loot, no danger)
            if (careful && int32(unit->GetLevel()) > int32(bot->GetLevel()) - 3)
                continue;
            // careful mode, tightened (S asked whether careful bots still die to green mobs: they
            // did - 57% of their deaths, some even to grey mobs - through elites at any level, camp
            // packs and pulling again while hurt):
            //  - no elites at all
            //  - no new pull below 60% health or, for mana users, 40% mana: rest first
            if (careful && ((Creature*)unit)->IsElite())
                continue;
            if (careful && (bot->GetHealthPercent() < 60.0f ||
                (bot->GetPowerType() == POWER_MANA && bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPower(POWER_MANA) * 100 < bot->GetMaxPower(POWER_MANA) * 40)))
                continue;
            // single pulls: skip a mob with another idle hostile mob right next to it (two at once
            // is how most ordinary fights were lost)
            // packs (S: Kojimbo pulled a 4-mob worgen pack): skip a mob with an idle hostile within
            // 12 yards (15 in careful mode; was 8), or with 2+ idle hostiles within 20 yards - camps
            // and packs that come together
            bool pack = false;
            uint32 around = 0;
            for (ObjectGuid const& other : targets)
            {
                if (other == unit->GetObjectGuid())
                    continue;
                Unit* near = ai->GetUnit(other);
                if (!near || !near->IsAlive() || near->IsInCombat() || near->GetTypeId() != TYPEID_UNIT || ((Creature*)near)->IsCritter() ||
                    !near->CanAttackOnSight(bot))
                    continue;
                float d = near->GetDistance(unit);
                if (d < (careful ? 15.0f : 12.0f) || (d < 20.0f && ++around >= 2))
                {
                    pack = true;
                    break;
                }
            }
            if (pack)
                continue;
        }

        if (abs(bot->GetPositionZ() - unit->GetPositionZ()) > sPlayerbotAIConfig.spellDistance)
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (to far above/below).");
            continue;
        }

        if (!bot->InBattleGround() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit))) //Do not grind mobs far away from master.
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (out of free range).");
            continue;
        }

        if (!bot->InBattleGround() && master &&
            (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
             ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
            sServerFacade.GetDistance2d(master, unit) > sPlayerbotAIConfig.proximityDistance)
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (far from master).");
            continue;
        }

        if (!bot->InBattleGround() && (int)unit->GetLevel() - (int)bot->GetLevel() > 4 && !unit->GetObjectGuid().IsPlayer())
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (" + std::to_string((int)unit->GetLevel() - (int)bot->GetLevel()) + " levels above bot).");
            continue;
        }

        Creature* creature = dynamic_cast<Creature*>(unit);
        if (creature && creature->GetCreatureInfo() && creature->GetCreatureInfo()->Rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite") &&
            !AI_VALUE2(bool, "trigger active", "in vehicle"))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (can not fight elites currently).");
            continue;
        }

        if (!AttackersValue::IsValid(unit, bot, nullptr, false, false))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (is pet or evading/unkillable).");
            continue;
        }

        if (!PossibleAttackTargetsValue::IsPossibleTarget(unit, bot, sPlayerbotAIConfig.sightDistance, false))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (tapped, cced or out of range).");
            continue;
        }

        if (creature && creature->IsCritter() && urand(0, 10))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (ignore critters).");
            continue;
        }

        float newdistance = sServerFacade.GetDistance2d(bot, unit);

        uint32 entry = unit->GetEntry();
        bool needForQuest = false;
        if (entry)
        {
            auto cacheIt = needForQuestCache.find(entry);
            if (cacheIt != needForQuestCache.end())
            {
                needForQuest = cacheIt->second;
            }
            else
            {
                needForQuest = AI_VALUE2(bool, "need for quest", std::to_string(entry));
                needForQuestCache[entry] = needForQuest;
            }
        }

        if (entry && !needForQuest)
        {
            if (urand(0, 100) < 99 && travelTargetWorking && !isGrindTravelDest)
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (not needed for active quest).");

                continue;
            }
            // policy (S): "ignore grey mobs while travelling" (on by default for your characters,
            // even when broke); off lets the usual rule decide
            else if (creature && !MaNGOS::XP::Gain(bot, creature) &&
                ((SilvermoonIsOwn(bot) && !(SilvermoonTravelRules(bot) & 2) && AI_VALUE(bool, "travel target traveling")) ||
                 urand(0, 50) || !SilvermoonGreyAllowed(bot)))
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    if ((AI_VALUE(bool, "travel target traveling") && typeid(AI_VALUE(TravelTarget*, "travel target")->GetDestination()) != typeid(GrindTravelDestination)))
                        ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " ignored (not xp and not needed for quest).");

                continue;
            }
            else if (urand(0, 100) < 75)
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    if (AI_VALUE(bool, "travel target traveling") && typeid(AI_VALUE(TravelTarget*, "travel target")->GetDestination()) != typeid(GrindTravelDestination))
                        ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " increased distance (not needed for quest).");

                newdistance += 20;
            }
        }

        if (needForQuest)
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " reduced distance (needed for active quest objective).");

            newdistance *= 0.5f;
        }

        if (!bot->InBattleGround() && GetTargetingPlayerCount(unit) > assistCount)
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + " increased distance (" + std::to_string(GetTargetingPlayerCount(unit)) + " bots already targeting).");

            newdistance =+ GetTargetingPlayerCount(unit) * 5;
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = sObjectMgr.GetPlayer(itr->guid);
                if (!member || !sServerFacade.IsAlive(member))
                    continue;

                float memberDistance = sServerFacade.GetDistance2d(member, unit);
                if (needForQuest)
                    memberDistance *= 0.5f;

                if (!result || memberDistance < distance)
                {
                    distance = memberDistance;
                    result = unit;
                }
            }
        }
        else
        {
            if (!result || (newdistance < distance && urand(0, abs(distance - newdistance)) > sPlayerbotAIConfig.sightDistance * 0.1))
            {
                distance = newdistance;
                result = unit;
            }
        }
    }

    if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
    {
        if(result)
        {
            ai->TellPlayer(GetMaster(), chat->formatWorldobject(result) + " selected.");
        }
        else
        {
            ai->TellPlayer(GetMaster(), "No grind target found.");
        }
    }

    return result;
}

int GrindTargetValue::GetTargetingPlayerCount( Unit* unit )
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    int count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player *member = sObjectMgr.GetPlayer(itr->guid);
        if( !member || !sServerFacade.IsAlive(member) || member == bot)
            continue;

        PlayerbotAI* ai = member->GetPlayerbotAI();
        if ((ai && ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get() == unit->GetObjectGuid()) ||
            (!ai && member->GetSelectionGuid() == unit->GetObjectGuid()))
            count++;
    }

    return count;
}