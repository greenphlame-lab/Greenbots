#pragma once
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/actions/GenericActions.h"
#include <map>
#include <set>
#include <string>
#include <list>

// Silvermoon policies (S, 2026-11-24, 1.6): standing rules for your own characters playing on their
// own (Auto, auto alts), set in the in-game Policies window. Policies OVERRIDE the bot's own rules
// at the point where it decides:
//  - zones:   off until you tick a zone; then an allow-list - only ticked zones are chosen for
//             quests, grinding, hubs or travel goals (S, 2026-11-24)
//  - rest:    eat until health X%, drink until mana Y% (25-100)
//  - flee:    run when N or more mobs are on it, or health drops below X%
//  - money:   gold worth of auction-house items that sends it on an AH trip, and the
//             share of its money it may spend on an upgrade (weapon, bag, Dual Wield)
//  - weapons: per weapon skill "avoid" (never bought or equipped through the upgrade rules) or
//             "train" (preferred), default = the bot decides
// Stored per character in characters.silvermoon_policy; changed with the chat command "pol" (the
// panel whispers it to the character itself); the panel gets the state as addon message "SMPOL".
class Player;
class Item;

namespace ai
{
    struct SilvermoonPolicy
    {
        std::set<uint32> allowedZones;   // empty = the bot picks zones itself
        uint8 eatTo = 0;             // percent health to eat up to, 0 = default (full)
        uint8 drinkTo = 0;           // percent mana to drink up to, 0 = default (full)
        // travel (S): bit 1 = avoid mobs while travelling (no new pulls on the way);
        // bit 2 = grey mobs allowed while travelling (default: ignored)
        uint8 travel = 0;
        uint8 fleeMobs = 0;          // 0 = off
        uint8 fleeHealth = 0;        // percent, 0 = off
        uint32 ahGold = 0;           // copper, 0 = off
        uint8 upgradePct = 0;        // 0 = the bot's defaults
        std::map<uint32, uint8> weapons;   // skill -> 1 train, 2 avoid
        uint8 bags = 0;              // bit 1: don't buy bags; bit 2: replace small bags with bigger ones
        // item rules (S): item id -> (action, always). action 1 sell, 2 auction, 3 destroy, 4 keep;
        // "once" rules end when none of the item is left
        std::map<uint32, std::pair<uint8, bool>> items;
        bool Any() const { return !allowedZones.empty() || eatTo || drinkTo || travel || fleeMobs || fleeHealth || ahGold || upgradePct || !weapons.empty() || bags || !items.empty(); }
    };

    // your own character playing on its own: Auto (self-bot) or an always-online alt with auto on
    bool SilvermoonIsOwn(Player* bot);
    SilvermoonPolicy SilvermoonPolicyFor(uint32 guid);
    bool SilvermoonZoneBlocked(uint32 guid, uint32 zoneId);
    // AH trip (S): estimated auction value of the items it means to auction (copper), and whether
    // that reached your "AH trip when worth" gold
    uint32 SilvermoonAhCarried(std::list<Item*> const& items);
    bool SilvermoonAhTripDue(Player* bot, std::list<Item*> const& items);
    // eat / drink until X% (0 = default)
    uint8 SilvermoonRestTarget(Player* bot, bool mana);
    // travel rules for your own characters (bits as SilvermoonPolicy::travel), 0 for everyone else
    uint8 SilvermoonTravelRules(Player* bot);
    // most the character may spend on one upgrade (its default share when no policy is set)
    uint32 SilvermoonUpgradeBudget(Player* bot, uint32 defaultPct);
    bool SilvermoonWeaponAvoided(uint32 guid, uint32 skill);
    bool SilvermoonPolicyCommand(Player* bot, std::string const& param, std::string& reply);
    std::string SilvermoonPolicyPanel(Player* bot);
    // weapon master training (S): in a capital, a weapon skill that is ticked (not "avoid"), that
    // the class can use and doesn't know yet, is learned for the weapon masters' 10 silver when it
    // fits the character's upgrade budget. One per call.
    void SilvermoonWeaponTraining(Player* bot);
    // item rules: 0 = none, else 1 sell, 2 auction, 3 destroy, 4 keep
    uint8 SilvermoonItemRule(uint32 guid, uint32 itemId);
    // destroys items ruled "destroy" and ends finished "once" rules (your characters, every 10 s)
    void SilvermoonApplyItemRules(Player* bot);

    class SilvermoonPolicyAction : public ChatCommandAction
    {
    public:
        SilvermoonPolicyAction(PlayerbotAI* ai) : ChatCommandAction(ai, "pol") {}
        virtual bool Execute(Event& event) override;
    };
}
