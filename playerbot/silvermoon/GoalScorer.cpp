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
    static const float CHAIN_XP_SHARE = 0.5f;      // follow-up quests count half (Phase 2a)

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
        case GoalKind::Mail: return "mail";
        case GoalKind::Relocate: return "relocate";
        case GoalKind::Gather: return "gather";
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
            s.value = c.questXp + c.chainXp * CHAIN_XP_SHARE;
            // a finished quest is worth handing in even when its XP is small (low or grey class
            // quests like Consecrated Letter): rewards, follow-ups, a free log slot - at least 6
            // minutes of the bot's XP rate (S: Mommy ground kobolds with a turn-in next door)
            s.value = std::max(s.value, xpm * 6.0f);
            s.minutes = travel + TURN_IN_MINUTES;
            s.reason = "quest xp";
            break;
        case GoalKind::QuestObjective:
            s.value = c.questXp / float(std::max<uint32_t>(1, c.objectivesLeft));
            s.minutes = travel + OBJECTIVE_MINUTES;
            s.reason = "quest xp";
            break;
        case GoalKind::QuestGiver:
            s.value = (c.questXp + c.chainXp * CHAIN_XP_SHARE) * GIVER_XP_SHARE;
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
            // destitute (S: can't even fix its weapon - find some way to make money): grey mobs give
            // no XP but drop coin and vendor loot, and are safe with a broken weapon
            // (close by only - 300 yards: Cavol kept choosing harpies and Venture Co. across the map and
            // gave up on the walk after 3 minutes, S Nov 26)
            else if (bot.destitute && c.mobLevel > 0 && c.mobLevel <= bot.greyLevel && int32_t(bot.level) - c.mobLevel <= 12 && c.distance <= 300.0f)
            {
                s.value = xpm * 4.0f;
                s.reason = "destitute: safe money";
            }
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
            // cash-out (S, 1.6.20): broken gear and no money for a full repair, with loot worth
            // selling - straight to a vendor, whatever the bag fill (Cavol carried 1g 23s of it)
            if (bot.cashOut && bot.sellValue >= std::max<uint32_t>(200, bot.level * bot.level * 2))
            {
                s.value = xpm * CHORE_URGENT_MINUTES * 2.0f;
                s.reason = "broke: sell loot for repairs";
            }
            break;
        }
        case GoalKind::Repair:
            s.value = xpm * (bot.urgentRepair ? CHORE_URGENT_MINUTES : CHORE_NORMAL_MINUTES);
            s.minutes = travel + CHORE_MINUTES;
            s.reason = bot.urgentRepair ? "needs repair" : "repair";
            // already tried with the money it has (Grashnak, 1 silver, walked between five
            // repair vendors in 15 minutes)
            if (bot.repairCooldown)
            {
                s.value = 0.0f;
                s.reason = "repair: tried recently";
            }
            break;
        case GoalKind::Gather:
            // mining or herbalism: worth a few minutes of XP normally, much more when broke
            // (sellable materials, no fighting)
            // x2 when broke (was x6: S, Nov 25 - 112 bots stuck 18h+ at a level spent 14% of their
            // choices gathering to sell, which pays little next to quest rewards)
            s.value = xpm * (bot.broke || bot.brokenGear ? 2.0f : 1.0f);
            s.minutes = travel + CHORE_MINUTES;
            s.reason = bot.broke || bot.brokenGear ? "broke: gather to sell" : "gather";
            // can't fix its weapon, or cashing out (S, 1.6.20): gathering is money with no fighting,
            // worth as much as the safe grey-mob grind (the materials go to the vendor while cashing out)
            if ((bot.destitute || bot.cashOut) && bot.canGather)
            {
                s.value = std::max(s.value, xpm * 4.0f);
                s.reason = "broke: gather to sell (no fighting)";
            }
            break;
        case GoalKind::Mail:
            // a gift or delivery from a real player is fetched soon; other mail is a small chore
            s.value = xpm * (bot.playerMail ? CHORE_URGENT_MINUTES : CHORE_NORMAL_MINUTES);
            s.minutes = travel + CHORE_MINUTES;
            s.reason = bot.playerMail ? "mail from a player" : "mail";
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

        // broke with damaged gear: earn money with easy fights until a full repair is affordable.
        // Mobs 1-6 levels below are worth double (safe kills, loot to sell), anything at or above
        // the bot's level much less; harder quest objectives wait.
        // careful mode (S: once gear is broken, or after dying a lot, don't ram into a wall): broke,
        // broken gear (weapon or 3+ items at 0 durability) or 3+ deaths in 30 minutes. Only green
        // mobs - 3+ levels below and still giving XP - are worth fighting; anything closer to the
        // bot's level waits, quest objectives included. 673 of 800 bots had 3+ broken items and
        // 40% of deaths were to mobs 3+ levels below the bot.
        // Resurrection Sickness (S: play safe until it wears off): no new fights at all; turn-ins,
        // town chores and resting in town keep their value
        if (bot.resSick && (c.kind == GoalKind::Grind || c.kind == GoalKind::QuestObjective || c.kind == GoalKind::Gather))
        {
            s.value = 0.0f;
            s.reason = "res sickness: rest";
        }
        if (bot.resSick && (c.kind == GoalKind::Vendor || c.kind == GoalKind::Repair || c.kind == GoalKind::Trainer || c.kind == GoalKind::Mail))
            s.value *= 2.0f;
        bool careful = bot.broke || bot.brokenGear || bot.recentDeaths >= 3 || bot.struggling;
        if (careful && c.mobLevel > 0 && (c.kind == GoalKind::Grind || c.kind == GoalKind::QuestObjective))
        {
            int32_t gap = int32_t(bot.level) - c.mobLevel;
            bool green = gap >= 3 && c.mobLevel > bot.greyLevel;
            if (bot.destitute && c.kind == GoalKind::Grind && c.mobLevel <= bot.greyLevel)
                ;   // grey: valued above as safe money
            else if (green)
            {
                if (c.kind == GoalKind::Grind)
                    s.value *= 2.5f;
                s.reason = bot.recentDeaths >= 3 ? "dying: green mobs" : "broke: green mobs";
            }
            // quests are a broke bot's best money and XP (S, Nov 25: stuck broke bots handed in 2
            // quests per 3 hours against 5 for the rest, and took 1 new one against 4): a quest
            // objective at or below the bot's level and not elite keeps its full value; only harder
            // ones, and grind spots near its level, wait
            else if (c.kind == GoalKind::QuestObjective && c.mobLevel <= int32_t(bot.level) && !c.elite)
                ;
            else if (gap < 3)
                s.value *= 0.15f;
        }
        // broke: a hand-in is money right now
        if ((bot.broke || bot.brokenGear) && c.kind == GoalKind::QuestTurnIn)
        {
            s.value *= 1.5f;
            s.reason = "broke: hand in for money";
        }

        // crowding (Phase 2a): a camp or objective area shared with other bots is worth less
        if (c.crowdFactor > 1.0f && (c.kind == GoalKind::Grind || c.kind == GoalKind::QuestObjective))
        {
            s.value /= c.crowdFactor;
            s.reason = "crowded";
        }

        // escort, timed and group quests are not solo goals (quest index, Phase 2a)
        if (c.avoid && (c.kind == GoalKind::QuestObjective || c.kind == GoalKind::QuestGiver))
        {
            s.value = 0.0f;
            s.reason = "avoid: group/escort/timed";
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
