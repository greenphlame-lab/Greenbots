#include "playerbot/playerbot.h"
#include "RelocateGoal.h"
#include "Policies.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/silvermoon/Hubs.h"
#include "playerbot/silvermoon/DecisionLog.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
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
    const float OUTGROWN_LOCAL_WEIGHT = 0.25f;   // outgrown bots: local goals count a quarter against relocating
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
        // diagnostics for trips that end without arriving
        bool sawCooldown = false;
        uint32 maxMoveRetries = 0;
        float startX = 0, startY = 0;
        uint16 startMap = 0xFFFF;
    };
    std::mutex tripLock;
    std::unordered_map<uint32, Trip> trips;
    std::unordered_map<uint32, uint32> failures;
    std::unordered_map<uint32, time_t> arrivedAt;   // guid -> last arrival at a hub (stay a while)
    const time_t STAY_AFTER_ARRIVAL = 45 * MINUTE;
    std::unordered_map<uint32, std::unordered_map<uint16, time_t>> skipHub;   // guid -> hub -> until (failed trips)

    void FailTrip(uint32 guid, uint16 hub)   // tripLock held
    {
        ++failures[guid];
        skipHub[guid][hub] = time(nullptr) + 30 * MINUTE;
    }

    bool Skipped(uint32 guid, uint16 hub)
    {
        std::lock_guard<std::mutex> guard(tripLock);
        auto it = skipHub.find(guid);
        if (it == skipHub.end())
            return false;
        auto h = it->second.find(hub);
        return h != it->second.end() && time(nullptr) < h->second;
    }
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
    // outgrown: the bot is well above its area's level (a level 21-28 crowd stayed in Durotar with
    // no relocation at all: grinding grey mobs still scored above the "thin" line)
    bool outgrown = false;
    {
        uint32 zoneId = 0, areaId = 0;
        bot->GetZoneAndAreaId(zoneId, areaId);
        // the zone's level, not the town's (S: a hub is as high as its whole zone - Menethil
        // Harbor's own lower level made level 27s "outgrow" Wetlands on arrival and fly on)
        for (uint32 id : { zoneId, areaId })
        {
            AreaTableEntry const* entry = id ? GetAreaEntryByAreaID(id) : nullptr;
            if (entry && entry->area_level > 0)
            {
                // +3 (was +5): S wants bots that outgrew an area to make getting to a
                // level-appropriate zone a top priority (90 of 800 level 15+ bots were still in
                // starter zones, mostly finishing leftover low quests or grinding)
                outgrown = int32(level) > int32(entry->area_level) + 3;
                break;
            }
        }
    }
    // stuck at this level (S: Cavol spent 47 hours at level 24 in a Barrens with 213 Horde bots -
    // crowded quests, open quest log, broke): 18+ hours at one level (about twice the usual 10 at
    // level 20-24) overrides the open-quests hold
    // and counts as thin, so the bot goes looking for a less crowded hub
    bool stuckAtLevel = level >= 10 && bot->m_Played_time[PLAYED_TIME_LEVEL] >= 18 * HOUR;
    // a bot under level 6 stays in its starting area (Grashnak, level 1, set off for Red Cloud
    // Mesa - another race's starting valley - two minutes after logging in)
    if (level < 6 && !outgrown && !want)
        return false;
    // Hub ping-pong (S: 20+ bots never seemed to leave Elwynn/Stormwind): after arriving at a hub
    // a bot stays at least 45 minutes before choosing another trip
    if (!want)
    {
        std::lock_guard<std::mutex> guard(tripLock);
        auto it = arrivedAt.find(guid);
        if (it != arrivedAt.end() && time(nullptr) - it->second < STAY_AFTER_ARRIVAL)
            return false;
    }
    // open quests here keep the bot here (Grashnak, level 11 with 13 Durotar quests open, was sent
    // to the Crossroads twice and walked straight back to the Southfury River each time, ~2 hours):
    // 3+ unfinished quests for this zone that aren't far below the bot block a trip
    if (!want && !stuckAtLevel)
    {
        uint32 zoneId = bot->GetZoneId();
        uint32 open = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
            if (quest && quest->GetZoneOrSort() == int32(zoneId) && quest->GetQuestLevel() + 4 >= int32(level))
                ++open;
        }
        if (open >= 3)
            return false;
    }
    // top up before a trip (S): at least 80% health and mana, out of combat
    if (!want && bot->IsInCombat())
        return false;
    if (!want && (bot->GetHealthPercent() < 80.0f ||
        (bot->GetPowerType() == POWER_MANA && bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPower(POWER_MANA) * 100 < bot->GetMaxPower(POWER_MANA) * 80)))
        return false;   // a wanted trip (e.g. leaving a zone where players keep killing it) goes at once
    bool thin = want || outgrown || stuckAtLevel || bestLocalScore < THIN_SHARE * typical;
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
        if (int32(level) < int32(h.minLevel) + 2 || int32(level) > int32(h.maxLevel) - 2)
            continue;
        if (Skipped(guid, h.id))
            continue;                                   // failed to get there recently
        if (SilvermoonIsOwn(bot))
        {
            uint32 zoneId = 0, areaId = 0;
            sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, h.map, h.x, h.y, h.z);
            if (SilvermoonZoneBlocked(guid, zoneId))
                continue;                               // ticked off in the Policies window
        }
        if (uint32 avoided = SilvermoonAvoidedZone(guid))
        {
            uint32 zoneId = 0, areaId = 0;
            sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, h.map, h.x, h.y, h.z);
            if (zoneId == avoided)
                continue;                               // died to players there twice lately
        }
        // stuck at its level: somewhere other than the zone it's stuck in
        if (stuckAtLevel)
        {
            uint32 zoneId = 0, areaId = 0;
            sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, h.map, h.x, h.y, h.z);
            if (zoneId == bot->GetZoneId())
                continue;
        }
        bool visited = Hubs::Visited(guid, h.id);
        if (explore && visited)
            continue;
        WorldPosition target(h.map, h.innkeeper ? h.innX : h.x, h.innkeeper ? h.innY : h.y, h.innkeeper ? h.innZ : h.z);
        float distance = here.distance(target);
        if (h.map == here.getMapId() && here.fDist(target) < AT_HUB)
            continue;
        options.push_back({ &h, distance, visited, 0.0f, 0.0f });
    }
    // same continent first: a trip to another continent needs a boat or zeppelin and failed far
    // more often; other-continent hubs only when none on this one fit
    if (std::any_of(options.begin(), options.end(), [&](Option const& o) { return o.hub->map == here.getMapId(); }))
        options.erase(std::remove_if(options.begin(), options.end(), [&](Option const& o) { return o.hub->map != here.getMapId(); }), options.end());
    if (options.empty())
    {
        // an outgrown bot with no hub to go to counts a failed trip, so the level-zone teleport
        // fallback still comes round after 3
        if (want || outgrown)
        {
            std::lock_guard<std::mutex> guard(tripLock);
            ++failures[guid];
        }
        return false;
    }
    std::sort(options.begin(), options.end(), [](Option const& a, Option const& b) { return a.distance < b.distance; });
    if (options.size() > MAX_HUBS)
        options.resize(MAX_HUBS);

    float speed = (bot->IsMounted() ? 14.0f : 7.0f) * 60.0f;   // yards per minute
    float value = typical * RELOCATE_MINUTES;
    for (Option& o : options)
    {
        o.minutes = o.distance / speed;
        // XP per minute over the whole plan (the trip plus the questing there), the same unit as a
        // local goal: dividing by the trip alone made a hub 3 minutes away look ~15x better than any
        // quest, and bots relocated 2,100 times in an hour
        o.score = value * (o.visited ? 1.0f : UNVISITED_BONUS) / (o.minutes + RELOCATE_MINUTES);
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
        options[i].score = value * (options[i].visited ? 1.0f : UNVISITED_BONUS) / (minutes + RELOCATE_MINUTES);
    }
    std::sort(options.begin(), options.begin() + std::min(options.size(), ROUTED_HUBS),
        [](Option const& a, Option const& b) { return a.score > b.score; });
    Option const& best = options.front();
    // an outgrown bot weighs what it could still do here at a quarter: leftover low-level quests
    // and grey mobs kept beating the trip
    if (best.score <= 0.0f || best.score <= bestLocalScore * (outgrown ? OUTGROWN_LOCAL_WEIGHT : 1.0f))
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
    // a trip to the same hub picked again within 2 minutes means the last one was dropped at once
    // (the travel target went inactive within seconds - no route out, blocked by enemy ground, the
    // search cap...): 1,144 of ~2,000 relocation choices in an hour were the same hub re-picked
    // every ~5 seconds by a bot standing still. Count it as a failed trip, so the hub is skipped
    // for 30 minutes and the level-zone teleport fallback comes after 3 failures.
    auto previous = trips.find(guid);
    if (previous != trips.end() && previous->second.hub == hubId && time(nullptr) - previous->second.start < 2 * MINUTE)
    {
        FailTrip(guid, hubId);
        DecisionRow row;
        row.ts = uint32(time(nullptr));
        row.guid = guid;
        row.goal = "relocate";
        row.target = "";
        row.reason = "relocate failed: dropped at once";
        DecisionLog::Add(row);
        trips.erase(previous);
        return;
    }
    Trip trip;
    trip.hub = hubId;
    trip.destination = destination;
    trip.start = time(nullptr);
    trip.estimateMinutes = estimateMinutes;
    trips[guid] = trip;
}

bool ai::SilvermoonOnRelocateTrip(uint32 guid)
{
    std::lock_guard<std::mutex> guard(tripLock);
    return trips.count(guid) > 0;
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
    if (trip.estimateMinutes > 0.0f && time(nullptr) - trip.start > time_t(2.0f * trip.estimateMinutes * MINUTE) + 5 * MINUTE)
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
        FailTrip(guid, trip.hub);
        return false;
    }
    WorldPosition here(bot);
    WorldPosition dest(hub->map, hub->innkeeper ? hub->innX : hub->x, hub->innkeeper ? hub->innY : hub->y, hub->innkeeper ? hub->innZ : hub->z);
    bool arrived = here.getMapId() == dest.getMapId() && here.fDist(dest) < ARRIVED;
    bool stillGoing = target && target->GetDestination() == trip.destination && target->IsActive();
    if (target && target->GetDestination() == trip.destination)
    {
        std::lock_guard<std::mutex> guard(tripLock);
        auto it = trips.find(guid);
        if (it != trips.end())
        {
            if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN)
                it->second.sawCooldown = true;
            it->second.maxMoveRetries = std::max(it->second.maxMoveRetries, target->GetRetryCount(true));
            if (it->second.startMap == 0xFFFF)
            {
                it->second.startMap = here.getMapId();
                it->second.startX = here.getX();
                it->second.startY = here.getY();
            }
            trip = it->second;
        }
    }

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
        arrivedAt[guid] = time(nullptr);
        return true;
    }
    if (!stillGoing)
    {
        // the trip ended without arriving: log how, so silent ends can be told apart
        std::string how = !target || !target->GetDestination() ? "no target" :
            target->GetDestination() != trip.destination ? "another goal took over" :
            target->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN ? "could not move there" :
            target->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED ? "trip timer ran out" : "ended";
        DecisionRow row;
        row.ts = uint32(time(nullptr));
        row.guid = guid;
        row.goal = "relocate";
        row.target = hub->name;
        // short diagnostics: went into cooldown (moves kept failing), most move retries seen, yards
        // moved since the trip started, yards still to go
        float moved = trip.startMap == here.getMapId() ? std::sqrt((here.getX() - trip.startX) * (here.getX() - trip.startX) +
            (here.getY() - trip.startY) * (here.getY() - trip.startY)) : -1.0f;
        char diag[64];
        snprintf(diag, sizeof(diag), " [%s r%u m%d left%d]", trip.sawCooldown ? "cd" : "-", trip.maxMoveRetries,
            int(moved), int(here.getMapId() == dest.getMapId() ? here.fDist(dest) : -1.0f));
        row.reason = "relocate failed: " + how + diag;
        row.map = bot->GetMapId();
        row.x = bot->GetPositionX();
        row.y = bot->GetPositionY();
        DecisionLog::Add(row);
        std::lock_guard<std::mutex> guard(tripLock);
        trips.erase(guid);
        FailTrip(guid, trip.hub);
    }
    return false;
}
