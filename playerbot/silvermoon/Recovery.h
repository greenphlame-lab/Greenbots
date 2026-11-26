#pragma once
#include <cstdint>

class Player;

// Silvermoon 1.6.20 (S, Nov 26): getting struggling bots back on their feet. Cavol (level 25, Ashenvale)
// died 152 times in a day with every item at 0 durability and 2 silver, while carrying 1g 23s of
// vendor value (gems, a Gold Bar, wool) that the AH rules kept him from selling.
namespace ai
{
    // what a full repair of the bot's equipped gear costs (the stock budget keeps this back first)
    uint32_t SilvermoonRepairReserve(Player* bot);
    // money above that reserve: what upgrades, bags, Dual Wield, professions and training may spend
    uint32_t SilvermoonSpendable(Player* bot);
    // vendor value of everything in the bags but quest items, tools and the Hearthstone
    uint32_t SilvermoonBagSellValue(Player* bot);

    // cash-out (idea 1): broken gear (weapon, or 3+ items at 0) and less money than a full repair.
    // The bot then sells everything below rare that it would otherwise auction, craft with or bank,
    // and goes to a vendor whatever its bag fill. Bot-thread safe (a cached flag).
    bool SilvermoonCashOut(Player* bot);

    // struggling (idea 2): 3+ deaths in the last hour and losing money once the durability those
    // deaths cost is counted (about 10% of a full repair each). It lasts at least 2 hours.
    bool SilvermoonStruggling(uint32_t guid);
    // the death-loop rescue (5 deaths in an hour) counts as struggling straight away
    void SilvermoonMarkStruggling(uint32_t guid);
    // levels taken off when fitting hubs and zones to a struggling bot (0 otherwise)
    int32_t SilvermoonStruggleLevelDrop(uint32_t guid);

    // fast loop (world thread, every 10 s per random bot): updates both flags; on the step into
    // "struggling" the bot hearths out if home is safe and elsewhere, or asks for a relocation
    void SilvermoonRecoveryTick(Player* bot);

    // food and water (S, 1.6.21): what the bot carries and wants. Mana users want 2 stacks of water and
    // 1 of food, everyone else 2 stacks of food (a stack counted as 20).
    struct SilvermoonSupplyNeed { uint32_t food = 0, water = 0, foodWanted = 0, waterWanted = 0; bool low = false, veryLow = false; };
    SilvermoonSupplyNeed SilvermoonSupplies(Player* bot);
    // what a shopping trip may spend: 20% of its money, and never the repair fund
    uint32_t SilvermoonSupplyBudget(Player* bot);
    // buys food and water from a friendly vendor within 30 yards that sells them (paid at the vendor
    // price, level-appropriate items, up to what the bot wants and the budget allows); true if it bought
    bool SilvermoonBuySupplies(Player* bot);

    // a zone that is safe to send a struggling bot to (under 40 deaths to players in the last
    // hour, and not the zone it is avoiding)
    bool SilvermoonZoneSafeFor(uint32_t guid, uint32_t zoneId);
}
