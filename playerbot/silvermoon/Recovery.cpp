#include "playerbot/playerbot.h"
#include "Recovery.h"
#include "playerbot/TravelNode.h"
#include "playerbot/strategy/values/BudgetValues.h"
#include "playerbot/strategy/silvermoon/RelocateGoal.h"
#include "Entities/Player.h"
#include "Entities/Bag.h"
#include "Maps/GridMap.h"
#include "playerbot/TravelMgr.h"
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
