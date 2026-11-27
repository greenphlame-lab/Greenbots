#include "playerbot/playerbot.h"
#include "Recovery.h"
#include "playerbot/TravelNode.h"
#include "playerbot/strategy/values/BudgetValues.h"
#include "playerbot/strategy/silvermoon/RelocateGoal.h"
#include "playerbot/strategy/silvermoon/Professions.h"
#include "Entities/Player.h"
#include "Entities/Bag.h"
#include "Maps/GridMap.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "Entities/Creature.h"
#include <list>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <unordered_map>

using namespace ai;

namespace
{
    struct RecoveryState
    {
        std::deque<std::pair<time_t, uint32>> money;   // (when, money), one sample a minute, 70 minutes
        std::deque<time_t> deaths;                     // its own count: the death-loop rescue clears the shared one
        bool wasAlive = true;
        bool cashOut = false;
        time_t strugglingUntil = 0;
        time_t lastSample = 0;
    };
    std::mutex recoveryLock;
    std::mutex profCheckLock;                           // gathering milestone checks: next check per bot
    std::unordered_map<uint32, time_t> profCheckAt;
    std::unordered_map<uint32, RecoveryState> recovery;

    bool IsTool(uint32 itemId)
    {
        // Hearthstone, Mining Pick, Skinning Knife, Blacksmith Hammer, Arclight Spanner, Runed Copper Rod
        return itemId == 6948 || itemId == 2901 || itemId == 7005 || itemId == 5956 || itemId == 6219 || itemId == 6218;
    }

    template <typename F> void ForEachBagItem(Player* bot, F f)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                f(item);
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* b = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
                for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                    if (Item* item = b->GetItemByPos(slot))
                        f(item);
    }
}

uint32_t ai::SilvermoonRepairReserve(Player* bot)
{
    uint32 total = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            total += RepairCostValue::RepairCost(item, true);
    return total;
}

uint32_t ai::SilvermoonSpendable(Player* bot)
{
    uint32 money = bot->GetMoney(), reserve = SilvermoonRepairReserve(bot);
    return money > reserve ? money - reserve : 0;
}

uint32_t ai::SilvermoonBagSellValue(Player* bot)
{
    uint32 total = 0;
    ForEachBagItem(bot, [&](Item* item)
    {
        ItemPrototype const* proto = item->GetProto();
        if (!proto || !proto->SellPrice || proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || IsTool(proto->ItemId) ||
            proto->Quality >= ITEM_QUALITY_RARE ||
            (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE))
            return;
        total += proto->SellPrice * std::max<uint32>(1, item->GetCount());
    });
    return total;
}

bool ai::SilvermoonCashOut(Player* bot)
{
    std::lock_guard<std::mutex> guard(recoveryLock);
    auto it = recovery.find(bot->GetGUIDLow());
    return it != recovery.end() && it->second.cashOut;
}

bool ai::SilvermoonStruggling(uint32_t guid)
{
    std::lock_guard<std::mutex> guard(recoveryLock);
    auto it = recovery.find(guid);
    return it != recovery.end() && time(nullptr) < it->second.strugglingUntil;
}

void ai::SilvermoonMarkStruggling(uint32_t guid)
{
    std::lock_guard<std::mutex> guard(recoveryLock);
    recovery[guid].strugglingUntil = time(nullptr) + 2 * HOUR;
}

int32_t ai::SilvermoonStruggleLevelDrop(uint32_t guid)
{
    return SilvermoonStruggling(guid) ? 4 : 0;
}

uint32_t ai::SilvermoonGatherMilestone(uint32_t level)
{
    // level -> gathering skill it should have by then (S: 150 at 25, 200 at 35), straight lines between
    static const uint32 table[][2] = { { 10, 50 }, { 15, 75 }, { 20, 110 }, { 25, 150 }, { 30, 175 }, { 35, 200 },
                                       { 40, 225 }, { 45, 250 }, { 50, 275 }, { 55, 290 }, { 60, 300 } };
    if (level < 10)
        return 0;
    for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); ++i)
        if (level <= table[i][0])
            return table[i - 1][1] + (table[i][1] - table[i - 1][1]) * (level - table[i - 1][0]) / (table[i][0] - table[i - 1][0]);
    return 300;
}

bool ai::SilvermoonZoneSafeFor(uint32_t guid, uint32_t zoneId)
{
    return SilvermoonZonePvpDeaths(zoneId) < 40 && zoneId != SilvermoonAvoidedZone(guid);
}

void ai::SilvermoonRecoveryTick(Player* bot)
{
    uint32 guid = bot->GetGUIDLow();
    time_t now = time(nullptr);
    bool alive = bot->IsAlive();

    // broken gear: weapon at 0 durability, or 3 or more items at 0
    uint32 broken = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && !item->GetUInt32Value(ITEM_FIELD_DURABILITY))
                broken += slot == EQUIPMENT_SLOT_MAINHAND ? 3 : 1;
    uint32 reserve = SilvermoonRepairReserve(bot);
    bool cashOut = broken >= 3 && bot->GetMoney() < reserve;

    // food and water (S, 1.6.21): at most once a minute, when a vendor is in reach and it's running low
    {
        static std::mutex buyLock;
        static std::unordered_map<uint32, time_t> lastBuy;
        bool due = false;
        {
            std::lock_guard<std::mutex> guard(buyLock);
            time_t& last = lastBuy[guid];
            if (now - last >= MINUTE)
            {
                last = now;
                due = true;
            }
        }
        if (due && alive && !cashOut && !bot->GetGroup())
            SilvermoonBuySupplies(bot);
    }

    // gathering milestones (S, 1.6.21: "level 25 at least 150 in a gathering skill, 200 by 35"; 1.6.22:
    // only for bots not levelling well anyway): every 2 minutes, a bot that is struggling, has
    // resurrection sickness, or is broke with nothing to sell, and whose Mining or Herbalism is below the
    // milestone for its level (not capped by its rank), commits to a gather session: it walks to an
    // area whose nodes still raise the skill (up to 45 minutes), then gathers there for 30 minutes,
    // then an hour before the next one. A capped skill gets its rank trained (the rank trip).
    {
        std::mutex& profLock = profCheckLock;
        std::unordered_map<uint32, time_t>& nextProfCheck = profCheckAt;
        bool due = false;
        {
            std::lock_guard<std::mutex> guard(profLock);
            time_t& at = nextProfCheck[guid];
            if (now >= at)
            {
                at = now + 2 * MINUTE;              // often enough to catch a resurrection sickness (up to 10 minutes)
                due = true;
            }
        }
        // ...also a bot cashing out with nothing left to sell (S: the stuck few - Olmoh, Ninmenno,
        // Kelyia - grinding grey mobs with broken gear): gathering is money without fighting
        bool brokeNoLoot = due && cashOut && SilvermoonBagSellValue(bot) < 200;
        // only for a bot that isn't levelling well right now anyway (S, 1.6.22): struggling, or with
        // resurrection sickness - or broke with nothing to sell
        bool idleAnyway = SilvermoonStruggling(guid) || bot->HasAura(15007) || brokeNoLoot;
        if (due && alive && idleAnyway && (!cashOut || brokeNoLoot) && !bot->GetGroup() && !bot->IsInCombat() && !SilvermoonCatchUpSkill(guid))
        {
            uint32 target = brokeNoLoot ? 300 : SilvermoonGatherMilestone(bot->GetLevel());
            uint32 best = 0, bestValue = 0;
            for (uint32 skill : { uint32(SKILL_MINING), uint32(SKILL_HERBALISM) })
                if (bot->HasSkill(skill) && bot->GetSkillValuePure(skill) >= bestValue)
                {
                    best = skill;
                    bestValue = bot->GetSkillValuePure(skill);
                }
            bool capped = best && bestValue >= bot->GetSkillMaxPure(best);
            if (best && target && bestValue < target && (!capped || brokeNoLoot))   // broke: capped or not, the materials sell
            {
                // the session's target (1.6.22): the tier its own area needs (so it can gather at home
                // afterwards), else the milestone; at least 15 points, never past the rank cap
                uint32 home = SilvermoonHomeNodeReq(bot, best, bestValue);
                uint32 goal = (home && home < target) ? home : target;
                goal = std::max(goal, bestValue + 15);
                goal = std::min<uint32>(goal, bot->GetSkillMaxPure(best));
                SilvermoonStartCatchUp(guid, best, goal);
                {
                    std::lock_guard<std::mutex> guard(profLock);
                    nextProfCheck[guid] = now + 2 * HOUR;      // ends at 30 minutes there (or the target), then an hour's rest (below)
                }
                sLog.outBasic("Silvermoon: bot %s (level %u) %s skill %u (%u, milestone %u) - gather session to %u",
                    bot->GetName(), bot->GetLevel(), brokeNoLoot ? "is broke with nothing to sell, gathers with" : "is behind on", best, bestValue, target, goal);
                if (PlayerbotAI* pai = bot->GetPlayerbotAI())
                    if (TravelTarget* t = pai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                        t->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            }
        }
    }

    // a gather session under way (1.6.22): the clock starts when it reaches the area; it ends at the
    // target skill, after which the next check is an hour away
    if (uint32 skill = SilvermoonCatchUpSkill(guid))
    {
        if (!SilvermoonGatherRouteRemote(guid) && SilvermoonCatchUpArrived(guid))
            sLog.outBasic("Silvermoon: bot %s reached its gathering area (skill %u at %u), 30 minutes there", bot->GetName(), skill, bot->GetSkillValuePure(skill));
        uint32 goal = SilvermoonCatchUpTarget(guid);
        if (goal && bot->GetSkillValuePure(skill) >= goal)
        {
            SilvermoonEndCatchUp(guid);
            sLog.outBasic("Silvermoon: bot %s reached skill %u in %u - gather session done", bot->GetName(), goal, skill);
            std::lock_guard<std::mutex> guard(profCheckLock);
            profCheckAt[guid] = now + HOUR;
        }
    }

    bool startStruggling = false;
    uint32 deathsHour = 0;
    int64 moneyDelta = 0;
    {
        std::lock_guard<std::mutex> guard(recoveryLock);
        RecoveryState& st = recovery[guid];
        if (st.wasAlive && !alive)
            st.deaths.push_back(now);
        st.wasAlive = alive;
        while (!st.deaths.empty() && now - st.deaths.front() > HOUR)
            st.deaths.pop_front();
        if (st.cashOut != cashOut)
            sLog.outBasic("Silvermoon: bot %s %s cash-out (%u copper, full repair %u, %u broken)", bot->GetName(),
                cashOut ? "starts" : "ends", bot->GetMoney(), reserve, broken);
        st.cashOut = cashOut;
        if (now - st.lastSample >= MINUTE)
        {
            st.lastSample = now;
            st.money.push_back({ now, bot->GetMoney() });
            while (st.money.size() > 1 && now - st.money.front().first > 70 * MINUTE)
                st.money.pop_front();
        }
        deathsHour = uint32(st.deaths.size());
        // an hour of samples, at least 50 minutes
        if (deathsHour >= 3 && !st.money.empty() && now - st.money.front().first >= 50 * MINUTE)
        {
            moneyDelta = int64(bot->GetMoney()) - int64(st.money.front().second);
            // each death costs about 10% durability on everything: that much repair is owed
            int64 owed = int64(deathsHour) * int64(reserve) / 10;
            if (moneyDelta - owed < 0)
            {
                if (now >= st.strugglingUntil)
                    startStruggling = true;
                st.strugglingUntil = now + 2 * HOUR;
            }
        }
    }

    if (!startStruggling)
        return;
    sLog.outBasic("Silvermoon: bot %s (level %u, zone %u) is struggling: %u deaths in an hour, money %+lld copper - easier zone (4 levels down)",
        bot->GetName(), bot->GetLevel(), bot->GetZoneId(), deathsHour, (long long)moneyDelta);
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai || !alive || bot->IsInCombat() || bot->GetGroup() || bot->InBattleGround() || ai->HasRealPlayerMaster())
    {
        SilvermoonWantRelocate(guid);
        return;
    }
    // idea 3, a safe way out: home first, by hearthstone, when it is somewhere else and safe
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
    uint32 hmap = 0, hzone = 0, harea = 0;
    bot->GetHomebindLocation(hx, hy, hz, hmap);
    sTerrainMgr.GetZoneAndAreaId(hzone, harea, hmap, hx, hy, hz);
    if (hzone != bot->GetZoneId() && SilvermoonZoneSafeFor(guid, hzone) &&
        ai->GetAiObjectContext()->GetValue<bool>("action useful", "hearthstone")->Get() &&
        ai->DoSpecificAction("hearthstone", Event("silvermoon struggling"), true))
    {
        sLog.outBasic("Silvermoon: bot %s hearthed home to zone %u to recover", bot->GetName(), hzone);
        return;
    }
    // otherwise a trip to an easier, quieter hub (relocation drops 4 levels and weighs PvP deaths)
    SilvermoonWantRelocate(guid);
    if (TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
}

// ---------------------------------------------------------------------------------------------
// Food and water (S, 1.6.21): only 174 of 798 online bots at level 10+ carried any food and 110 of
// 527 mana users any water - they bought only when they happened to be at a vendor, and the stock
// budget put consumables behind the gear money (level^3). Bots now restock with their own money.

namespace
{
    const uint32 STACK = 20;

    bool IsFood(ItemPrototype const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONSUMABLE && proto->Spells[0].SpellCategory == 11;
    }
    bool IsWater(ItemPrototype const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONSUMABLE && proto->Spells[0].SpellCategory == 59;
    }
    bool UsesMana(Player* bot)
    {
        uint8 cls = bot->getClass();
        return cls != CLASS_WARRIOR && cls != CLASS_ROGUE;
    }
}

SilvermoonSupplyNeed ai::SilvermoonSupplies(Player* bot)
{
    SilvermoonSupplyNeed n;
    ForEachBagItem(bot, [&](Item* item)
    {
        ItemPrototype const* proto = item->GetProto();
        if (IsFood(proto))
            n.food += item->GetCount();
        else if (IsWater(proto))
            n.water += item->GetCount();
    });
    bool mana = UsesMana(bot);
    // 1.6.23 (S: gold in circulation fell from 907g to 413g in 6 hours, ~80g an hour on food and
    // water): a stack of water and half a stack of food for mana users, a stack of food for others
    n.waterWanted = mana ? STACK : 0;
    n.foodWanted = mana ? STACK / 2 : STACK;
    // shopping when below a quarter of what it wants; a trip when nearly out (under 5)
    n.low = n.food * 4 < n.foodWanted || (mana && n.water * 4 < n.waterWanted);
    n.veryLow = n.food < 5 || (mana && n.water < 5);
    return n;
}

namespace
{
    std::mutex supplyLock;
    std::unordered_map<uint32, std::pair<time_t, uint32>> supplySpent;   // guid -> (hour started, spent in it)
}

uint32_t ai::SilvermoonSupplyBudget(Player* bot)
{
    // 1.6.23: at most 10% of its money per hour in all (it was 20% per purchase, and the buy step runs
    // every minute), and never the repair fund
    uint32 spent = 0;
    {
        std::lock_guard<std::mutex> guard(supplyLock);
        auto& s = supplySpent[bot->GetGUIDLow()];
        if (time(nullptr) - s.first >= HOUR)
            s = { time(nullptr), 0 };
        spent = s.second;
    }
    uint32 hourly = (bot->GetMoney() + spent) / 10;
    uint32 left = hourly > spent ? hourly - spent : 0;
    return std::min(left, SilvermoonSpendable(bot));
}

bool ai::SilvermoonBuySupplies(Player* bot)
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsTaxiFlying())
        return false;
    SilvermoonSupplyNeed need = SilvermoonSupplies(bot);
    if (!need.low)
        return false;
    uint32 budget = SilvermoonSupplyBudget(bot);
    if (!budget)
        return false;

    std::list<Unit*> near;
    MaNGOS::AnyUnitInObjectRangeCheck check(bot, 30.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
    Cell::VisitAllObjects(bot, searcher, 30.0f);
    for (Unit* u : near)
    {
        if (u->GetTypeId() != TYPEID_UNIT || !u->IsAlive() || !u->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR) || bot->CanAttack(u))
            continue;
        Creature* vendor = (Creature*)u;
        // the best food and the best water it can use from this vendor (highest required level,
        // then cheapest); limited-stock items are left alone
        ItemPrototype const* bestFood = nullptr;
        ItemPrototype const* bestWater = nullptr;
        for (VendorItemData const* list : { vendor->GetVendorItems(), vendor->GetVendorTemplateItems() })
        {
            if (!list)
                continue;
            for (VendorItem const* vi : list->m_items)
            {
                if (!vi || vi->maxcount)
                    continue;
                ItemPrototype const* proto = sObjectMgr.GetItemPrototype(vi->item);
                if (!proto || !proto->BuyPrice || proto->RequiredLevel > bot->GetLevel())
                    continue;
                if (!IsFood(proto) && !IsWater(proto))
                    continue;
                ItemPrototype const*& best = IsFood(proto) ? bestFood : bestWater;
                if (!best || proto->RequiredLevel > best->RequiredLevel ||
                    (proto->RequiredLevel == best->RequiredLevel && proto->BuyPrice < best->BuyPrice))
                    best = proto;
            }
        }
        if (!bestFood && !bestWater)
            continue;
        float discount = bot->GetReputationPriceDiscount(vendor);
        uint32 spent = 0;
        std::string bought;
        // mana users: water first
        std::vector<std::pair<ItemPrototype const*, uint32>> order;
        if (UsesMana(bot) && bestWater)
            order.push_back({ bestWater, need.waterWanted > need.water ? need.waterWanted - need.water : 0 });
        if (bestFood)
            order.push_back({ bestFood, need.foodWanted > need.food ? need.foodWanted - need.food : 0 });
        for (auto const& o : order)
        {
            ItemPrototype const* proto = o.first;
            uint32 lot = std::max<uint32>(1, proto->BuyCount);
            uint32 price = std::max<uint32>(1, uint32(proto->BuyPrice * discount));
            uint32 lots = (o.second + lot - 1) / lot;
            lots = std::min(lots, (budget - spent) / price);
            if (!lots)
                continue;
            uint32 count = lots * lot;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, proto->ItemId, count) != EQUIP_ERR_OK)
                continue;
            bot->ModifyMoney(-int32(lots * price));
            spent += lots * price;
            bot->StoreNewItem(dest, proto->ItemId, true);
            bought += (bought.empty() ? "" : ", ") + std::to_string(count) + " x " + proto->Name1;
        }
        if (!spent)
            return false;
        silvermoon::Ledger(silvermoon::LG_BUY) += spent;
        {
            std::lock_guard<std::mutex> guard(supplyLock);
            supplySpent[bot->GetGUIDLow()].second += spent;
        }
        sLog.outBasic("Silvermoon: bot %s (level %u) bought %s from %s for %u copper (had %u food, %u water)", bot->GetName(), bot->GetLevel(),
            bought.c_str(), vendor->GetName(), spent, need.food, need.water);
        return true;
    }
    return false;
}
