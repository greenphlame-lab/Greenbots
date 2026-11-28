#pragma once
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/actions/GenericActions.h"
#include "ChooseGoalAction.h"
#include <string>

// Silvermoon Professions tab (S, 2026-11-24): your own characters (Auto, auto alts) get profession
// orders from the in-game panel.
//  - Learn: from level 5, put learning a profession at the top of the character's priorities. It
//    walks to the nearest friendly trainer for it and learns it if it can pay; if it can't, the order
//    is dropped. Several can be picked; they're trained one after another.
//  - Gather: once a gathering profession is known (Mining, Herbalism, Skinning, Fishing), make
//    gathering it the top priority until switched off.
// Orders arrive as the chat command "prof" (the panel whispers it to the character itself):
//   prof learn <skill id>   prof gather <skill id>   prof stop <skill id>   prof stop
// The panel gets the state as addon message "SMPROF".
class Player;

namespace ai
{
    bool SilvermoonProfCommand(Player* bot, std::string const& param, std::string& reply);
    // a profession goal ahead of everything else (learn order at the head of the list), or false
    bool SilvermoonProfessionGoal(Player* bot, PlayerbotAI* ai, SilvermoonScoredGoal& goal);
    // gather order for this purpose's skill: the gather goal is worth this factor (1 = none)
    float SilvermoonGatherOrderFactor(uint32 guid, uint32 skill);
    // Gather order patrol route (S: "point to point across the entire zone, one big loop"): every
    // herb (or ore) spawn point in the bot's current zone that its skill can gather, ordered into one
    // loop by nearest neighbour from where the order started. Returns the next point and moves on;
    // false when there is no gather order or no point. index/total are for the log line.
    bool SilvermoonGatherRouteNext(Player* bot, WorldPosition& next, uint32& index, uint32& total);
    // "SMPROF\t<skill>:<value>:<max>:<state>;..." for the panel; state L = learn queued, G = gathering
    std::string SilvermoonProfPanel(Player* bot);
    // the tool a gathering profession needs (Mining Pick, Skinning Knife, Fishing Pole): bought
    // in a town for the vendor price when the character has the profession but not the tool
    void SilvermoonProfBuyTools(Player* bot);
    // "Level it" (S): for professions you told it to level, craft recipes that still give skill-ups
    // with the materials it has (a few per pass, out of combat); a missing material that a vendor
    // sells is bought from a vendor standing nearby, or the character goes to the nearest vendor
    // that sells it. Command: ".bot prof level <skill>" / ".bot prof unlevel <skill>"
    void SilvermoonLevelPass(Player* bot);
    // Rogue poisons (S, 1.8.1: "no logic for poisons"): every rogue of 20+ gets the Poisons skill (the
    // reward of the level 20 rogue quest, Klaven's Tower / Hinott's Assistance - bots never finish those
    // chains), keeps 10-20 of its best known Instant Poison (else Deadly) made from reagents bought at a
    // vendor in reach, and goes to a vendor selling them when out. Applying them is the stock "poisons"
    // step. Called often; throttled per character.
    void SilvermoonRoguePoisons(Player* bot);
    // Policies "vendor when N bag slots free" (1.8.3, S): a vendor trip once the bags get that full
    void SilvermoonBagVendorTrip(Player* bot);

    // "Go to" (S): send your character to the nearest friendly inn(keeper), flight master, auction
    // house, bank or vendor on its continent - ahead of everything else until it gets there.
    // Command: ".bot go inn|flight|ah|bank|vendor|stop"
    bool SilvermoonGoCommand(Player* bot, std::string const& param, std::string& reply);
    bool SilvermoonGoGoal(Player* bot, SilvermoonScoredGoal& goal);
    // Go to queue (S, 1.7): ".bot go +ah" adds a stop after the current one, "clear" empties the queue,
    // "-N" drops the Nth; the panel gets "SMGO	<current>;<queued>^<queued>"
    std::string SilvermoonGoQueueText(Player* bot);
    // on a Go to trip (yours, or the brain's: rank training, bags, vendor materials)
    bool SilvermoonGoActive(Player* bot);
    // a Go to trip you ordered (not one the brain planned: rank training, bags, materials)
    bool SilvermoonGoIsYours(Player* bot);
    // a brain-planned Go trip that got stuck: dropped, and no brain trip for this bot for an hour
    void SilvermoonGoGiveUp(Player* bot);
    // "Buy bigger bags" (S, 1.6.21): a go order to the nearest vendor that sells this bag
    bool SilvermoonGoBagVendor(Player* bot, uint32 itemId);
    // profession ranks (S, 1.6.22): a go order to the nearest trainer of the rank this character can
    // train now (skill, level and money allow); and the training itself with that trainer in reach
    bool SilvermoonGoTrainRank(Player* bot);
    bool SilvermoonLearnRankHere(Player* bot);
    // profession catch-up (S, 1.6.21): a gather session for a random bot behind its milestone
    void SilvermoonStartCatchUp(uint32 guid, uint32 skill, uint32 target);
    uint32 SilvermoonCatchUpSkill(uint32 guid);
    // 1.6.22: arrival at the gathering area starts the session's 2 hours (true the first time)
    bool SilvermoonCatchUpArrived(uint32 guid);
    uint32 SilvermoonCatchUpTarget(uint32 guid);
    void SilvermoonEndCatchUp(uint32 guid);
    // the bot's gather route leads to another area it hasn't reached yet (or there is no route yet)
    bool SilvermoonGatherRouteRemote(uint32 guid);
    // the smallest node requirement above its skill around it (0 = none): the tier its area needs
    uint32 SilvermoonHomeNodeReq(Player* bot, uint32 skill, uint32 value);

    // at the trainer the learn order points at: learn it (paying the trainer's fee)
    class ProfessionLearnAction : public Action
    {
    public:
        ProfessionLearnAction(PlayerbotAI* ai) : Action(ai, "silvermoon profession learn") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };

    class SilvermoonProfAction : public ChatCommandAction
    {
    public:
        SilvermoonProfAction(PlayerbotAI* ai) : ChatCommandAction(ai, "prof") {}
        virtual bool Execute(Event& event) override;
    };
}
