#pragma once
#include <string>
#include <cstdint>

// Silvermoon brain (Phase 1): pure scoring of candidate destinations. No game calls in here, so it
// can be unit-tested with fixed inputs. Score = value (XP) / minutes, times risk and a little
// variety. Constants are at the top of GoalScorer.cpp.
namespace silvermoon
{
    enum class GoalKind
    {
        QuestTurnIn,
        QuestObjective,
        QuestGiver,
        Grind,
        Vendor,
        Repair,
        Trainer,
        Explore,
        Mail,
        Relocate,
        Gather,
        Other
    };

    struct BotSnapshot
    {
        uint32_t level = 1;
        float xpPerMinute = 0.0f;     // own XP gain over the last ~30 min; 0 = unknown
        bool mounted = false;
        uint32_t deathCount = 0;       // "death count" from PlayerTravelInfo
        bool urgentRepair = false;     // "should repair"
        bool urgentVendor = false;     // bags over 90%
        uint32_t bagFill = 0;          // bags used, percent
        bool playerMail = false;       // a real player sent it something by mail
        bool broke = false;            // gear needs repair and it can't pay: earn money first
        bool brokenGear = false;       // weapon at 0 durability, or 3+ equipped items at 0
        uint32_t recentDeaths = 0;     // deaths in the last 30 minutes
        int32_t greyLevel = 0;         // mobs at or below this level give no XP
        bool repairCooldown = false;   // a repair visit in the last 30 min left gear damaged
        bool destitute = false;        // weapon broken and no money to fix it
        bool resSick = false;          // Resurrection Sickness
        bool cashOut = false;          // 1.6.20: broken gear, can't pay a full repair: sell the bags
        uint32_t sellValue = 0;        // 1.6.20: vendor value of the bags (copper)
        bool struggling = false;       // 1.6.20: dying and losing money (Recovery.h)
        bool canGather = false;        // 1.6.20: has Mining or Herbalism
    };

    struct GoalCandidate
    {
        GoalKind kind = GoalKind::Other;
        float distance = 0.0f;         // yards, straight line
        float questXp = 0.0f;          // quest XP for this bot (already rate-adjusted)
        uint32_t objectivesLeft = 1;   // for quest objectives
        float killXp = 0.0f;           // XP per kill for grind spots (rate-adjusted)
        int32_t mobLevel = 0;          // creatures at the destination (0 = none / unknown)
        float chainXp = 0.0f;          // Phase 2a: XP of follow-up quests (quest index)
        bool avoid = false;            // Phase 2a: escort, timed or group quest (quest index)
        float crowdFactor = 1.0f;      // Phase 2a: expected XP is divided by this (bots already there)
        bool elite = false;
    };

    struct GoalScore
    {
        float score = 0.0f;
        float value = 0.0f;            // XP
        float minutes = 0.0f;
        std::string reason;            // the biggest term, e.g. "quest xp", "close by"
    };

    class GoalScorer
    {
    public:
        // variety: a factor from 0.9 to 1.1 chosen by the caller (kept out of here for testing)
        static GoalScore Score(BotSnapshot const& bot, GoalCandidate const& candidate, float variety);
        static float XpPerMinute(BotSnapshot const& bot);
        static char const* KindName(GoalKind kind);
    };
}
