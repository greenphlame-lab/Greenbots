#include "playerbot/playerbot.h"
#include "AutoActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/silvermoon/DecisionLog.h"
#include "Chat/Chat.h"
#include <cstdio>
#include <ctime>

using namespace ai;

namespace
{
    std::string Clean(std::string s)
    {
        for (char& c : s)
            if (c == ';' || c == '|' || c == '\t' || c == '\n')
                c = ' ';
        if (s.size() > 90)
            s = s.substr(0, 90);
        return s;
    }
}

void SilvermoonPushBrainPanel(Player* bot, Player* owner)
{
    if (!bot || !owner || !owner->GetSession())
        return;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    // your own character: auto = it has the player AI (self-bot); an always-online alt: its switch
    bool on = bot == owner ? ai != nullptr : SilvermoonAutoOn(bot->GetGUIDLow());
    bool brain = ai && ai->HasStrategy("silvermoon brain", BotState::BOT_STATE_NON_COMBAT);
    silvermoon::DecisionRow row;
    bool have = brain && silvermoon::DecisionLog::Last(bot->GetGUIDLow(), row);
    std::string goal = have ? row.goal : (on ? (bot->GetGroup() ? "waiting (in a group)" : "starting up") : "manual");
    char score[16], ago[16];
    snprintf(score, sizeof(score), "%.0f", have ? row.score : 0.0f);
    snprintf(ago, sizeof(ago), "%u", have && row.ts ? (uint32(time(nullptr)) - row.ts) / 60 : 0);
    std::string text = std::string("SMBRAIN\t") + Clean(bot->GetName()) + ";" + (on ? "1" : "0") + ";" + std::to_string(bot->GetLevel()) + ";" +
        Clean(goal) + ";" + Clean(have ? row.target : "") + ";" + Clean(have ? row.reason : "") + ";" + score + ";" +
        Clean(have ? row.runnerUp1 : "") + ";" + ago;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, text.c_str(), LANG_ADDON, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());
    owner->GetSession()->SendPacket(data);
}

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
        SilvermoonPushBrainPanel(bot, requester);
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
