#pragma once

#include "playerbot/strategy/Value.h"
#include "playerbot/strategy/NamedObjectContext.h"
#include "playerbot/playerbot.h"

namespace ai
{
    enum class NeedMoneyFor : uint32
    {
        none = 0,
        repair = 1,
        ammo = 2,
        spells = 3,
        travel = 4,
        consumables = 5,
        gear = 6,
        guild = 7,
        skilltraining = 8,
        tradeskill = 9,
        ah = 10,
        mount = 11,
        anything = 12
    };

    class RepairCostValue : public Uint32CalculatedValue
    {
    public:
        RepairCostValue(PlayerbotAI* ai, std::string name = "repair cost", int checkInterval = 60) : Uint32CalculatedValue(ai, name, checkInterval) {}

        static uint32 RepairCost(const Item* item, bool fullCost = false);

        virtual uint32 Calculate() override;
    };

    class MaxGearRepairCostValue : public RepairCostValue
    {
    public:
        MaxGearRepairCostValue(PlayerbotAI* ai) : RepairCostValue(ai,"max repair cost",checkInterval) {}

        virtual uint32 Calculate() override;
    };

    class MinRepairCostValue : public RepairCostValue
    {
    public:
        MinRepairCostValue(PlayerbotAI* ai) : RepairCostValue(ai, "min repair cost", checkInterval) {}

        virtual uint32 Calculate() override;
    };

    class MoneyNeededForValue : public Uint32CalculatedValue, public Qualified
    {
    public:
        MoneyNeededForValue(PlayerbotAI* ai) : Uint32CalculatedValue(ai, "money needed for", 60), Qualified() {}
        virtual uint32 Calculate() override;
    };

    class TotalMoneyNeededForValue : public Uint32CalculatedValue, public Qualified
    {
    public:
        TotalMoneyNeededForValue(PlayerbotAI* ai) : Uint32CalculatedValue(ai, "total money needed for", 60), Qualified() {}
        virtual uint32 Calculate() override;
    private:
        std::vector<NeedMoneyFor> saveMoneyFor = { NeedMoneyFor::repair, NeedMoneyFor::consumables, NeedMoneyFor::spells, NeedMoneyFor::ammo, NeedMoneyFor::ah, NeedMoneyFor::guild, NeedMoneyFor::travel, NeedMoneyFor::gear }; // Silvermoon patch: food and water (S, 1.6.21), then spells, right after repairs (were behind the auction house and guild); gear money is kept back before buying consumables or trade goods
    };

    class FreeMoneyForValue : public Uint32CalculatedValue, public Qualified
    {
    public:
        FreeMoneyForValue(PlayerbotAI* ai) : Uint32CalculatedValue(ai, "free money for"), Qualified() {}
        virtual uint32 Calculate() override;
    };

    class HasAllMoneyForValue : public BoolCalculatedValue, public Qualified
    {
    public:
        HasAllMoneyForValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "has all money for"), Qualified() {}
        virtual bool Calculate() override;
    };
    
    class ShouldGetMoneyValue : public BoolCalculatedValue
    {
    public:
        ShouldGetMoneyValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "should get money",2) {}
        virtual bool Calculate() override { return bot->GetLevel() > 6 && !AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::ammo); };
    };
}

