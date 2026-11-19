#include "playerbot/playerbot.h"
#include "RelocateGoal.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/silvermoon/Hubs.h"
#include "playerbot/silvermoon/DecisionLog.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

using namespace ai;
using namespace silvermoon;

namespace
{
    const float RELOCATE_MINUTES = 45.0f;        // expected questing at a fresh hub
    const float UNVISITED_BONUS = 1.3f;
    const float THIN_SHARE = 0.25f;              // local options "thin" below 25% of the typical rate
    const uint32 EXPLORE_ONE_IN = 4;             // unvisited in-range hubs in about 1 of 4 decisions
    const size_t MAX_HUBS = 3;
    const size_t ROUTED_HUBS = 2;                // real route time for the top 1-2
    const float AT_HUB = 400.0f;                 // already at this hub
    const float ARRIVED = 40.0f;
    const uint32 MAX_FAILED_TRIPS = 3;

    // typical XP rate: latest rate per bot, medians per level rebuilt every 5 minutes
    std::mutex rateLock;
    std::unordered_map<uint32, std::tuple<uint32, float, time_t>> rates;   // guid -> level, rate, when
    std::map<uint32, float> medians;
    time_t mediansAt = 0;

    void RebuildMedians()
    {
        time_t now = time(nullptr);
        if (now - mediansAt < 5 * MINUTE)
            return;
        mediansAt = now;
        std::map<uint32, std::vector<float>> byLevel;
        for (auto const& [guid, r] : rates)
            if (now - std::get<2>(r) < 30 * MINUTE && std::get<1>(r) > 0.0f)
                byLevel[std::get<0>(r)].push_back(std::get<1>(r));
        medians.clear();
        for (auto& [level, v] : byLevel)
        {
            if (v.size() < 5)
                continue;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            medians[level] = v[v.size() / 2];
        }
    }

    struct Trip
    {
        uint16 hub = 0;
        TravelDestination* destination = nullptr;
        time_t start = 0;
        float estimateMinutes = 0.0f;
        uint32 deaths = 0;
        bool wasAlive = true;
    };
    std::mutex tripLock;
    std::unordered_map<uint32, Trip> trips;
    std::unordered_map<uint32, uint32> failures;
    std::set<uint32> wanted;

    float RouteMinutes(Player* bot, WorldPosition const& to)
    {
        WorldPosition from(bot);
        std::vector<WorldPosition> beginPath, endPath;
        TravelNodeRoute route = sTravelNodeMap.getRoute(from, to, beginPath, endPath, bot);
        if (route.isEmpty())
            return -1.0f;
        float seconds = 0.0f;
        std::vector<TravelNode*>& nodes = route.getNodes();
        uint32 gold = bot->GetMoney();
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
        {
            auto links = nodes[i]->getLinks();
            auto it = links->find(nodes[i + 1]);
            float cost = it != links->end() ? it->second->getCost(bot, gold) : -1.0f;
            if (cost <= 0.0f)
                cost = nodes[i]->fDist(nodes[i + 1]) / 7.0f;   // hearthstone portal or unknown: rough walk time
            seconds += cost;
        }
        if (!nodes.empty())
        {
            seconds += from.fDist(*nodes.front()->getPosition()) / 7.0f;
            seconds += nodes.back()->getPosition()->fDist(to) / 7.0f;
        }
        route.cleanTempNodes();
        return seconds / 60.0f;
    }
}

void ai::SilvermoonNoteXpRate(uint32 guid, uint32 level, float xpPerMinute)
{
    std::lock_guard<std::mutex> guard(rateLock);
    rates[guid] = { level, xpPerMinute, time(nullptr) };
}

float ai::SilvermoonTypicalXpPerMinute(uint32 level)
{
    std::lock_guard<std::mutex> guard(rateLock);
    RebuildMedians();
    auto it = medians.find(level);
    if (it != medians.end())
        return it->second;
    // nearest level with enough bots, scaled by the level ratio
    float best = 0.0f;
    uint32 bestGap = 1000;
    for (auto const& [l, m] : medians)
    {
        uint32 gap = l > level ? l - level : level - l;
        if (gap < bestGap)
        {
            bestGap = gap;
            best = m * float(level) / float(std::max<uint32>(1, l));
        }
    }
    return best > 0.0f ? best : level * 20.0f;
}

bool ai::SilvermoonBestRelocate(Player* bot, PlayerbotAI* ai, float bestLocalScore, SilvermoonScoredGoal& goal, uint16& hubId)
{
    if (!sPlayerbotAIConfig.silvermoonTravel || bot->GetGroup() || !bot->GetMap() || !bot->GetMap()->IsContinent())
        return false;
    uint32 guid = bot->GetGUIDLow();
    uint32 level = bot->GetLevel();
    float typical = SilvermoonTypicalXpPerMinute(level);

    bool want = false;
    {
        std::lock_guard<std::mutex> guard(tripLock);
        want = wanted.erase(guid) > 0;
    }
    bool thin = want || bestLocalScore < THIN_SHARE * typical;
    bool explore = !thin && urand(1, EXPLORE_ONE_IN) == 1;
    if (!thin && !explore)
        return false;

    bool alliance = bot->GetTeam() == ALLIANCE;
    WorldPosition here(bot);
    struct Option { Hub const* hub; float distance; bool visited; float score; float minutes; };
    std::vector<Option> options;
    for (Hub const& h : Hubs::All())
    {
        if (!Hubs::Knows(guid, h, alliance))
            continue;
        if (int32(level) < int32(h.minLevel) - 1 || int32(level) > int32(h.maxLevel) - 2)
            continue;
        bool visited = Hubs::Visited(guid, h.id);
        if (explore && visited)
            continue;
        WorldPosition target(h.map, h.innkeeper ? h.innX : h.x, h.innkeeper ? h.innY : h.y, h.innkeeper ? h.innZ : h.z);
        float distance = here.distance(target);
        if (h.map == here.getMapId() && here.fDist(target) < AT_HUB)
            continue;
        options.push_back({ &h, distance, visited, 0.0f, 0.0f });
    }
    if (options.empty())
        return false;
    std::sort(options.begin(), options.end(), [](Option const& a, Option const& b) { return a.distance < b.distance; });
    if (options.size() > MAX_HUBS)
        options.resize(MAX_HUBS);

    float speed = (bot->IsMounted() ? 14.0f : 7.0f) * 60.0f;   // yards per minute
    float value = typical * RELOCATE_MINUTES;
    for (Option& o : options)
    {
        o.minutes = o.distance / speed;
        o.score = value * (o.visited ? 1.0f : UNVISITED_BONUS) / std::max(1.0f, o.minutes);
    }
    std::sort(options.begin(), options.end(), [](Option const& a, Option const& b) { return a.score > b.score; });
    // the real route for the top 1-2, re-scored with it: close on the map but far by road loses
    for (size_t i = 0; i < options.size() && i < ROUTED_HUBS; ++i)
    {
        Hub const* h = options[i].hub;
        WorldPosition target(h->map, h->innkeeper ? h->innX : h->x, h->innkeeper ? h->innY : h->y, h->innkeeper ? h->innZ : h->z);
        float minutes = RouteMinutes(bot, target);
        if (minutes < 0.0f)
        {
            options[i].score = 0.0f;   // no route
            continue;
        }
        options[i].minutes = minutes;
        options[i].score = value * (options[i].visited ? 1.0f : UNVISITED_BONUS) / std::max(1.0f, minutes);
    }
    std::sort(options.begin(), options.begin() + std::min(options.size(), ROUTED_HUBS),
        [](Option const& a, Option const& b) { return a.score > b.score; });
    Option const& best = options.front();
    if (best.score <= 0.0f || best.score <= bestLocalScore)
        return false;

    Hub const* h = best.hub;
    WorldPosition target(h->map, h->innkeeper ? h->innX : h->x, h->innkeeper ? h->innY : h->y, h->innkeeper ? h->innZ : h->z);
    TemporaryTravelDestination* destination = new TemporaryTravelDestination(target);
    goal.destination = destination;
    goal.position = destination->GetPosition();
    goal.kind = GoalKind::Relocate;
    goal.score.value = value;
    goal.score.minutes = best.minutes;
    goal.score.score = best.score;
    goal.score.reason = std::string(thin ? "relocate: " : "relocate (new hub): ") + h->name;
    hubId = h->id;
    return true;
}

void ai::SilvermoonRelocateStarted(uint32 guid, uint16 hubId, TravelDestination* destination, float estimateMinutes)
{
    std::lock_guard<std::mutex> guard(tripLock);
    Trip trip;
    trip.hub = hubId;
    trip.destination = destination;
    trip.start = time(nullptr);
    trip.estimateMinutes = estimateMinutes;
    trips[guid] = trip;
}

void ai::SilvermoonWantRelocate(uint32 guid)
{
    std::lock_guard<std::mutex> guard(tripLock);
    wanted.insert(guid);
}

uint32 ai::SilvermoonRelocateFailures(uint32 guid)
{
    std::lock_guard<std::mutex> guard(tripLock);
    auto it = failures.find(guid);
    return it == failures.end() ? 0 : it->second;
}

void ai::SilvermoonResetRelocateFailures(uint32 guid)
{
    std::lock_guard<std::mutex> guard(tripLock);
    failures.erase(guid);
}

bool ai::SilvermoonRelocateTick(Player* bot, PlayerbotAI* ai)
{
    uint32 guid = bot->GetGUIDLow();
    Trip trip;
    {
        std::lock_guard<std::mutex> guard(tripLock);
        auto it = trips.find(guid);
        if (it == trips.end())
            return false;
        trip = it->second;
    }
    Hub const* hub = Hubs::Get(trip.hub);
    if (!hub)
        return false;
    TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    // deaths on the way (the tick also runs while the bot is dead)
    {
        std::lock_guard<std::mutex> guard(tripLock);
        auto it = trips.find(guid);
        if (it != trips.end())
        {
            if (it->second.wasAlive && !bot->IsAlive())
                ++it->second.deaths;
            it->second.wasAlive = bot->IsAlive();
            trip = it->second;
        }
    }
    if (!bot->IsAlive())
        return false;
    // abandon a trip that runs over twice its route estimate, cost two deaths, or no longer fits
    // the bot's level (it out-levelled the hub on the way)
    std::string abandon;
    if (trip.estimateMinutes > 0.0f && time(nullptr) - trip.start > time_t(2.0f * trip.estimateMinutes * MINUTE) + 2 * MINUTE)
        abandon = "over twice the route time";
    else if (trip.deaths >= 2)
        abandon = "died twice on the way";
    else if (int32(bot->GetLevel()) > int32(hub->maxLevel))
        abandon = "out-levelled the hub";
    if (!abandon.empty())
    {
        DecisionRow row;
        row.ts = uint32(time(nullptr));
        row.guid = guid;
        row.goal = "relocate";
        row.target = hub->name;
        row.reason = "relocate abandoned: " + abandon;
        row.map = bot->GetMapId();
        row.x = bot->GetPositionX();
        row.y = bot->GetPositionY();
        DecisionLog::Add(row);
        if (target && target->GetDestination() == trip.destination)
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        std::lock_guard<std::mutex> guard(tripLock);
        trips.erase(guid);
        ++failures[guid];
        return false;
    }
    WorldPosition here(bot);
    WorldPosition dest(hub->map, hub->innkeeper ? hub->innX : hub->x, hub->innkeeper ? hub->innY : hub->y, hub->innkeeper ? hub->innZ : hub->z);
    bool arrived = here.getMapId() == dest.getMapId() && here.fDist(dest) < ARRIVED;
    bool stillGoing = target && target->GetDestination() == trip.destination && target->IsActive();

    if (arrived)
    {
        // hearthstone at the innkeeper, the way a player does it: the stock "home" action with the
        // innkeeper as rpg target and selection
        if (hub->innkeeper)
        {
            std::list<ObjectGuid> npcs = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs")->Get();
            for (ObjectGuid const& npc : npcs)
            {
                Unit* unit = ai->GetUnit(npc);
                if (!unit || !unit->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_INNKEEPER))
                    continue;
                ai->GetAiObjectContext()->GetValue<GuidPosition>("rpg target")->Set(GuidPosition(unit));
                bot->SetSelectionGuid(npc);
                ai->DoSpecificAction("home", Event(), true);
                break;
            }
        }
        Hubs::NoteVisits(bot);
        DecisionRow row;
        row.ts = uint32(time(nullptr));
        row.guid = guid;
        row.goal = "relocate";
        row.target = hub->name;
        row.reason = "relocate: arrived";
        row.map = bot->GetMapId();
        row.x = bot->GetPositionX();
        row.y = bot->GetPositionY();
        DecisionLog::Add(row);
        if (stillGoing)
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        std::lock_guard<std::mutex> guard(tripLock);
        trips.erase(guid);
        failures.erase(guid);
        return true;
    }
    if (!stillGoing)
    {
        // the trip ended (cooldown, expiry, stuck give-up, or another goal won) without arriving
        std::lock_guard<std::mutex> guard(tripLock);
        trips.erase(guid);
        ++failures[guid];
    }
    return false;
}
