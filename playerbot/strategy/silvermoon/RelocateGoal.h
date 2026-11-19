#pragma once
#include "ChooseGoalAction.h"

// Silvermoon brain Phase 2b: the Relocate goal. A bot whose local options have run thin (or now and
// then, an unvisited hub in its level range) travels to a quest hub by road and flight paths, and
// sets its hearthstone at the hub's innkeeper on arrival. After 3 failed trips in a row the old
// level-zone teleport runs as a fallback.
namespace ai
{
    // the typical XP per minute for a level: median of the tracked rates of bots at that level
    void SilvermoonNoteXpRate(uint32 guid, uint32 level, float xpPerMinute);
    float SilvermoonTypicalXpPerMinute(uint32 level);

    // Relocate candidates for this bot, scored against the best local goal. Returns true and fills
    // `goal` (with a new TemporaryTravelDestination the caller must delete if it is not used) when a
    // hub beats the local goal. Bot-update thread only (uses the route search).
    bool SilvermoonBestRelocate(Player* bot, PlayerbotAI* ai, float bestLocalScore, SilvermoonScoredGoal& goal, uint16& hubId);

    void SilvermoonRelocateStarted(uint32 guid, uint16 hubId, TravelDestination* destination);
    // the level-zone check asks for a Relocate decision instead of teleporting
    void SilvermoonWantRelocate(uint32 guid);
    uint32 SilvermoonRelocateFailures(uint32 guid);
    void SilvermoonResetRelocateFailures(uint32 guid);

    // fast loop (world thread): arrival (home bind at the innkeeper, hub visited) and failed trips.
    // Returns true when the bot arrived this tick.
    bool SilvermoonRelocateTick(Player* bot, PlayerbotAI* ai);
}
