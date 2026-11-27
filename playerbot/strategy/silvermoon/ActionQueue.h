#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Player;

// Action queue (S, 1.8; auto-mode brief section 3): an ordered list of jobs for your own character on
// Auto, ahead of its normal choices. The step at the top drives the character; when it's done (or
// fails, or times out after 2 hours) the next one starts. With "queue only" on, the character does
// nothing but its queue: when the queue is empty, it waits where it is.
//
// Step kinds (first version):
//   quest <quest id>                  pick it up if needed, do its objectives, turn it in; done when rewarded
//   grind <creature entry> [level] [minutes]
//                                     grind that mob (only that mob is pulled); done at the level, or after
//                                     the minutes (30 if neither is given)
//   go <inn|flight|ah|bank|vendor|repair>   a Go to trip; done when it gets there
//   train                             visit the class trainer; done when there's nothing left to learn
//   wait <minutes>                    stand by; done when the time is up
//
// Chat command (the panel whispers it): ".bot pol aq add <kind> <params>", "aq del <n>", "aq up <n>",
// "aq clear", "aq only on|off", "aq find quest|mob <text>". The panel gets the queue as addon message
// "SMAQ\tR;<only>;<n>|<kind>|<label>|<state>^..." (then "SMAQ\t+;..." lines) and search results as
// "SMAQF\t<kind>;R;<id>|<label>^..." (then "SMAQF\t<kind>;+;..." lines).
namespace ai
{
    enum SilvermoonQueueKind : uint8_t { AQ_NONE = 0, AQ_QUEST = 1, AQ_GRIND = 2, AQ_GO = 3, AQ_TRAIN = 4, AQ_WAIT = 5 };

    bool SilvermoonQueueCommand(Player* bot, std::string const& param, std::string& reply, std::vector<std::string>& messages);
    // the panel's messages: "SMAQ	R;<only>;<step>^..." then "SMAQ	+;<step>^..." (short lines)
    std::vector<std::string> SilvermoonQueueText(Player* bot);

    // the brain's hook, called from "choose goal" after the Go to step. Checks the top step (done,
    // failed, timed out), runs Go and Wait steps itself, and for quest, grind and train steps sets the
    // filter the scored goals must pass. Returns 0: no queue (normal play); 1: handled, choose nothing
    // else now; 2: score normally, then keep only goals matching kind/id (none matching: wait if
    // "queue only", else play normally).
    int SilvermoonQueueStep(Player* bot, uint8_t& kind, uint32_t& id);
    bool SilvermoonQueueOnly(Player* bot);
    // a queue step is driving the character (the 5-minute re-check leaves its goal alone)
    bool SilvermoonQueueActive(Player* bot);
    // the creature a grind step is about (only that one is pulled), 0 when none
    uint32_t SilvermoonQueueGrindEntry(Player* bot);
    // the quest / creature the top step needs fetched from afar (for the goal search), 0 when none
    uint32_t SilvermoonQueueFetchQuest(Player* bot);
}
