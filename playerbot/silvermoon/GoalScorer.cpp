#include "GoalScorer.h"
#include <algorithm>

namespace silvermoon
{
    // ---- tuning constants -----------------------------------------------------------------------
    static const float WALK_YARDS_PER_SECOND = 7.0f;
    static const float MOUNT_YARDS_PER_SECOND = 14.0f;

    static const float TURN_IN_MINUTES = 1.0f;
    static const float OBJECTIVE_MINUTES = 8.0f;
    static const float GIVER_MINUTES = 2.0f;
    static const float GRIND_MINUTES = 10.0f;
    static const float CHORE_MINUTES = 2.0f;
    static const float EXPLORE_MINUTES = 5.0f;

    static const float GIVER_XP_SHARE = 0.5f;       // a quest not yet picked up is worth half
    static const float GRIND_KILLS = 15.0f;
    static const float CHORE_URGENT_MINUTES = 20.0f; // vendor/repair worth 20 minutes of XP when urgent
    static const float CHORE_NORMAL_MINUTES = 3.0f;
    static const float TRAINER_MINUTES = 30.0f;      // a new spell is worth 30 minutes of XP
    static const float EXPLORE_VALUE_MINUTES = 2.0f;

    static const float DEFAULT_XP_PER_MINUTE_PER_LEVEL = 20.0f; // when the bot's own rate is unknown

    static const float RISK_HIGH_LEVEL = 0.5f;      // creatures 3+ levels above the bot
    static const float RISK_ELITE = 0.25f;
    static const float RISK_DIED_RECENTLY = 0.7f;   // the bot died twice or more recently
    static const int32_t HIGH_LEVEL_GAP = 3;
    static const uint32_t DEATHS_FOR_CAUTION = 2;

    static const float CLOSE_BY_MINUTES = 1.0f;     // "close by" when travel is under a minute
    // ---------------------------------------------------------------------------------------------

    float GoalScorer::XpPerMinute(BotSnapshot const& bot)
    {
        if (bot.xpPerMinute > 0.0f)
            return bot.xpPerMinute;
        return bot.level * DEFAULT_XP_PER_MINUTE_PER_LEVEL;
    }

    char const* GoalScorer::KindName(GoalKind kind)
    {
        switch (kind)
        {
        case GoalKind::QuestTurnIn: return "quest turn-in";
        case GoalKind::QuestObjective: return "quest objective";
        case GoalKind::QuestGiver: return "quest giver";
        case GoalKind::Grind: return "grind";
        case GoalKind::Vendor: return "vendor";
        case GoalKind::Repair: return "repair";
        case GoalKind::Trainer: return "trainer";
        case GoalKind::Explore: return "explore";
        default: return "other";
        }
    }

    GoalScore GoalScorer::Score(BotSnapshot const& bot, GoalCandidate const& c, float variety)
    {
        GoalScore s;
        float travel = c.distance / (bot.mounted ? MOUNT_YARDS_PER_SECOND : WALK_YARDS_PER_SECOND) / 60.0f;
        float xpm = XpPerMinute(bot);

        switch (c.kind)
        {
        case GoalKind::QuestTurnIn:
            s.value = c.questXp;
            s.minutes = travel + TURN_IN_MINUTES;
            s.reason = "quest xp";
            break;
        case GoalKind::QuestObjective:
            s.value = c.questXp / float(std::max<uint32_t>(1, c.objectivesLeft));
            s.minutes = travel + OBJECTIVE_MINUTES;
            s.reason = "quest xp";
            break;
        case GoalKind::QuestGiver:
            s.value = c.questXp * GIVER_XP_SHARE;
            s.minutes = travel + GIVER_MINUTES;
            s.reason = "new quest";
            break;
        case GoalKind::Grind:
            s.value = c.killXp * GRIND_KILLS;
            s.minutes = travel + GRIND_MINUTES;
            s.reason = "grind fallback";
            // never pick an elite camp or mobs 3+ levels up as a grind spot (a level 16 chose a
            // level 27 rare elite: the elite XP bonus outweighed the risk factors)
            if (c.elite || (c.mobLevel > 0 && c.mobLevel >= int32_t(bot.level) + HIGH_LEVEL_GAP))
                s.value = 0.0f;
            break;
        case GoalKind::Vendor:
        {
            // worth more the fuller the bags: 3 minutes of XP at 65% full, rising to 20 at 90%,
            // so a bot stops at a vendor on its way instead of waiting until it is nearly full
            float fill = std::min(1.0f, std::max(0.0f, (float(bot.bagFill) - 65.0f) / 25.0f));
            float minutes = bot.urgentVendor ? CHORE_URGENT_MINUTES : CHORE_NORMAL_MINUTES + fill * (CHORE_URGENT_MINUTES - CHORE_NORMAL_MINUTES);
            s.value = xpm * minutes;
            s.minutes = travel + CHORE_MINUTES;
            s.reason = bot.urgentVendor ? "bags full" : "sell loot";
            break;
        }
        case GoalKind::Repair:
            s.value = xpm * (bot.urgentRepair ? CHORE_URGENT_MINUTES : CHORE_NORMAL_MINUTES);
            s.minutes = travel + CHORE_MINUTES;
            s.reason = bot.urgentRepair ? "needs repair" : "repair";
            break;
        case GoalKind::Trainer:
            s.value = xpm * TRAINER_MINUTES;
            s.minutes = travel + CHORE_MINUTES;
            s.reason = "new spell";
            break;
        case GoalKind::Explore:
            s.value = xpm * EXPLORE_VALUE_MINUTES;
            s.minutes = travel + EXPLORE_MINUTES;
            s.reason = "explore";
            break;
        default:
            s.value = 0.0f;
            s.minutes = travel + 1.0f;
            s.reason = "other";
            break;
        }

        float risk = 1.0f;
        if (c.mobLevel > 0 && c.mobLevel >= int32_t(bot.level) + HIGH_LEVEL_GAP)
        {
            risk *= RISK_HIGH_LEVEL;
            s.reason = "risky: high level";
        }
        if (c.elite)
        {
            risk *= RISK_ELITE;
            s.reason = "risky: elite";
        }
        if (bot.deathCount >= DEATHS_FOR_CAUTION)
            risk *= RISK_DIED_RECENTLY;

        s.score = (s.minutes > 0.0f ? s.value / s.minutes : 0.0f) * risk * variety;

        // quest goals that are right here read better as "close by"
        if (travel < CLOSE_BY_MINUTES && risk == 1.0f && (c.kind == GoalKind::QuestObjective || c.kind == GoalKind::QuestTurnIn || c.kind == GoalKind::QuestGiver))
            s.reason = "close by";
        return s;
    }
}
