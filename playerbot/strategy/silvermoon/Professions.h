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

    // "Go to" (S): send your character to the nearest friendly inn(keeper), flight master, auction
    // house, bank or vendor on its continent - ahead of everything else until it gets there.
    // Command: ".bot go inn|flight|ah|bank|vendor|stop"
    bool SilvermoonGoCommand(Player* bot, std::string const& param, std::string& reply);
    bool SilvermoonGoGoal(Player* bot, SilvermoonScoredGoal& goal);

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
