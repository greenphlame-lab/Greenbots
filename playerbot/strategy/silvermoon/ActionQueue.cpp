#include "playerbot/playerbot.h"
#include "ActionQueue.h"
#include "Policies.h"
#include "Professions.h"
#include "playerbot/TravelNode.h"
#include "Globals/ObjectMgr.h"
#include "Server/SQLStorages.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace ai;

namespace
{
    struct AqStep
    {
        uint8 kind = AQ_NONE;
        uint32 a = 0, b = 0, c = 0;
        time_t started = 0;
        bool issued = false;      // a Go step's order was given
    };
    struct AqQueue
    {
        bool only = false;
        std::vector<AqStep> steps;
    };

    std::mutex queueLock;
    std::unordered_map<uint32, AqQueue> queues;
    std::unordered_map<uint32, bool> loaded;
    const time_t STEP_TIMEOUT = 2 * HOUR;
    const size_t QUEUE_MAX = 20;
    const char* PLACES[] = { "inn", "flight", "ah", "bank", "vendor", "repair" };
    const char* PLACE_NAMES[] = { "the inn", "the flight master", "the auction house", "the bank", "a vendor", "a repair vendor" };

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return s;
    }

    std::string Clean(std::string s)
    {
        for (char& ch : s)
            if (ch == '|' || ch == '^' || ch == ';' || ch == '\t')
                ch = ' ';
        return s;
    }

    // with queueLock held
    AqQueue& Get(uint32 guid)
    {
        if (!loaded[guid])
        {
            loaded[guid] = true;
            static std::once_flag table;
            std::call_once(table, []()
            {
                CharacterDatabase.Execute("CREATE TABLE IF NOT EXISTS silvermoon_action_queue (guid INT UNSIGNED NOT NULL PRIMARY KEY, "
                    "only_queue TINYINT UNSIGNED NOT NULL DEFAULT 0, data TEXT NOT NULL, updated_at INT UNSIGNED NOT NULL DEFAULT 0)");
            });
            AqQueue q;
            if (auto r = CharacterDatabase.PQuery("SELECT only_queue, data FROM silvermoon_action_queue WHERE guid = %u", guid))
            {
                Field* f = r->Fetch();
                q.only = f[0].GetUInt32() != 0;
                std::stringstream ss(f[1].GetCppString());
                std::string part;
                while (std::getline(ss, part, ','))
                {
                    AqStep s;
                    unsigned k = 0, a = 0, b = 0, c = 0;
                    unsigned long st = 0;
                    unsigned is = 0;
                    if (sscanf(part.c_str(), "%u:%u:%u:%u:%lu:%u", &k, &a, &b, &c, &st, &is) >= 4 && k)
                    {
                        s.kind = uint8(k); s.a = a; s.b = b; s.c = c; s.started = time_t(st); s.issued = is != 0;
                        q.steps.push_back(s);
                    }
                }
            }
            queues[guid] = q;
        }
        return queues[guid];
    }

    void Save(uint32 guid, AqQueue const& q)
    {
        std::ostringstream out;
        bool first = true;
        for (AqStep const& s : q.steps)
        {
            out << (first ? "" : ",") << uint32(s.kind) << ":" << s.a << ":" << s.b << ":" << s.c << ":" << (unsigned long)s.started << ":" << (s.issued ? 1 : 0);
            first = false;
        }
        CharacterDatabase.PExecute("REPLACE INTO silvermoon_action_queue (guid, only_queue, data, updated_at) VALUES (%u, %u, '%s', UNIX_TIMESTAMP())",
            guid, q.only ? 1u : 0u, out.str().c_str());
    }

    std::string Label(AqStep const& s)
    {
        switch (s.kind)
        {
            case AQ_QUEST:
            {
                Quest const* q = sObjectMgr.GetQuestTemplate(s.a);
                return std::string("Quest: ") + (q ? q->GetTitle() : "#" + std::to_string(s.a));
            }
            case AQ_GRIND:
            {
                CreatureInfo const* ci = ObjectMgr::GetCreatureTemplate(s.a);
                std::string t = std::string("Grind ") + (ci ? ci->Name : "#" + std::to_string(s.a));
                if (s.b)
                    t += " to level " + std::to_string(s.b);
                if (s.c)
                    t += std::string(s.b ? " or " : " for ") + std::to_string(s.c) + " min";
                if (!s.b && !s.c)
                    t += " for 30 min";
                return t;
            }
            case AQ_GO:
                return std::string("Go to ") + (s.a < 6 ? PLACE_NAMES[s.a] : "?");
            case AQ_TRAIN:
                return "Train at the class trainer";
            case AQ_WAIT:
                return "Wait " + std::to_string(s.a) + " min";
        }
        return "?";
    }

    // quests given on a continent (creature or object quest givers spawned there), and creatures
    // spawned there - cached per map (1.8.1: the queue lists start from these)
    std::set<uint32> const& QuestsOnMap(uint32 map)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, std::set<uint32>> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(map);
        if (it != cache.end())
            return it->second;
        std::set<uint32>& out = cache[map];
        if (auto r = WorldDatabase.PQuery("SELECT DISTINCT qr.quest FROM creature_questrelation qr JOIN creature c ON c.id = qr.id WHERE c.map = %u "
            "UNION SELECT DISTINCT gr.quest FROM gameobject_questrelation gr JOIN gameobject g ON g.id = gr.id WHERE g.map = %u", map, map))
        {
            do { out.insert(r->Fetch()[0].GetUInt32()); } while (r->NextRow());
        }
        return out;
    }
    std::vector<uint32> const& MobsOnMap(uint32 map)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, std::vector<uint32>> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(map);
        if (it != cache.end())
            return it->second;
        std::vector<uint32>& out = cache[map];
        if (auto r = WorldDatabase.PQuery("SELECT DISTINCT c.id FROM creature c JOIN creature_template t ON t.entry = c.id WHERE c.map = %u AND t.NpcFlags = 0", map))
        {
            do { out.push_back(r->Fetch()[0].GetUInt32()); } while (r->NextRow());
        }
        return out;
    }

    void Note(Player* bot, AqStep const& s, char const* how)
    {
        std::string text = std::string("AqQueue: ") + Label(s) + " - " + how;
        sLog.outBasic("Silvermoon: %s %s", bot->GetName(), text.c_str());
        SilvermoonAwayNote(bot, text);
    }
}

bool ai::SilvermoonQueueCommand(Player* bot, std::string const& param, std::string& reply, std::vector<std::string>& messages)
{
    // param: everything after "aq "
    std::stringstream ss(param);
    std::string verb;
    ss >> verb;
    uint32 guid = bot->GetGUIDLow();

    if (verb == "find")
    {
        std::string what, text;
        ss >> what;
        std::getline(ss, text);
        while (!text.empty() && text[0] == ' ')
            text.erase(0, 1);
        text = Lower(text);
        // no text (1.8.1, S): the full list for this continent, best fits first; text narrows it
        int32 level = int32(bot->GetLevel());
        std::vector<std::pair<int32, std::string>> found;       // (distance from my level, "id|label")
        if (what == "quest")
        {
            std::set<uint32> const& here = QuestsOnMap(bot->GetMapId());
            for (auto const& [id, qp] : sObjectMgr.GetQuestTemplates())
            {
                Quest const* q = qp.get();
                if (!q || (!text.empty() && Lower(q->GetTitle()).find(text) == std::string::npos))
                    continue;
                bool inLog = bot->GetQuestStatus(id) == QUEST_STATUS_INCOMPLETE || bot->GetQuestStatus(id) == QUEST_STATUS_COMPLETE;
                if (!inLog && (bot->GetQuestRewardStatus(id) || !bot->CanTakeQuest(q, false) || !here.count(id)))
                    continue;           // takeable, and given on this continent (or already in the log)
                if (!inLog && q->GetQuestLevel() > 0 && (int32(q->GetQuestLevel()) > level + 4 || int32(q->GetQuestLevel()) < level - 10))
                    continue;
                int32 ql = q->GetQuestLevel() > 0 ? int32(q->GetQuestLevel()) : level;
                found.push_back({ std::abs(ql - level) - (inLog ? 100 : 0),
                    std::to_string(id) + "|" + Clean(q->GetTitle()) + " (" + std::to_string(ql) + (inLog ? ", in your log)" : ")") });
            }
        }
        else if (what == "mob")
        {
            std::unordered_map<std::string, bool> seen;
            for (uint32 id : MobsOnMap(bot->GetMapId()))
            {
                CreatureInfo const* ci = sCreatureStorage.LookupEntry<CreatureInfo>(id);
                if (!ci || !ci->Name || ci->NpcFlags || (!text.empty() && Lower(ci->Name).find(text) == std::string::npos))
                    continue;
                if (int32(ci->MaxLevel) < level - 10 || int32(ci->MinLevel) > level + 4)
                    continue;
                if (seen[ci->Name])
                    continue;
                seen[ci->Name] = true;
                found.push_back({ std::abs(int32(ci->MinLevel + ci->MaxLevel) / 2 - level),
                    std::to_string(id) + "|" + Clean(ci->Name) + " (" + std::to_string(ci->MinLevel) +
                    (ci->MaxLevel != ci->MinLevel ? "-" + std::to_string(ci->MaxLevel) : "") + ")" });
            }
        }
        else
            return false;
        std::sort(found.begin(), found.end());
        // short lines: "SMAQF\t<kind>;R;..." starts the results, "SMAQF\t<kind>;+;..." adds to them
        std::string msg = "SMAQF\t" + what + ";R;";
        bool first = true;
        for (size_t i = 0; i < found.size() && i < 60; ++i)
        {
            std::string part = found[i].second.substr(0, 70);
            if (!first && msg.size() + part.size() > 200)
            {
                messages.push_back(msg);
                msg = "SMAQF\t" + what + ";+;";
                first = true;
            }
            msg += (first ? "" : "^") + part;
            first = false;
        }
        messages.push_back(msg);
        reply = found.empty() ? "Nothing found." : std::to_string(std::min<size_t>(found.size(), 60)) + " found.";
        return true;
    }

    std::lock_guard<std::mutex> guard(queueLock);
    AqQueue& q = Get(guid);
    if (verb == "add")
    {
        std::string kind;
        ss >> kind;
        AqStep s;
        if (kind == "quest")
        {
            s.kind = AQ_QUEST;
            ss >> s.a;
            if (!sObjectMgr.GetQuestTemplate(s.a))
                return false;
        }
        else if (kind == "grind")
        {
            s.kind = AQ_GRIND;
            ss >> s.a >> s.b >> s.c;
            if (!ObjectMgr::GetCreatureTemplate(s.a))
                return false;
        }
        else if (kind == "go")
        {
            std::string place;
            ss >> place;
            s.kind = AQ_GO;
            s.a = 99;
            for (uint32 i = 0; i < 6; ++i)
                if (place == PLACES[i])
                    s.a = i;
            if (s.a == 99)
                return false;
        }
        else if (kind == "train")
            s.kind = AQ_TRAIN;
        else if (kind == "wait")
        {
            s.kind = AQ_WAIT;
            ss >> s.a;
            s.a = std::max<uint32>(1, std::min<uint32>(s.a, 240));
        }
        else
            return false;
        if (q.steps.size() >= QUEUE_MAX)
        {
            reply = "The queue is full (20 steps).";
            return true;
        }
        q.steps.push_back(s);
        reply = "Queued: " + Label(s) + " (step " + std::to_string(q.steps.size()) + ").";
    }
    else if (verb == "del" || verb == "up")
    {
        uint32 n = 0;
        ss >> n;
        if (!n || n > q.steps.size())
            return false;
        if (verb == "del")
        {
            reply = "Removed: " + Label(q.steps[n - 1]) + ".";
            q.steps.erase(q.steps.begin() + (n - 1));
        }
        else if (n > 1)
        {
            std::swap(q.steps[n - 1], q.steps[n - 2]);
            q.steps[0].started = 0;             // the new top step starts fresh
            q.steps[0].issued = false;
            reply = "Moved up.";
        }
    }
    else if (verb == "clear")
    {
        q.steps.clear();
        reply = "AqQueue cleared.";
    }
    else if (verb == "only")
    {
        std::string v;
        ss >> v;
        q.only = v == "on";
        reply = q.only ? "AqQueue only: I'll do just my queue, and wait when it's empty." : "AqQueue only off: I play on my own between queue steps.";
    }
    else
        return false;
    Save(guid, q);
    return true;
}

std::vector<std::string> ai::SilvermoonQueueText(Player* bot)
{
    std::lock_guard<std::mutex> guard(queueLock);
    AqQueue& q = Get(bot->GetGUIDLow());
    std::vector<std::string> out;
    std::string text = std::string("SMAQ\tR;") + (q.only ? "1" : "0") + ";";
    bool first = true;
    for (size_t i = 0; i < q.steps.size(); ++i)
    {
        std::string part = std::to_string(i + 1) + "|" + std::to_string(uint32(q.steps[i].kind)) + "|" + Clean(Label(q.steps[i])).substr(0, 60) + "|" +
            (i == 0 && q.steps[i].started ? "running" : "queued");
        if (!first && text.size() + part.size() > 200)
        {
            out.push_back(text);
            text = "SMAQ\t+;";
            first = true;
        }
        text += (first ? "" : "^") + part;
        first = false;
    }
    out.push_back(text);
    return out;
}

bool ai::SilvermoonQueueOnly(Player* bot)
{
    std::lock_guard<std::mutex> guard(queueLock);
    return Get(bot->GetGUIDLow()).only;
}

bool ai::SilvermoonQueueActive(Player* bot)
{
    if (!SilvermoonIsOwn(bot))
        return false;
    std::lock_guard<std::mutex> guard(queueLock);
    AqQueue& q = Get(bot->GetGUIDLow());
    return !q.steps.empty() && q.steps.front().started;
}

uint32_t ai::SilvermoonQueueGrindEntry(Player* bot)
{
    if (!SilvermoonIsOwn(bot))
        return 0;
    std::lock_guard<std::mutex> guard(queueLock);
    AqQueue& q = Get(bot->GetGUIDLow());
    return !q.steps.empty() && q.steps.front().kind == AQ_GRIND ? q.steps.front().a : 0;
}

uint32_t ai::SilvermoonQueueFetchQuest(Player* bot)
{
    if (!SilvermoonIsOwn(bot))
        return 0;
    std::lock_guard<std::mutex> guard(queueLock);
    AqQueue& q = Get(bot->GetGUIDLow());
    return !q.steps.empty() && q.steps.front().kind == AQ_QUEST ? q.steps.front().a : 0;
}

int ai::SilvermoonQueueStep(Player* bot, uint8_t& kind, uint32_t& id)
{
    kind = AQ_NONE;
    id = 0;
    if (!SilvermoonIsOwn(bot))
        return 0;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    std::lock_guard<std::mutex> guard(queueLock);
    uint32 guid = bot->GetGUIDLow();
    AqQueue& q = Get(guid);
    time_t now = time(nullptr);
    bool changed = false;
    while (!q.steps.empty())
    {
        AqStep& s = q.steps.front();
        if (!s.started)
        {
            s.started = now;
            changed = true;
        }
        char const* end = nullptr;
        switch (s.kind)
        {
            case AQ_QUEST:
            {
                Quest const* quest = sObjectMgr.GetQuestTemplate(s.a);
                QuestStatus st = bot->GetQuestStatus(s.a);
                if (!quest)
                    end = "failed (no such quest)";
                else if (bot->GetQuestRewardStatus(s.a))
                    end = "done";
                else if (st != QUEST_STATUS_INCOMPLETE && st != QUEST_STATUS_COMPLETE && !bot->CanTakeQuest(quest, false))
                    end = "failed (can't take it)";
                break;
            }
            case AQ_GRIND:
            {
                uint32 minutes = s.c ? s.c : (s.b ? 0 : 30);
                if (s.b && bot->GetLevel() >= s.b)
                    end = "done (level reached)";
                else if (minutes && now - s.started >= time_t(minutes) * MINUTE)
                    end = "done (time up)";
                break;
            }
            case AQ_GO:
                if (s.issued && !SilvermoonGoActive(bot))
                    end = "done";
                break;
            case AQ_TRAIN:
                if (ai && !ai->GetAiObjectContext()->GetValue<bool>("should travel named", "trainer class")->Get())
                    end = "done (nothing more to learn)";
                break;
            case AQ_WAIT:
                if (now - s.started >= time_t(s.a) * MINUTE)
                    end = "done";
                break;
            default:
                end = "dropped";
        }
        if (!end && now - s.started > STEP_TIMEOUT)
            end = "failed (2 hours without finishing)";
        if (!end)
            break;
        Note(bot, s, end);
        q.steps.erase(q.steps.begin());
        changed = true;
    }
    int result = 0;
    if (q.steps.empty())
        result = q.only ? 1 : 0;           // queue only: nothing to do, wait
    else
    {
        AqStep& s = q.steps.front();
        switch (s.kind)
        {
            case AQ_GO:
                if (!s.issued)
                {
                    std::string reply;
                    if (SilvermoonGoCommand(bot, PLACES[s.a], reply))
                        s.issued = true;
                    changed = true;
                }
                result = 1;                // the Go to step takes it from here
                break;
            case AQ_WAIT:
                result = 1;
                break;
            case AQ_QUEST:
            case AQ_GRIND:
            case AQ_TRAIN:
                kind = s.kind;
                id = s.a;
                result = 2;
                break;
        }
    }
    if (changed)
        Save(guid, q);
    return result;
}
