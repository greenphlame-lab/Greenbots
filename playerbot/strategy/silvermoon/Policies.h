#pragma once
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/actions/GenericActions.h"
#include <map>
#include <set>
#include <string>

// Silvermoon policies (S, 2026-11-24, 1.6): standing rules for your own characters playing on their
// own (Auto, auto alts), set in the in-game Policies window. Policies OVERRIDE the bot's own rules
// at the point where it decides:
//  - zones:   ticked-off zones are never chosen for quests, grinding, hubs or travel goals
//  - flee:    run when N or more mobs are on it, or health drops below X%
//  - money:   gold that sends it to the auction house (stored; the AH trip comes later) and the
//             share of its money it may spend on an upgrade (weapon, bag, Dual Wield)
//  - weapons: per weapon skill "avoid" (never bought or equipped through the upgrade rules) or
//             "train" (preferred), default = the bot decides
// Stored per character in characters.silvermoon_policy; changed with the chat command "pol" (the
// panel whispers it to the character itself); the panel gets the state as addon message "SMPOL".
class Player;

namespace ai
{
    struct SilvermoonPolicy
    {
        std::set<uint32> blockedZones;
        uint8 fleeMobs = 0;          // 0 = off
        uint8 fleeHealth = 0;        // percent, 0 = off
        uint32 ahGold = 0;           // copper, 0 = off
        uint8 upgradePct = 0;        // 0 = the bot's defaults
        std::map<uint32, uint8> weapons;   // skill -> 1 train, 2 avoid
        bool Any() const { return !blockedZones.empty() || fleeMobs || fleeHealth || ahGold || upgradePct || !weapons.empty(); }
    };

    // your own character playing on its own: Auto (self-bot) or an always-online alt with auto on
    bool SilvermoonIsOwn(Player* bot);
    SilvermoonPolicy SilvermoonPolicyFor(uint32 guid);
    bool SilvermoonZoneBlocked(uint32 guid, uint32 zoneId);
    // most the character may spend on one upgrade (its default share when no policy is set)
    uint32 SilvermoonUpgradeBudget(Player* bot, uint32 defaultPct);
    bool SilvermoonWeaponAvoided(uint32 guid, uint32 skill);
    bool SilvermoonPolicyCommand(Player* bot, std::string const& param, std::string& reply);
    std::string SilvermoonPolicyPanel(Player* bot);

    class SilvermoonPolicyAction : public ChatCommandAction
    {
    public:
        SilvermoonPolicyAction(PlayerbotAI* ai) : ChatCommandAction(ai, "pol") {}
        virtual bool Execute(Event& event) override;
    };
}
