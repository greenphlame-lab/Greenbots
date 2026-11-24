#include "playerbot/playerbot.h"
#include "AutoActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/silvermoon/DecisionLog.h"
#include <cstdio>
#include <ctime>

using namespace ai;

bool SilvermoonAutoAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;
    // only your own always-online characters, and only you
    if (!sPlayerbotAIConfig.IsFreeAltBot(bot) || requester->GetSession()->GetAccountId() != bot->GetSession()->GetAccountId())
    {
        ai->TellPlayerNoFacing(requester, "Auto mode is only for your own always-online characters.");
        return false;
    }
    std::string param = event.getParam();
    uint32 guid = bot->GetGUIDLow();
    if (param == "on" || param == "off")
    {
        bool on = param == "on";
        SilvermoonSetAuto(guid, on);
        if (on)
        {
            if (bot->GetGroup())
                ai->TellPlayerNoFacing(requester, "Auto is on. I'll start playing on my own once I'm not in a group with you.");
            else
            {
                ai->ChangeStrategy("+travel,+rpg,+grind,+silvermoon brain", BotState::BOT_STATE_NON_COMBAT);
                ai->TellPlayerNoFacing(requester, "Auto is on. I'm off to play on my own. Whisper \"why\" any time to see what I'm doing.");
            }
        }
        else
        {
            ai->ChangeStrategy("-silvermoon brain", BotState::BOT_STATE_NON_COMBAT);
            if (TravelTarget* target = AI_VALUE(TravelTarget*, "travel target"))
                target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            ai->TellPlayerNoFacing(requester, "Auto is off. Back to normal.");
        }
        return true;
    }
    ai->TellPlayerNoFacing(requester, std::string("Auto is ") + (SilvermoonAutoOn(guid) ? "on" : "off") + ". Whisper \"auto on\" or \"auto off\" to change it.");
    return true;
}

bool SilvermoonWhyAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;
    if (!ai->HasStrategy("silvermoon brain", BotState::BOT_STATE_NON_COMBAT))
    {
        ai->TellPlayerNoFacing(requester, "I'm not on the brain right now (following you, in a group, a dungeon or a battleground, or auto is off).");
        return true;
    }
    silvermoon::DecisionRow row;
    if (!silvermoon::DecisionLog::Last(bot->GetGUIDLow(), row))
    {
        ai->TellPlayerNoFacing(requester, "I haven't picked a goal yet.");
        return true;
    }
    char buf[512];
    uint32 ago = row.ts ? uint32(time(nullptr)) - row.ts : 0;
    snprintf(buf, sizeof(buf), "Goal: %s - %s (score %.0f, because: %s), chosen %u min ago.",
        row.goal.c_str(), row.target.c_str(), row.score, row.reason.c_str(), ago / 60);
    ai->TellPlayerNoFacing(requester, buf);
    if (!row.runnerUp1.empty())
    {
        snprintf(buf, sizeof(buf), "Passed on: %s (%.0f)%s%s%s", row.runnerUp1.c_str(), row.runnerUp1Score,
            row.runnerUp2.empty() ? "" : ", ", row.runnerUp2.c_str(), "");
        std::string out(buf);
        if (!row.runnerUp2.empty())
        {
            char s2[32];
            snprintf(s2, sizeof(s2), " (%.0f)", row.runnerUp2Score);
            out += s2;
        }
        ai->TellPlayerNoFacing(requester, out);
    }
    return true;
}
