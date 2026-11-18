#include "Config/Config.h"

#include "playerbot/playerbot.h"
#include <atomic>
#include <array>
#include <deque>
#include <mutex>
#include <algorithm>
#include <functional>
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/RandomItemMgr.h"
#include "Tools/Formulas.h"
#include "playerbot/strategy/actions/GuildCreateActions.h"
#include "playerbot/RandomPlayerbotFactory.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/strategy/values/CraftValues.h"
#include "AuctionHouse/AuctionHouseMgr.h"
#include "strategy/values/LastMovementValue.h"
#include "Accounts/AccountMgr.h"
#include "Globals/ObjectMgr.h"
#include "Database/DatabaseEnv.h"
#include "PlayerbotAI.h"
#include "Entities/Player.h"
#include "Entities/Pet.h"
#include "playerbot/AiFactory.h"
#include "PlayerbotCommandServer.h"
#include "MemoryMonitor.h"

#include "Grids/GridNotifiers.h"
#include "Maps/GridDefines.h"
#include "MotionGenerators/MoveMap.h"
#include "MotionGenerators/PathFinder.h"

#ifdef MANGOSBOT_TWO
#include "Vmap/VMapFactory.h"
#include "Vmap/VMapManager2.h"
#else
#include "vmap/VMapFactory.h"
#include "vmap/VMapManager2.h"
#endif

#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "FleeManager.h"
#include "playerbot/ServerFacade.h"

#include "BattleGround/BattleGround.h"
#include "BattleGround/BattleGroundMgr.h"

#ifdef GenerateBotTests
#include "strategy/tests/TestRegistry.h"
#include "strategy/tests/TravelProbe.h"
#endif
#include "Chat/ChannelMgr.h"
#include "Guilds/GuildMgr.h"
#include "World/WorldState.h"
#include "PlayerbotLoginMgr.h"
#include "Entities/Transports.h"

#ifndef MANGOSBOT_ZERO
#ifdef CMANGOS
#include "Arena/ArenaTeam.h"
#endif
#ifdef MANGOS
#include "ArenaTeam.h"
#endif
#endif

#include "playerbot/TravelMgr.h"
#include <iomanip>
#include <float.h>

#if PLATFORM == PLATFORM_WINDOWS
#include "windows.h"
#include "psapi.h"
#endif

using namespace ai;
using namespace MaNGOS;

INSTANTIATE_SINGLETON_1(RandomPlayerbotMgr);

#ifdef CMANGOS
#include <boost/thread/thread.hpp>
#endif

#ifdef MANGOS
class PrintStatsThread: public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.PrintStats(); return 0; }
};
#endif
#ifdef CMANGOS
void PrintStatsThread(uint32 requesterGuid)
{
    sRandomPlayerbotMgr.PrintStats(requesterGuid);
}
#endif

void activatePrintStatsThread(uint32 requesterGuid)
{
#ifdef MANGOS
    PrintStatsThread *thread = new PrintStatsThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(PrintStatsThread, requesterGuid);
    t.detach();
#endif
}

#ifdef MANGOS
class CheckBgQueueThread : public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.CheckBgQueue(); return 0; }
};
#endif
#ifdef CMANGOS
void CheckBgQueueThread()
{
    sRandomPlayerbotMgr.CheckBgQueue();
}
#endif

void activateCheckBgQueueThread()
{
#ifdef MANGOS
    CheckBgQueueThread *thread = new CheckBgQueueThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(CheckBgQueueThread);
    t.detach();
#endif
}

#ifdef MANGOS
class CheckLfgQueueThread : public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.CheckLfgQueue(); return 0; }
};
#endif
#ifdef CMANGOS
void CheckLfgQueueThread()
{
    sRandomPlayerbotMgr.CheckLfgQueue();
}
#endif

void activateCheckLfgQueueThread()
{
#ifdef MANGOS
    CheckLfgQueueThread *thread = new CheckLfgQueueThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(CheckLfgQueueThread);
    t.detach();
#endif
}

#ifdef MANGOS
class CheckPlayersThread : public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { sRandomPlayerbotMgr.CheckPlayers(); return 0; }
};
#endif
#ifdef CMANGOS
void CheckPlayersThread()
{
    sRandomPlayerbotMgr.CheckPlayers();
}
#endif

void activateCheckPlayersThread()
{
#ifdef MANGOS
    CheckPlayersThread *thread = new CheckPlayersThread();
    thread->activate();
#endif
#ifdef CMANGOS
    boost::thread t(CheckPlayersThread);
    t.detach();
#endif
}

class botPIDImpl
{
public:
    botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd);
    ~botPIDImpl();
    double calculate(double setpoint, double pv);
    void adjust(double Kp, double Ki, double Kd) { _Kp = Kp; _Ki = Ki; _Kd = Kd; }
    void reset() { _integral = 0; }

private:
    double _dt;
    double _max;
    double _min;
    double _Kp;
    double _Ki;
    double _Kd;
    double _pre_error;
    double _integral;
};


botPID::botPID(double dt, double max, double min, double Kp, double Ki, double Kd)
{
    pimpl = new botPIDImpl(dt, max, min, Kp, Ki, Kd);
}
void botPID::adjust(double Kp, double Ki, double Kd)
{
    pimpl->adjust(Kp, Ki, Kd);
}
void botPID::reset()
{
    pimpl->reset();
}
double botPID::calculate(double setpoint, double pv)
{
    return pimpl->calculate(setpoint, pv);
}
botPID::~botPID()
{
    delete pimpl;
}


/**
 * Implementation
 */
botPIDImpl::botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd) :
    _dt(dt),
    _max(max),
    _min(min),
    _Kp(Kp),
    _Ki(Ki),
    _Kd(Kd),
    _pre_error(0),
    _integral(0)
{
}

double botPIDImpl::calculate(double setpoint, double pv)
{

    // Calculate error
    double error = setpoint - pv;

    // Proportional term
    double Pout = _Kp * error;

    // Integral term
    _integral += error * _dt;

    double Iout = _Ki * _integral;

    // Derivative term
    double derivative = (error - _pre_error) / _dt;
    double Dout = _Kd * derivative;

    // Calculate total output
    double output = Pout + Iout + Dout;

    // Restrict to max/min
    if (output > _max)
    {
        output = _max;
        _integral -= error * _dt; //Stop integral buildup at max
    }
    else if (output < _min)
    {
        output = _min;
        _integral -= error * _dt; //Stop integral buildup at min
    }

    // Save error to previous error
    _pre_error = error;

    return output;
}

botPIDImpl::~botPIDImpl()
{
}

RandomPlayerbotMgr::RandomPlayerbotMgr() 
: PlayerbotHolder()
, processTicks(0)
, loginProgressBar(NULL)
{
    if (sPlayerbotAIConfig.enabled && sPlayerbotAIConfig.randomBotAutologin)
    {
        sPlayerbotCommandServer.Start();
        PrepareTeleportCache();

        for (int i = BG_BRACKET_ID_FIRST; i < MAX_BATTLEGROUND_BRACKETS; ++i)
        {
            for (int j = BATTLEGROUND_QUEUE_AV; j < MAX_BATTLEGROUND_QUEUE_TYPES; ++j)
            {
                BgPlayers[j][i][0] = 0;
                BgPlayers[j][i][1] = 0;
                BgBots[j][i][0] = 0;
                BgBots[j][i][1] = 0;
                ArenaBots[j][i][0][0] = 0;
                ArenaBots[j][i][0][1] = 0;
                ArenaBots[j][i][1][0] = 0;
                ArenaBots[j][i][1][1] = 0;
                NeedBots[j][i][0] = false;
                NeedBots[j][i][1] = false;
            }
        }

        //1) Proportional: Amount activity is adjusted based on diff being above or below wanted diff. (100 wanted diff & 0.1 p = 150 diff = -5% activity)
        //2) Integral: Same as proportional but builds up each tick. (100 wanted diff & 0.01 i = 150 diff = -0.5% activity each tick)
        //3) Derative: Based on speed of diff. (+5 diff last tick & 0.05 d = -0.25% activity)
        pid.adjust(0.05,0.001,0.05);
        BgCheckTimer = 0;
        LfgCheckTimer = 0;
        PlayersCheckTimer = 0;
        EventTimeSyncTimer = 0;
        OfflineGroupBotsTimer = 0;
        guildsDeleted = false;
        arenaTeamsDeleted = false;

        std::list<uint32> availableBots = GetBots();

        for (auto& bot : availableBots)
        {
            if(GetEventValue(bot,"login"))
                SetEventValue(bot, "login", 0, 0);
        }

#ifndef MANGOSBOT_ZERO
        // load random bot team members
        auto results = CharacterDatabase.PQuery("SELECT guid FROM arena_team_member");
        if (results)
        {
            sLog.outString("Loading arena team bot members...");
            do
            {
                Field* fields = results->Fetch();
                uint32 lowguid = fields[0].GetUInt32();
                arenaTeamMembers.push_back(lowguid);
            } while (results->NextRow());
        }
#endif
        // sync event timers
        SyncEventTimers();

        for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
        {
            if (!sMapStore.LookupEntry(i))
                continue;

            uint32 mapId = sMapStore.LookupEntry(i)->MapID;
            facingFix[mapId] = {};
        }

        showLoginWarning = true;
    }
}

RandomPlayerbotMgr::~RandomPlayerbotMgr()
{
}

int RandomPlayerbotMgr::GetMaxAllowedBotCount()
{
    return GetEventValue(0, "bot_count");
}

inline void print_line(Unit* bot, const std::vector<std::pair<int, int>> line, bool is_sqDist_greater_200)
{
    std::ostringstream out;
    out << bot->GetName() << ",";
    out << std::fixed << std::setprecision(1);
    out << "\"LINESTRING(";
    for (auto& p : line)
    {
        out << p.first << " " << p.second << (&p == &line.back() ? "" : ",");
    }    
    out << ")\",";
    out << bot->GetOrientation() << ",";
    out << std::to_string(bot->getRace()) << ",";
    out << std::to_string(bot->getClass()) << ",";
    out << (is_sqDist_greater_200 ? "1" : "0");
    sPlayerbotAIConfig.log("player_paths.csv", out.str().c_str());
}

inline void print_path(Unit* bot, std::vector<std::pair<int, int>>& log)
{
    std::vector<std::pair<int, int>> line;

    std::pair<int, int> lastP = {0, 0};

    for (auto& p : log)
    {
        if (lastP.first && lastP.second && pow(lastP.first - p.first, 2) + pow(lastP.second - p.second, 2) > 200 * 200)
        {
            if (line.size()>1)
                print_line(bot, line, false);      //Print previous path.
            print_line(bot, {lastP, p}, true); //Print jump.
            line.clear();
        }
        line.push_back(p);
        lastP = p;
    }
    if (line.size() > 1)
        print_line(bot, line, false); //Print remaining path.
}

void RandomPlayerbotMgr::LogPlayerLocation()
{
    botCount = 0;
    activeBots = 0;
    if (sPlayerbotAIConfig.randomBotAutologin)
    {
        ForEachPlayerbot([&](Player* bot) {
            if (bot->GetPlayerbotAI())
            {

                botCount++;
                if (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY))
                {
                    activeBots++;
                }
            }
        });
    }

    for (auto i : GetPlayers())
    {
        Player* bot = i.second;
        if (!bot)
            continue;
        if (bot->GetPlayerbotAI())
        {
            botCount++;
            if (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY))
                activeBots++;
        }
    }

    if (sPlayerbotAIConfig.hasLog("player_location.csv"))
    {
        try
        {
            sPlayerbotAIConfig.openLog("player_location.csv", "w");

            if (sPlayerbotAIConfig.hasLog("player_route.csv"))
                sPlayerbotAIConfig.openLog("player_route.csv", "w");

            if (sPlayerbotAIConfig.randomBotAutologin)
            {
                ForEachPlayerbot([&](Player* bot) {
                    std::ostringstream out;
                    out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                    out << "RND" << ",";
                    out << bot->GetName() << ",";
                    out << std::fixed << std::setprecision(2);
                    WorldPosition(bot).printWKT(out);
                    out << bot->GetOrientation() << ",";
                    out << std::to_string(bot->getRace()) << ",";
                    out << std::to_string(bot->getClass()) << ",";
                    out << bot->GetMapId() << ",";
                    out << bot->GetLevel() << ",";
                    out << bot->GetHealth() << ",";
                    out << bot->GetPowerPercent() << ",";
                    out << bot->GetMoney() << ",";

                    if (bot->GetPlayerbotAI())
                    {
                        out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGrouperType())) << ",";
                        out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGuilderType())) << ",";
                        out << (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                        out << (bot->GetPlayerbotAI()->IsActive() ? "active" : "delay") << ",";
                        out << bot->GetPlayerbotAI()->HandleRemoteCommand("state") << ",";
                        PlayerbotAI* ai = bot->GetPlayerbotAI();
                        AiObjectContext* context = ai->GetAiObjectContext();

                        out << (AI_VALUE(bool, "should get money") ? "should get money" : "has enough money") << ",";

                        if (sPlayerbotAIConfig.hasLog("player_route.csv") && WorldPosition(bot))
                        {
                            LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");

                            std::vector<PathNodePoint> fullPath = lastMove.lastPath.getPath();

                            if (!fullPath.empty())
                            {
                                std::vector<std::pair<std::vector<WorldPosition>, bool>> splitPath;

                                bool currentWalkable = fullPath[0].isWalkable();
                                std::vector<WorldPosition> currentSegment;
                                currentSegment.push_back(fullPath[0].point);

                                for (size_t i = 1; i < fullPath.size(); i++)
                                {
                                    bool walkable = fullPath[i].isWalkable();

                                    if (walkable != currentWalkable)
                                    {
                                        // End current segment, start new one beginning with the last point
                                        splitPath.push_back({currentSegment, currentWalkable});
                                        currentSegment.clear();
                                        currentSegment.push_back(fullPath[i - 1].point); // shared junction point
                                        currentWalkable = walkable;
                                    }

                                    currentSegment.push_back(fullPath[i].point);
                                }

                                splitPath.push_back({currentSegment, currentWalkable});

                                uint32 segmentNr = 0;

                                for (auto& [segement, walkable] : splitPath)
                                {
                                    segmentNr++;
                                    std::ostringstream out;
                                    out << bot->GetName() << ",";
                                    out << std::fixed << std::setprecision(1);

                                    out << segmentNr << ",";

                                    WorldPosition().printWKT(segement, out, 1, false);

                                    out << bot->GetOrientation() << ",";
                                    out << std::to_string(bot->getRace()) << ",";
                                    out << std::to_string(bot->getClass()) << ",";
                                    out << (walkable ? "1" : "0") << ",";
                                    out << lastMove.moveEvent.getSource();
                                    sPlayerbotAIConfig.log("player_route.csv", out.str().c_str());
                                }
                            }
                        }
                    }
                    else
                    {
                        out << 0 << "," << 0 << ",err,err,err,err,";
                    }

                    out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                    out << (bot->IsDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive") << ",";

                    if (bot->GetGroup())
                        WorldPosition(bot).printWKT({bot, sObjectMgr.GetPlayer(bot->GetGroup()->GetLeaderGuid())}, out, 1);

                    sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());

                    PlayerbotAI* heartbeatAi = bot->GetPlayerbotAI();
                    if (sPlayerbotAIConfig.hasLog("bot_heartbeat.csv") && heartbeatAi && heartbeatAi->HasStrategy("debug heartbeat", BotState::BOT_STATE_NON_COMBAT))
                    {
                        std::ostringstream hb;
                        hb << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                        hb << bot->GetName() << ",";
                        hb << bot->GetLevel() << ",";
                        hb << bot->GetHealth() << ",";
                        hb << bot->GetPowerPercent() << ",";
                        hb << (bot->IsInCombat() ? "combat" : "non-combat") << ",";
                        WorldPosition(bot).printWKT(hb);

                        sPlayerbotAIConfig.log("bot_heartbeat.csv", hb.str().c_str());
                    }

                    if (sPlayerbotAIConfig.hasLog("player_paths.csv") && WorldPosition(bot))
                    {
                        auto& botMoveLog = playerBotMoveLog[bot->GetObjectGuid().GetCounter()];

                        std::pair<int32, int32> curDisplayPos = std::make_pair(WorldPosition(bot).getDisplayX(), WorldPosition(bot).getDisplayY());

                        botMoveLog.push_back(curDisplayPos);

                        if (botMoveLog.size() > 100)
                        {
                            print_path(bot, botMoveLog);
                            botMoveLog.clear();
                            botMoveLog.push_back(curDisplayPos); //Start next path at current position.
                        }
                    }
                });
            }

            for (auto i : GetPlayers())
            {
                Player* bot = i.second;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "PLR" << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPercent() << ",";
                out << bot->GetMoney() << ",";
                if (bot->GetPlayerbotAI())
                {
                    out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGrouperType())) << ",";
                    out << std::to_string(uint8(bot->GetPlayerbotAI()->GetGuilderType())) << ",";
                    out << (bot->GetPlayerbotAI()->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (bot->GetPlayerbotAI()->IsActive() ? "active" : "delay") << ",";
                    out << bot->GetPlayerbotAI()->HandleRemoteCommand("state") << ",";
                    PlayerbotAI* ai = bot->GetPlayerbotAI();
                    AiObjectContext* context = ai->GetAiObjectContext();

                    out << (AI_VALUE(bool, "should get money") ? "should get money" : "has enough money") << ",";
                }
                else
                {
                    out << 0 << "," << 0 << ",player,player,player,player,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->IsDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive") << ",";

                if (bot->GetGroup())
                    WorldPosition(bot).printWKT({bot, sObjectMgr.GetPlayer(bot->GetGroup()->GetLeaderGuid())}, out, 1);

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());

                if (sPlayerbotAIConfig.hasLog("player_paths.csv") && WorldPosition(bot))
                {
                    auto& botMoveLog = playerBotMoveLog[bot->GetObjectGuid().GetCounter()];

                    std::pair<int32, int32> curDisplayPos = std::make_pair(WorldPosition(bot).getDisplayX(), WorldPosition(bot).getDisplayY());

                    botMoveLog.push_back(curDisplayPos);

                    if (botMoveLog.size() > 100)
                    {
                        print_path(bot, botMoveLog);
                        botMoveLog.clear();
                        botMoveLog.push_back(curDisplayPos); //Start next path at current position.
                    }
                }
            }
        }
        catch (...)
        {
            return;
            //This is to prevent some thread-unsafeness. Crashes would happen if bots get added or removed.
            //We really don't care here. Just skip a log. Making this thread-safe is not worth the effort.
        }
    }
    if (sPlayerbotAIConfig.hasLog("transport.csv"))
    {
        sPlayerbotAIConfig.openLog("transport.csv", "w");
        for (auto& [mapId, map] : sMapMgr.Maps())
        {
            for (auto& transport : WorldPosition(map->GetId(), 1, 1).getTransports())
            {
                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                if (transport->GetName() == nullptr || transport->GetName()[0] == '\0')
                {
                    GameObjectInfo const* data = sGOStorage.LookupEntry<GameObjectInfo>(transport->GetEntry());
                    out << data->name << ",";
                }
                else
                    out << transport->GetName() << ",";

                out << transport->GetEntry() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(transport).printWKT(out);
                out << transport->GetOrientation();

                sPlayerbotAIConfig.log("transport.csv", out.str().c_str());
            }
        }
    }
}

// Silvermoon patch: number of guilds with at least one member online, per faction
// (0 = Alliance, 1 = Horde). Counted once a minute in the world thread and read by
// BuyPetitionAction::canBuyPetition to decide how eagerly bots found new guilds.
std::atomic<uint32> g_silvermoonOnlineGuilds[2] = { {0}, {0} };

// Silvermoon patch: GM spectating. Maps a GM's guid to the player or bot it watches.
static std::map<ObjectGuid, ObjectGuid> g_silvermoonSpectate;
// defined in the core (LootMgr.cpp): GMs who get in-game loot messages for dungeon runs
extern std::map<uint32, uint32> g_silvermoonLootWatch;
extern std::mutex g_silvermoonLootWatchLock;

// Silvermoon patch: the highest-level random bot of each class on each faction. They are
// kept online (logout postponed, logged back in if they drop) so the realm's leaders
// keep progressing. Refreshed every 10 minutes.
static std::set<uint32> g_silvermoonPriorityBots;

// Silvermoon patch: dungeons ".rndbot dungeon" can send a party into: level range, the
// entrance inside the instance and the final boss whose death ends the run.
struct SilvermoonDungeon { uint32 map, minLevel, maxLevel; float x, y, z, o; uint32 finalBoss; std::vector<uint32> bosses; };
static const std::map<std::string, SilvermoonDungeon> g_silvermoonDungeons = {
    // Ragefire Chasm: Oggleflint, Jergosh, Bazzalan, Taragaman (final)
    {"rfc", {389, 13, 18, 0.80f, -8.23f, -15.53f, 4.71f, 11520, {11517, 11518, 11519, 11520}}},
    // Wailing Caverns, in route order: Anacondra, Pythas, Cobrahn, Skum, Serpentis, Verdan (final;
    // Mutanus needs the escort event)
    {"wc",  {43, 17, 24, -158.44f, 131.60f, -74.26f, 5.85f, 5775, {3671, 3670, 3669, 3674, 3673, 5775}}},
    // Deadmines: Rhahk'Zor, Sneed's Shredder, Gilnid, Mr. Smite, Greenskin, Cookie, VanCleef (final)
    {"dm",  {36, 17, 26, -14.57f, -385.48f, 62.46f, 1.57f, 639, {644, 642, 1763, 646, 647, 645, 639}}},
    // Silvermoon patch: every other classic 5-player dungeon (entrances and boss spawns from the world database)
    // Shadowfang Keep: Rethilgore, Razorclaw the Butcher, Baron Silverlaine, Commander Springvale, Odo the Blindwatcher (not spawned: skipped on the route), Fenrus the Devourer, Wolf Master Nandos, Archmage Arugal; final: Archmage Arugal
    {"sfk", {33, 22, 30, -228.19f, 2111.41f, 76.89f, 1.22f, 4275, {3914, 3886, 3887, 4278, 4279, 4274, 3927, 4275}}},
    // The Stockade: Targorr the Dread, Kam Deepfury, Hamhock, Dextren Ward, Bazil Thredd; final: Bazil Thredd
    {"stocks", {34, 24, 32, 48.98f, 0.48f, -16.39f, 6.28f, 1716, {1696, 1666, 1717, 1663, 1716}}},
    // Blackfathom Deeps: Ghamoo-ra, Lady Sarevess, Gelihast, Lorgus Jett, Baron Aquanis (not spawned: skipped on the route), Twilight Lord Kelris, Old Serra'kis, Aku'mai; final: Aku'mai
    {"bfd", {48, 24, 32, -150.23f, 106.59f, -39.78f, 4.45f, 4829, {4887, 4831, 6243, 12902, 12876, 4832, 4830, 4829}}},
    // Gnomeregan: Grubbis (not spawned: skipped on the route), Viscous Fallout, Electrocutioner 6000, Crowd Pummeler 9-60, Mekgineer Thermaplugg; final: Mekgineer Thermaplugg
    {"gnomer", {90, 29, 38, -329.10f, -3.21f, -152.85f, 2.97f, 7800, {7361, 7079, 6235, 6229, 7800}}},
    // Razorfen Kraul: Roogug, Aggem Thorncurse, Death Speaker Jargba, Overlord Ramtusk, Agathelos the Raging, Charlga Razorflank; final: Charlga Razorflank
    {"rfk", {47, 29, 38, 1942.27f, 1544.23f, 83.31f, 1.31f, 4421, {6168, 4424, 4428, 4420, 4422, 4421}}},
    // Scarlet Monastery Graveyard: Interrogator Vishas, Bloodmage Thalnos; final: Bloodmage Thalnos
    {"smgy", {189, 28, 36, 1687.27f, 1050.09f, 18.68f, 1.57f, 4543, {3983, 4543}}},
    // Scarlet Monastery Library: Houndmaster Loksey, Arcanist Doan; final: Arcanist Doan
    {"smlib", {189, 31, 39, 253.67f, -206.62f, 18.68f, 4.71f, 6487, {3974, 6487}}},
    // Scarlet Monastery Armory: Herod; final: Herod
    {"smarm", {189, 33, 41, 1608.10f, -318.92f, 18.67f, 4.71f, 3975, {3975}}},
    // Scarlet Monastery Cathedral: High Inquisitor Fairbanks, Scarlet Commander Mograine, High Inquisitor Whitemane; final: High Inquisitor Whitemane
    {"smcath", {189, 36, 44, 853.18f, 1319.18f, 18.67f, 1.57f, 3977, {4542, 3976, 3977}}},
    // Razorfen Downs: Tuten'kash (not spawned: skipped on the route), Mordresh Fire Eye, Glutton, Amnennar the Coldbringer; final: Amnennar the Coldbringer
    {"rfd", {129, 37, 46, 2593.68f, 1111.23f, 50.95f, 4.71f, 7358, {7355, 7357, 8567, 7358}}},
    // Uldaman: Revelosh (not spawned: skipped on the route), Ironaya (not spawned: skipped on the route), Ancient Stone Keeper, Galgann Firehammer, Grimlok, Archaedas; final: Archaedas
    {"ulda", {70, 41, 51, -228.86f, 46.10f, -46.02f, 1.57f, 2748, {6910, 7228, 7206, 7291, 4854, 2748}}},
    // Zul'Farrak: Antu'sul, Witch Doctor Zum'rah, Hydromancer Velratha, Theka the Martyr, Nekrum Gutchewer (not spawned: skipped on the route), Shadowpriest Sezz'ziz (not spawned: skipped on the route), Chief Ukorz Sandscalp; final: Chief Ukorz Sandscalp
    {"zf", {209, 44, 54, 1212.67f, 842.04f, 8.93f, 6.28f, 7267, {8127, 7271, 7795, 7272, 7796, 7275, 7267}}},
    // Maraudon: Noxxion, Razorlash, Lord Vyletongue, Celebras the Cursed, Landslide, Tinkerer Gizlock, Rotgrip, Princess Theradras; final: Princess Theradras
    {"mara", {349, 46, 55, 1016.83f, -458.52f, -43.47f, 6.28f, 12201, {13282, 12258, 12236, 12225, 12203, 13601, 13596, 12201}}},
    // Sunken Temple: Dreamscythe (not spawned: skipped on the route), Weaver (not spawned: skipped on the route), Jammal'an the Prophet, Morphaz, Hazzas, Shade of Eranikus; final: Shade of Eranikus
    {"st", {109, 50, 60, -315.90f, 100.20f, -131.85f, 3.14f, 5709, {5721, 5720, 5710, 5719, 5722, 5709}}},
    // Blackrock Depths: Lord Roccor, High Interrogator Gerstahn, Houndmaster Grebmar, Pyromancer Loregrain, Lord Incendius, Fineous Darkvire, Bael'Gar, General Angerforge, Golem Lord Argelmach, Phalanx, Ambassador Flamelash, Magmus, Emperor Dagran Thaurissan; final: Emperor Dagran Thaurissan
    {"brd", {230, 52, 60, 456.93f, 34.09f, -68.09f, 4.71f, 9019, {9025, 9018, 9319, 9024, 9017, 9056, 9016, 9033, 8983, 9502, 9156, 9938, 9019}}},
    // Lower Blackrock Spire: Highlord Omokk, Shadow Hunter Vosh'gajin, War Master Voone, Mother Smolderweb, Quartermaster Zigris, Halycon, Gizrul the Slavener (not spawned: skipped on the route), Overlord Wyrmthalak; final: Overlord Wyrmthalak
    {"lbrs", {229, 55, 60, 78.35f, -226.84f, 49.77f, 4.71f, 9568, {9196, 9236, 9237, 10596, 9736, 10220, 10268, 9568}}},
    // Scholomance: Jandice Barov, Rattlegore, Marduk Blackpool, Vectus, Ras Frostwhisper, Instructor Malicia, Doctor Theolen Krastinov, Lorekeeper Polkelt, The Ravenian, Lord Alexei Barov, Lady Illucia Barov, Darkmaster Gandling (not spawned: skipped on the route); final: Darkmaster Gandling
    {"scholo", {289, 58, 60, 190.82f, 126.33f, 137.23f, 6.28f, 1853, {10503, 11622, 10433, 10432, 10508, 10505, 11261, 10901, 10507, 10504, 10502, 1853}}},
    // Stratholme (Live): The Unforgiven, Timmy the Cruel (not spawned: skipped on the route), Malor the Zealous, Cannon Master Willey, Archivist Galford, Grand Crusader Dathrohan, Balnazzar (not spawned: skipped on the route); final: Balnazzar
    {"stratlive", {329, 58, 60, 3392.84f, -3364.44f, 142.97f, 4.71f, 10813, {10516, 10808, 11032, 10997, 10811, 10812, 10813}}},
    // Stratholme (Undead): Magistrate Barthilas, Nerub'enkan, Baroness Anastari, Maleki the Pallid, Ramstein the Gorger (not spawned: skipped on the route), Baron Rivendare; final: Baron Rivendare
    {"stratud", {329, 58, 60, 3590.87f, -3643.22f, 138.49f, 5.50f, 10440, {10435, 10437, 10436, 10438, 10439, 10440}}},
    // Dire Maul East: Zevrim Thornhoof, Hydrospawn, Lethtendris, Alzzin the Wildshaper; final: Alzzin the Wildshaper
    {"dme", {429, 55, 60, 47.45f, -153.66f, -2.71f, 5.50f, 11492, {11490, 13280, 14327, 11492}}},
    // Dire Maul West: Tendris Warpwood, Illyanna Ravenoak, Magister Kalendris, Immol'thar, Prince Tortheldrin; final: Prince Tortheldrin
    {"dmw", {429, 57, 60, -65.66f, 159.56f, -3.46f, 2.36f, 11486, {11489, 11488, 11487, 11496, 11486}}},
    // Dire Maul North: Guard Mol'dar, Stomper Kreeg, Guard Fengus, Guard Slip'kik, Captain Kromcrush, King Gordok; final: King Gordok
    {"dmn", {429, 57, 60, 254.92f, -19.46f, -2.56f, 5.50f, 11501, {14326, 14322, 14321, 14323, 14325, 11501}}},
};

// Doors near a committed bot-only party are opened for them (the Deadmines' Iron Clad Door
// needs the cannon event, which bots cannot run).
class SilvermoonDoorCheck
{
public:
    SilvermoonDoorCheck(WorldObject const* obj, float range) : i_obj(obj), i_range(range) {}
    WorldObject const& GetFocusObject() const { return *i_obj; }
    bool operator()(GameObject* go)
    {
        return go && go->GetGoType() == GAMEOBJECT_TYPE_DOOR && go->GetGoState() == GO_STATE_READY &&
            i_obj->IsWithinDistInMap(go, i_range);
    }
private:
    WorldObject const* i_obj;
    float i_range;
};


// Silvermoon patch: level of each faction's 10th highest online random bot
// (0 = Alliance, 1 = Horde). Bots at or above it count as the faction's
// front-runners and get checked for a level-appropriate zone more often.
std::atomic<uint32> g_silvermoonTopLevel[2] = { {255}, {255} };

// Silvermoon patch: one-time gear help for random bots, limited to what a player could
// buy: white (common) items sold by some vendor, usable at the bot's level, in weapon
// types the bot has actually trained. A slot is only replaced when the vendor item is
// better. Unobtainable items (Gamemaster attire, test items) are removed first.
// Silvermoon patch: supplies for a dungeon run. Mana users without water spent 3-5 minutes
// after every pull regenerating. Each party member is given, free, level-appropriate water
// up to 10 (mana users) and food up to 5 - when the party forms and again whenever it runs
// low during the run.
static void SilvermoonStockUp(Player* bot)
{
    uint32 lvl = bot->GetLevel();
    uint32 water = lvl >= 25 ? 1708 : lvl >= 15 ? 1205 : lvl >= 5 ? 1179 : 159;   // Sweet Nectar, Melon Juice, Ice Cold Milk, Refreshing Spring Water
    uint32 food = lvl >= 25 ? 4544 : lvl >= 15 ? 4542 : lvl >= 5 ? 4541 : 4540;    // Mulgore Spice Bread, Moist Cornbread, Freshly Baked Bread, Tough Hunk of Bread
    std::vector<uint32> wanted = { food };
    if (bot->GetPowerType() == POWER_MANA)
        wanted.insert(wanted.begin(), water);
    for (uint32 itemId : wanted)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto)
            continue;
        uint32 target = itemId == water ? 10 : 5;
        uint32 have = bot->GetItemCount(itemId);
        if (have >= target)
            continue;
        uint32 count = target - have;
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK)
            continue;
        bot->StoreNewItem(dest, itemId, true); // free (S's call): dungeon supplies are on the house
        sLog.outBasic("Silvermoon: bot %s was given %u x %s for its dungeon run", bot->GetName(), count, proto->Name1);
    }
}

// Silvermoon patch: fell through the floor in any instance (dungeon or raid), for every bot -
// including ones grouped with a player and ones not on a dungeon run (those have their own
// check in the dungeon watchdog). Every 2 seconds: a living bot with no ground under it, or
// ground more than 15 yards below, is put back where it last stood on solid ground - or,
// without one, next to its group's leader or the instance entrance.
static void SilvermoonGroundGuard(Player* p, RandomPlayerbotMgr& mgr)
{
    static std::map<uint32, std::array<float, 4>> lastSolid; // guid -> x, y, z, map
    if (!p || !p->IsInWorld() || !p->GetPlayerbotAI() || !p->GetMap() || !p->GetMap()->Instanceable() ||
        p->IsBeingTeleported() || p->IsTaxiFlying() || mgr.SilvermoonDungeonOf(p->GetGUIDLow()))
        return;
    if (!p->IsAlive() || p->IsFalling())
        return;
    float ground = p->GetMap()->GetHeight(p->GetPositionX(), p->GetPositionY(), p->GetPositionZ() + 2.0f);
    bool solid = ground > INVALID_HEIGHT;
    uint32 guid = p->GetGUIDLow();
    if (solid)
    {
        lastSolid[guid] = { p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), float(p->GetMapId()) };
        return;
    }
    auto it = lastSolid.find(guid);
    if (it != lastSolid.end() && it->second[3] == float(p->GetMapId()))
    {
        sLog.outBasic("Silvermoon: bot %s fell through the floor of map %u, back to solid ground", p->GetName(), p->GetMapId());
        p->NearTeleportTo(it->second[0], it->second[1], it->second[2], p->GetOrientation());
        return;
    }
    if (Group* g = p->GetGroup())
        if (Player* leader = sObjectMgr.GetPlayer(g->GetLeaderGuid()))
            if (leader != p && leader->GetMap() == p->GetMap())
            {
                sLog.outBasic("Silvermoon: bot %s fell through the floor of map %u, back to %s", p->GetName(), p->GetMapId(), leader->GetName());
                p->NearTeleportTo(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), leader->GetOrientation());
                return;
            }
    if (auto result = WorldDatabase.PQuery("SELECT `target_position_x`, `target_position_y`, `target_position_z`, `target_orientation` FROM `areatrigger_teleport` WHERE `target_map` = '%u' LIMIT 1", p->GetMapId()))
    {
        Field* f = result->Fetch();
        sLog.outBasic("Silvermoon: bot %s fell through the floor of map %u, back to the entrance", p->GetName(), p->GetMapId());
        p->NearTeleportTo(f[0].GetFloat(), f[1].GetFloat(), f[2].GetFloat(), f[3].GetFloat());
    }
}

// Silvermoon patch: run from fights the bot cannot win (see the comment where it is called).
static void SilvermoonDisengage(Player* p, RandomPlayerbotMgr& mgr)
{
    if (!p || !p->IsInWorld() || !p->IsAlive() || !p->IsInCombat() || p->IsBeingTeleported() || !p->GetPlayerbotAI() ||
        !mgr.IsRandomBot(p) || p->GetPlayerbotAI()->HasRealPlayerMaster() || mgr.SilvermoonDungeonOf(p->GetGUIDLow()) || !p->GetMap() || !p->GetMap()->IsContinent())
        return;
    if (Group* g = p->GetGroup())
        if (g->GetMembersCount() > 1)
            return; // a party fights together
    uint32 attackers = 0;
    int32 topLevel = 0;
    for (Unit* u : p->getAttackers())
    {
        if (u && u->IsAlive())
        {
            ++attackers;
            topLevel = std::max(topLevel, int32(u->GetLevel()));
        }
    }
    float hp = p->GetHealthPercent();
    bool bad = attackers >= 3 || (attackers >= 2 && hp < 35.0f) || (topLevel >= int32(p->GetLevel()) + 4 && hp < 50.0f);
    if (!bad)
        return;
    static std::map<uint32, time_t> lastFlee;
    if (time(0) - lastFlee[p->GetGUIDLow()] < 6)
        return;
    lastFlee[p->GetGUIDLow()] = time(0);
    p->GetPlayerbotAI()->DoSpecificAction("flee", Event(), true);
}

// Silvermoon patch: profession pairs that make sense. A crafting profession that needs a
// gathering one (Blacksmithing or Engineering - Mining, Leatherworking - Skinning, Alchemy -
// Herbalism) without it is a dead end: both primary professions are dropped and the bot picks
// a fitting pair right away (it is in town), paying the 10 silver apprentice fee for each.
static void SilvermoonFixProfessions(Player* bot)
{
    enum { MIN = 186, HERB = 182, SKIN = 393, BS = 164, LW = 165, ALCH = 171, TAIL = 197, ENG = 202, ENCH = 333 };
    static const std::map<uint32, uint32> needs = { { BS, MIN }, { ENG, MIN }, { LW, SKIN }, { ALCH, HERB } };
    static const std::vector<uint32> primaries = { MIN, HERB, SKIN, BS, LW, ALCH, TAIL, ENG, ENCH };
    bool broken = false;
    for (auto const& n : needs)
        if (bot->HasSkill(n.first) && !bot->HasSkill(n.second))
            broken = true;
    if (!broken)
        return;
    // apprentice spells: profession skill -> spell that teaches it
    static const std::map<uint32, uint32> apprentice = { { MIN, 2575 }, { HERB, 2366 }, { SKIN, 8613 }, { BS, 2018 }, { LW, 2108 },
        { ALCH, 2259 }, { TAIL, 3908 }, { ENG, 4036 }, { ENCH, 7411 } };
    static const std::vector<std::pair<uint32, uint32>> pairs = { { BS, MIN }, { ENG, MIN }, { LW, SKIN }, { ALCH, HERB },
        { TAIL, ENCH }, { MIN, HERB }, { SKIN, HERB }, { MIN, SKIN } };
    if (bot->GetMoney() < 2000)
        return; // try again when it can afford both apprentice fees
    std::string dropped;
    for (uint32 skill : primaries)
        if (bot->HasSkill(skill))
        {
            bot->SetSkill(skill, 0, 0);
            dropped += (dropped.empty() ? "" : ", ") + std::to_string(skill);
        }
    auto const& pick = pairs[urand(0, pairs.size() - 1)];
    for (uint32 skill : { pick.first, pick.second })
    {
        bot->learnSpell(apprentice.at(skill), false);
        bot->ModifyMoney(-1000);
    }
    sLog.outBasic("Silvermoon: bot %s dropped mismatched professions (%s) and learned %u + %u", bot->GetName(), dropped.c_str(), pick.first, pick.second);
}

// Silvermoon patch: repairs. 42 online bots had gear at 0 durability (useless) although most
// could afford a repair; the trip to a repair npc is where they got lost. A bot resting in a
// town repairs at any friendly repair npc within 60 yards, paying the normal price (weapons
// first, then the rest, as far as its money goes). Returns true when it repaired something.
static bool SilvermoonRepairNearby(Player* bot)
{
    bool damaged = false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && !damaged; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && item->GetUInt32Value(ITEM_FIELD_DURABILITY) < item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                damaged = true;
    if (!damaged || !bot->GetMoney())
        return false;
    std::list<Unit*> near;
    MaNGOS::AnyUnitInObjectRangeCheck check(bot, 60.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
    Cell::VisitAllObjects(bot, searcher, 60.0f);
    for (Unit* u : near)
    {
        if (u->GetTypeId() != TYPEID_UNIT || !u->IsAlive() || !u->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR) || bot->CanAttack(u))
            continue;
        float discount = bot->GetReputationPriceDiscount((Creature*)u);
        uint32 before = bot->GetMoney();
        for (uint8 slot : { uint8(EQUIPMENT_SLOT_MAINHAND), uint8(EQUIPMENT_SLOT_RANGED), uint8(EQUIPMENT_SLOT_OFFHAND) })
            bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | slot, true, discount);
        bot->DurabilityRepairAll(true, discount);
        if (bot->GetMoney() < before)
        {
            sLog.outBasic("Silvermoon: bot %s repaired its gear at %s for %u copper", bot->GetName(), u->GetName(), before - bot->GetMoney());
            return true;
        }
        return false;
    }
    return false;
}

// Silvermoon patch: class training in passing. A bot resting in a town with its class trainer
// within 60 yards learns what it can afford right there (damage spells first, see
// TrainerAction), instead of relying only on a dedicated trip to the trainer.
static void SilvermoonTrainNearby(Player* bot)
{
    if (!bot->GetPlayerbotAI() || !bot->GetMoney())
        return;
    std::list<Unit*> near;
    MaNGOS::AnyUnitInObjectRangeCheck check(bot, 60.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
    Cell::VisitAllObjects(bot, searcher, 60.0f);
    for (Unit* u : near)
    {
        if (u->GetTypeId() != TYPEID_UNIT || !u->IsAlive() || bot->CanAttack(u))
            continue;
        Creature* c = (Creature*)u;
        if (!c->isTrainer() || !c->GetCreatureInfo() || c->GetCreatureInfo()->TrainerType != TRAINER_TYPE_CLASS || !c->IsTrainerOf(bot, false))
            continue;
        Event event("rpg action", c->GetObjectGuid());
        bot->GetPlayerbotAI()->DoSpecificAction("trainer", event, true);
        return;
    }
}

// Silvermoon patch: professions without economy creep. Called for a bot that is resting in a
// town or city. For every recipe it knows that would still raise its skill, or that makes
// gear it would equip:
//  - with the materials already in its bags it crafts (the materials are used up), a few
//    items per visit;
//  - otherwise, in a capital city, it may buy the missing materials on the auction house, but
//    only when they are cheap for it: at most 3% of its money for a skill-up, at most 20% for
//    an item it would wear. The materials arrive by mail and are used on a later visit.
// Returns true when it bought something (so the caller can make it wait before buying again).
static bool SilvermoonCraftPass(Player* bot, bool allowBuying)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;
    AiObjectContext* context = ai->GetAiObjectContext();
    std::vector<uint32> spells = context->GetValue<std::vector<uint32>>("craft spells")->Get();
    static const std::set<uint32> capitals = { 1519, 1537, 1657, 1637, 1638, 1497 };
    bool inCapital = capitals.count(bot->GetZoneId()) > 0;
    uint32 crafted = 0;
    bool bought = false;

    // New recipes and profession ranks from the trainer, in a capital city: only for professions
    // the bot already has, only what its skill and level allow, and only when one costs less
    // than 5% of the money it has right now. At most 3 per visit; the bot pays the full price.
    if (inCapital)
    {
        struct TradeSpell { uint32 skill, skillValue, level, cost, spell; };
        static std::vector<TradeSpell> tradeSpells;
        static bool tradeSpellsLoaded = false;
        if (!tradeSpellsLoaded)
        {
            tradeSpellsLoaded = true;
            std::map<uint32, TradeSpell> bySpell;
            for (uint32 id = 0; id < sCreatureStorage.GetMaxEntry(); ++id)
            {
                CreatureInfo const* co = sCreatureStorage.LookupEntry<CreatureInfo>(id);
                if (!co || co->TrainerType != TRAINER_TYPE_TRADESKILLS)
                    continue;
                uint32 trainerId = co->TrainerTemplateId ? co->TrainerTemplateId : co->Entry;
                TrainerSpellData const* trainerSpells = sObjectMgr.GetNpcTrainerTemplateSpells(trainerId);
                if (!trainerSpells)
                    trainerSpells = sObjectMgr.GetNpcTrainerSpells(trainerId);
                if (!trainerSpells)
                    continue;
                for (auto const& itr : trainerSpells->spellList)
                {
                    TrainerSpell const& tSpell = itr.second;
                    if (!tSpell.reqSkill || !tSpell.reqSkillValue)
                        continue; // learning a profession from scratch is not done here
                    uint32 learned = tSpell.learnedSpell;
                    if (SpellEntry const* teach = sServerFacade.LookupSpellInfo(tSpell.spell))
                        for (int j = 0; j < 3; ++j)
                            if (teach->Effect[j] == SPELL_EFFECT_LEARN_SPELL && teach->EffectTriggerSpell[j])
                                learned = teach->EffectTriggerSpell[j];
                    if (!learned)
                        continue;
                    auto found = bySpell.find(learned);
                    if (found == bySpell.end() || tSpell.spellCost < found->second.cost)
                        bySpell[learned] = { tSpell.reqSkill, tSpell.reqSkillValue, tSpell.reqLevel, tSpell.spellCost, learned };
                }
            }
            for (auto const& entry : bySpell)
                tradeSpells.push_back(entry.second);
            std::sort(tradeSpells.begin(), tradeSpells.end(), [](TradeSpell const& a, TradeSpell const& b) { return a.skillValue < b.skillValue; });
        }
        uint32 learnedNow = 0;
        for (TradeSpell const& t : tradeSpells)
        {
            if (learnedNow >= 3)
                break;
            if (!bot->HasSkill(t.skill) || bot->GetSkillValuePure(t.skill) < t.skillValue || bot->GetLevel() < t.level || bot->HasSpell(t.spell))
                continue;
            if (uint64(t.cost) * 20 >= bot->GetMoney()) // must cost less than 5% of its money
                continue;
            bot->ModifyMoney(-int32(t.cost));
            bot->learnSpell(t.spell, false);
            ++learnedNow;
            SpellEntry const* info = sServerFacade.LookupSpellInfo(t.spell);
            sLog.outBasic("Silvermoon: bot %s learned %s from a profession trainer for %u copper", bot->GetName(), info ? info->SpellName[0] : "a recipe", t.cost);
        }
        if (learnedNow)
            spells = context->GetValue<std::vector<uint32>>("craft spells")->Get();
    }

    for (uint32 spellId : spells)
    {
        if (crafted >= 5)
            break;
        SpellEntry const* spell = sServerFacade.LookupSpellInfo(spellId);
        if (!spell || spell->Effect[0] != SPELL_EFFECT_CREATE_ITEM || !spell->EffectItemType[0])
            continue;
        uint32 itemId = spell->EffectItemType[0];
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto)
            continue;
        bool skillUp = ShouldCraftSpellValue::SpellGivesSkillUp(spellId, bot);
        bool upgrade = proto->InventoryType != INVTYPE_NON_EQUIP && !bot->HasItemCount(itemId, 1) &&
            context->GetValue<ItemUsage>("item usage", std::to_string(itemId))->Get() == ItemUsage::ITEM_USAGE_EQUIP;
        bool forSale = RandomPlayerbotMgr::SilvermoonIsCrafter(bot->GetGUIDLow()) && proto->Quality >= ITEM_QUALITY_UNCOMMON &&
            proto->InventoryType != INVTYPE_NON_EQUIP && bot->GetItemCount(itemId) < 2;
        if (!skillUp && !upgrade && !forSale)
            continue;

        std::map<uint32, uint32> missing; // reagent -> count still needed
        bool hasReagents = false;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i])
                continue;
            hasReagents = true;
            uint32 have = bot->GetItemCount(uint32(spell->Reagent[i]));
            if (have < spell->ReagentCount[i])
                missing[uint32(spell->Reagent[i])] = spell->ReagentCount[i] - have;
        }
        if (!hasReagents)
            continue;

        if (missing.empty())
        {
            int32 made = spell->CalculateSimpleValue(EFFECT_INDEX_0);
            uint32 count = uint32(std::max(1, std::min(made, 20)));
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK)
                continue;
            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (spell->Reagent[i] > 0 && spell->ReagentCount[i])
                    bot->DestroyItemCount(uint32(spell->Reagent[i]), spell->ReagentCount[i], true);
            bot->StoreNewItem(dest, itemId, true, Item::GenerateItemRandomPropertyId(itemId));
            bot->UpdateCraftSkill(spellId);
            ++crafted;
            sLog.outBasic("Silvermoon: bot %s crafted %s (%s)", bot->GetName(), proto->Name1, upgrade ? "gear for itself" : "skill-up");
            continue;
        }

        if (!allowBuying || !inCapital || bought)
            continue;
        AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntry(bot);
        AuctionHouseObject* house = houseEntry ? sAuctionMgr.GetAuctionsMap(houseEntry) : nullptr;
        if (!house)
            continue;
        uint32 budget = uint32(bot->GetMoney() * (upgrade ? 0.20f : 0.03f));
        if (!budget)
            continue;
        // cheapest buyouts first, per missing material
        std::vector<AuctionEntry*> picks;
        uint32 total = 0;
        bool covered = true;
        for (auto const& need : missing)
        {
            std::vector<AuctionEntry*> offers;
            for (auto const& entry : house->GetAuctions())
            {
                AuctionEntry* a = entry.second;
                if (a && a->itemTemplate == need.first && a->buyout && a->itemCount && a->owner != bot->GetGUIDLow() && a->bidder != bot->GetGUIDLow())
                    offers.push_back(a);
            }
            std::sort(offers.begin(), offers.end(), [](AuctionEntry* x, AuctionEntry* y)
            {
                return uint64(x->buyout) * y->itemCount < uint64(y->buyout) * x->itemCount;
            });
            uint32 got = 0;
            for (AuctionEntry* a : offers)
            {
                if (got >= need.second)
                    break;
                picks.push_back(a);
                total += a->buyout;
                got += a->itemCount;
            }
            if (got < need.second)
                covered = false;
        }
        if (!covered || total > budget || total > bot->GetMoney())
            continue;
        for (AuctionEntry* a : picks)
            a->UpdateBid(a->buyout, bot); // buys it out; the auction entry is gone afterwards
        bought = true;
        sLog.outBasic("Silvermoon: bot %s bought materials for %s on the auction house for %u copper (%s, budget %u)",
            bot->GetName(), proto->Name1, total, upgrade ? "gear for itself" : "skill-up", budget);
    }
    return bought;
}

static void SilvermoonVendorGear(Player* bot)
{
    static std::vector<uint32> vendorItems;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        auto result = WorldDatabase.Query("SELECT DISTINCT `item` FROM `npc_vendor` UNION SELECT DISTINCT `item` FROM `npc_vendor_template`");
        if (result)
        {
            do
            {
                uint32 id = result->Fetch()[0].GetUInt32();
                ItemPrototype const* proto = sObjectMgr.GetItemPrototype(id);
                if (proto && proto->Quality == ITEM_QUALITY_NORMAL && proto->InventoryType != INVTYPE_NON_EQUIP &&
                    proto->InventoryType != INVTYPE_BAG && proto->InventoryType != INVTYPE_AMMO && proto->InventoryType != INVTYPE_QUIVER)
                    vendorItems.push_back(id);
            } while (result->NextRow());
        }
        sLog.outBasic("Silvermoon: %u white vendor items available for bot gear", uint32(vendorItems.size()));
    }

    static const std::set<uint32> unobtainable = { 2586, 11508, 12064, 5417, 5418, 19160 };
    for (uint8 slot = 0; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (unobtainable.count(item->GetEntry()))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                if (Item* item = bag->GetItemByPos(j))
                    if (unobtainable.count(item->GetEntry()))
                        bot->DestroyItem(bagSlot, j, true);

    static const std::map<uint32, uint32> weaponSkill = {
        {0, 44}, {1, 172}, {2, 45}, {3, 46}, {4, 54}, {5, 160}, {6, 229}, {7, 43}, {8, 55},
        {10, 136}, {13, 473}, {15, 173}, {16, 176}, {18, 226}, {19, 228} };

    // warriors and rogues: an orb/tome/lantern in the off hand is unequipped (moved to the bags,
    // or destroyed if the bags are full) so a weapon or shield can take the slot
    if (bot->getClass() == CLASS_WARRIOR || bot->getClass() == CLASS_ROGUE)
    {
        Item* off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (off && off->GetProto()->InventoryType == INVTYPE_HOLDABLE)
        {
            ItemPosCountVec dest;
            uint8 bagSlot = 0;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, off, bagSlot, false) == EQUIP_ERR_OK)
            {
                bot->RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
                bot->StoreItem(dest, off, true);
            }
            else
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
        }
    }

    uint32 specId = sRandomItemMgr.GetPlayerSpecId(bot);
    for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_TRINKET1 || slot == EQUIPMENT_SLOT_TRINKET2)
            continue;
        Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 bestWeight = oldItem ? sRandomItemMgr.GetLiveStatWeight(bot, oldItem->GetEntry(), specId) : 0;
        uint32 bestItem = 0;
        uint16 bestDest = 0;
        for (uint32 id : vendorItems)
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(id);
            if (!proto || proto->RequiredLevel > bot->GetLevel() || (oldItem && oldItem->GetEntry() == id))
                continue;
            if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (proto->InventoryType == INVTYPE_HOLDABLE && (bot->getClass() == CLASS_WARRIOR || bot->getClass() == CLASS_ROGUE))
                continue;
            if (proto->Class == ITEM_CLASS_WEAPON)
            {
                auto skill = weaponSkill.find(proto->SubClass);
                if (skill == weaponSkill.end() || bot->GetSkillValue(skill->second) * 2 < bot->GetSkillMax(skill->second) || bot->GetSkillValue(skill->second) <= 1)
                    continue;
            }
            uint32 weight = sRandomItemMgr.GetLiveStatWeight(bot, id, specId);
            if (weight <= bestWeight)
                continue;
            uint16 dest;
            if (RandomPlayerbotMgr::CanEquipUnseenItem(bot, slot, dest, id) != EQUIP_ERR_OK)
                continue;
            bestWeight = weight;
            bestItem = id;
            bestDest = dest;
        }
        if (bestItem)
        {
            if (oldItem)
                bot->DestroyItem(oldItem->GetBagSlot(), oldItem->GetSlot(), true);
            bot->EquipNewItem(bestDest, bestItem, true);
        }
    }
}

void RandomPlayerbotMgr::UpdateAIInternal(uint32 elapsed, bool minimal)
{
#ifdef MEMORY_MONITOR
    sMemoryMonitor.LogCount(sConfig.GetStringDefault("LogsDir") + "/" + "memory.csv");
    sMemoryMonitor.SampleDiagnostics();
#endif

    if (!sPlayerbotAIConfig.randomBotAutologin || !sPlayerbotAIConfig.enabled)
        return;

    if (sPlayerbotAIConfig.IsPartyBotOnly())
        return;

    // Silvermoon patch: bots sent into a dungeon with ".rndbot dungeon" commit to it for up to
    // 2.5 hours. Inside, travel/rpg/gathering/fishing are off (those walked them straight back
    // out) and they fight their way through. After a wipe they are brought back in to try
    // again instead of wandering off. The run ends when the dungeon's final boss is dead (or
    // the time is up): then their normal strategies return and the party breaks up.
    {
        static uint32 silvermoonDungeonTimer = 0;
        static uint32 silvermoonBossCheck = 0;
        static std::set<uint32> committed;
        static std::map<uint32, std::array<float, 4>> silvermoonLastGood; // last safe spot inside: x, y, z, map
        static std::map<uint32, time_t> silvermoonDeadSince;
        static std::set<uint32> silvermoonAbandoned; // party ids that gave up
        static std::map<uint32, std::array<float, 4>> silvermoonLastCalm; // last out-of-combat spot inside
        static std::map<uint32, std::deque<std::array<float, 4>>> silvermoonCalmTrail; // recent out-of-combat spots, newest last
        static std::map<uint32, std::array<float, 4>> silvermoonEngageSpot; // where it stood, calm, right before its latest fight
        static std::set<uint32> silvermoonFighting;                          // bots seen in combat at the last check
        silvermoonDungeonTimer += elapsed;
        silvermoonBossCheck += elapsed;
        if (silvermoonDungeonTimer >= 2000)
        {
            silvermoonDungeonTimer = 0;
            bool checkBoss = silvermoonBossCheck >= 20000;
            if (checkBoss)
                silvermoonBossCheck = 0;
            std::set<uint32> finishedMaps; // instance ids whose final boss is dead
            std::vector<Player*> toRelease;
            ForEachPlayerbot([&](Player* p) { SilvermoonDisengage(p, *this); SilvermoonGroundGuard(p, *this); });
            // A committed bot that lost its group (and so would land in a copy of its own) is put
            // back into its party's group first, so the return teleport lands in the party's copy.
            auto rejoinParty = [&](Player* p)
            {
                uint32 partyId = GetEventValue(p->GetGUIDLow(), "silvermoon_dungeon_party");
                uint32 map = GetEventValue(p->GetGUIDLow(), "silvermoon_dungeon");
                if (!partyId || !map)
                    return;
                Group* target = nullptr;
                ForEachPlayerbot([&](Player* q)
                {
                    if (target || !q || q == p || !q->IsInWorld() || !q->GetGroup() || q->GetGroup()->IsRaidGroup() || q->GetGroup()->IsFull())
                        return;
                    if (GetEventValue(q->GetGUIDLow(), "silvermoon_dungeon_party") == partyId && GetEventValue(q->GetGUIDLow(), "silvermoon_dungeon") == map)
                        target = q->GetGroup();
                });
                if (!target || p->GetGroup() == target)
                    return;
                if (Group* old = p->GetGroup())
                    old->RemoveMember(p->GetObjectGuid(), 0);
                if (p->GetMapId() != map)
                    p->UnbindInstance(map);
                if (target->AddMember(p->GetObjectGuid(), p->GetName()))
                    sLog.outBasic("Silvermoon: bot %s is back in its dungeon party (map %u)", p->GetName(), map);
            };
            ForEachPlayerbot([&](Player* p)
            {
                if (!p || !p->IsInWorld() || !p->GetPlayerbotAI() || p->IsBeingTeleported())
                    return;
                PlayerbotAI* pai = p->GetPlayerbotAI();
                uint32 guid = p->GetGUIDLow();
                uint32 dungeonMap = GetEventValue(guid, "silvermoon_dungeon");
                if (!dungeonMap)
                {
                    silvermoonLastGood.erase(guid);
                    silvermoonLastCalm.erase(guid);
                    silvermoonCalmTrail.erase(guid);
                    silvermoonEngageSpot.erase(guid);
                    silvermoonFighting.erase(guid);
                    silvermoonDeadSince.erase(guid);
                    if (committed.erase(guid))
                        toRelease.push_back(p);
                    return;
                }
                if (uint32 partyId = GetEventValue(guid, "silvermoon_dungeon_party"))
                    if (silvermoonAbandoned.count(partyId))
                    {
                        sLog.outBasic("Silvermoon: bot %s leaves dungeon map %u with its party", p->GetName(), dungeonMap);
                        if (!p->IsAlive())
                        {
                            p->ResurrectPlayer(0.5f);
                            p->SpawnCorpseBones();
                        }
                        SetEventValue(guid, "silvermoon_dungeon", 0, 0);
                        SetEventValue(guid, "silvermoon_dungeon_wipes", 0, 0);
                        silvermoonLastGood.erase(guid);
                        if (committed.erase(guid))
                            toRelease.push_back(p);
                        return;
                    }
                SilvermoonDungeon const* d = nullptr;
                for (auto const& entry : g_silvermoonDungeons)
                    if (entry.second.map == dungeonMap)
                        d = &entry.second;
                if (!d)
                    return;
                // Deaths and getting back in. While alive inside the dungeon (and within its area) the
                // bot's position is remembered every 2 seconds. A dead bot - body or ghost, inside or
                // released outside - waits until no living party member is still fighting (and at
                // least 15 seconds), then is revived at the spot where it was last alive inside, in
                // its party's copy of the dungeon. Only a death with no living party member left
                // counts as a wipe; after more than 5 wipes the party gives up.
                {
                    static std::map<uint32, std::array<float, 4>> bounds; // map -> min x, max x, min y, max y
                    static std::map<uint32, float> floorOf;                 // map -> lowest creature z - 40
                    if (!floorOf.count(dungeonMap))
                    {
                        floorOf[dungeonMap] = -100000.0f;
                        if (auto result = WorldDatabase.PQuery("SELECT MIN(`position_z`) FROM `creature` WHERE `map` = '%u'", dungeonMap))
                            floorOf[dungeonMap] = result->Fetch()[0].GetFloat() - 40.0f;
                    }
                    if (!bounds.count(dungeonMap))
                    {
                        std::array<float, 4> b = { -100000.0f, 100000.0f, -100000.0f, 100000.0f };
                        if (auto result = WorldDatabase.PQuery("SELECT MIN(`position_x`), MAX(`position_x`), MIN(`position_y`), MAX(`position_y`) FROM `creature` WHERE `map` = '%u'", dungeonMap))
                        {
                            Field* f = result->Fetch();
                            b = { f[0].GetFloat() - 150.0f, f[1].GetFloat() + 150.0f, f[2].GetFloat() - 150.0f, f[3].GetFloat() + 150.0f };
                        }
                        bounds[dungeonMap] = b;
                    }
                    std::array<float, 4> const& b = bounds[dungeonMap];
                    bool inside = p->GetMapId() == dungeonMap;
                    bool inBounds = inside && p->GetPositionX() >= b[0] && p->GetPositionX() <= b[1] && p->GetPositionY() >= b[2] && p->GetPositionY() <= b[3] &&
                        p->GetPositionZ() >= floorOf[dungeonMap]; // below every spawn: fell through the floor
                    // Under the floor of a multi-level dungeon (the Deadmines): no ground anywhere
                    // below the bot (a ship deck or bridge over a drop still has ground). Only checked for a bot
                    // that is alive and not falling or flying through a jump.
                    if (inBounds && p->IsAlive() && !p->IsFalling())
                    {
                        float ground = p->GetMap()->GetHeight(p->GetPositionX(), p->GetPositionY(), p->GetPositionZ() + 2.0f);
                        if (ground <= INVALID_HEIGHT)
                            inBounds = false;
                    }
                    auto returnSpot = [&]() -> std::array<float, 4>
                    {
                        // Just before the fight it died in: the spot where the party leader (or the
                        // bot itself) stood, out of combat, right before that fight began - ground the
                        // party had already cleared, e.g. just short of Mr. Smite if Smite killed them.
                        {
                            uint32 leaderGuid = guid;
                            if (Group* g = p->GetGroup())
                                leaderGuid = g->GetLeaderGuid().GetCounter();
                            for (uint32 who : { leaderGuid, guid })
                            {
                                auto engage = silvermoonEngageSpot.find(who);
                                if (engage != silvermoonEngageSpot.end() && engage->second[3] == float(dungeonMap))
                                    return { engage->second[0], engage->second[1], engage->second[2], p->GetOrientation() };
                            }
                        }
                        // Otherwise back along the way it came: the newest out-of-combat spot that is at
                        // least 40 yards from where it was last alive, then the oldest one it has, then
                        // the entrance.
                        auto it = silvermoonLastGood.find(guid);
                        if (it != silvermoonLastGood.end() && it->second[3] == float(dungeonMap))
                        {
                            auto trail = silvermoonCalmTrail.find(guid);
                            if (trail != silvermoonCalmTrail.end() && !trail->second.empty())
                            {
                                for (auto r = trail->second.rbegin(); r != trail->second.rend(); ++r)
                                {
                                    float dx = (*r)[0] - it->second[0], dy = (*r)[1] - it->second[1];
                                    if ((*r)[3] == float(dungeonMap) && dx * dx + dy * dy >= 40.0f * 40.0f)
                                        return { (*r)[0], (*r)[1], (*r)[2], p->GetOrientation() };
                                }
                                auto const& oldest = trail->second.front();
                                if (oldest[3] == float(dungeonMap))
                                    return { oldest[0], oldest[1], oldest[2], p->GetOrientation() };
                            }
                        }
                        return { d->x, d->y, d->z, d->o };
                    };

                    if (!p->IsAlive())
                    {
                        auto ds = silvermoonDeadSince.find(guid);
                        if (ds == silvermoonDeadSince.end())
                        {
                            silvermoonDeadSince[guid] = time(0);
                            sLog.outBasic("Silvermoon: bot %s died in dungeon map %u", p->GetName(), dungeonMap);
                            return;
                        }
                        bool partyAlive = false, partyFighting = false;
                        if (Group* g = p->GetGroup())
                            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                                if (Player* m = ref->getSource())
                                    if (m != p && m->IsAlive() && m->GetMapId() == dungeonMap)
                                    {
                                        partyAlive = true;
                                        if (m->IsInCombat())
                                            partyFighting = true;
                                    }
                        // after a wipe, give the mobs time to walk home and reset
                        if (partyFighting || time(0) - ds->second < (partyAlive ? 15 : 45))
                            return;
                        silvermoonDeadSince.erase(ds);
                        if (!partyAlive)
                        {
                            uint32 wipes = GetEventValue(guid, "silvermoon_dungeon_wipes") + 1;
                            SetEventValue(guid, "silvermoon_dungeon_wipes", wipes, 150 * MINUTE);
                            if (wipes > 8)
                            {
                                sLog.outBasic("Silvermoon: bot %s gives up on dungeon map %u after %u wipes", p->GetName(), dungeonMap, wipes - 1);
                                if (uint32 partyId = GetEventValue(guid, "silvermoon_dungeon_party"))
                                    silvermoonAbandoned.insert(partyId); // the whole party calls it, not one member at a time
                                SetEventValue(guid, "silvermoon_dungeon", 0, 0);
                                SetEventValue(guid, "silvermoon_dungeon_wipes", 0, 0);
                                silvermoonLastGood.erase(guid);
                                p->ResurrectPlayer(0.5f);
                                p->SpawnCorpseBones();
                                if (committed.erase(guid))
                                    toRelease.push_back(p);
                                return;
                            }
                        }
                        std::array<float, 4> spot = returnSpot();
                        p->ResurrectPlayer(1.0f); // full health and mana: no half-drunk re-pull
                        p->SpawnCorpseBones();
                        sLog.outBasic("Silvermoon: bot %s revived in dungeon map %u at (%.0f, %.0f, %.0f)", p->GetName(), dungeonMap, spot[0], spot[1], spot[2]);
                        if (inside)
                            p->NearTeleportTo(spot[0], spot[1], spot[2], spot[3]);
                        else
                        {
                            rejoinParty(p);
                            p->TeleportTo(dungeonMap, spot[0], spot[1], spot[2], spot[3]);
                        }
                        return;
                    }
                    silvermoonDeadSince.erase(guid);

                    if (inside && checkBoss && (!p->GetGroup() || p->GetGroup()->GetMembersCount() < 2) && GetEventValue(guid, "silvermoon_dungeon_party"))
                    {
                        // alone in a copy of its own (it lost its group, e.g. across a restart): back
                        // into its party's group, and out and in again so it lands in their copy
                        rejoinParty(p);
                        if (Group* g = p->GetGroup())
                            if (g->GetMembersCount() > 1)
                                if (Player* leader = sObjectMgr.GetPlayer(g->GetLeaderGuid()))
                                    if (leader != p && leader->GetMapId() == dungeonMap && leader->GetInstanceId() != p->GetInstanceId())
                                    {
                                        sLog.outBasic("Silvermoon: bot %s was alone in its own copy of dungeon map %u, rejoining its party", p->GetName(), dungeonMap);
                                        p->TeleportToHomebind(); // the outside handler brings it back, into the party's copy
                                        return;
                                    }
                    }
                    if (!inside || !inBounds)
                    {
                        // wandered (or was put) outside: back to where it was, at most every 10 seconds
                        static std::map<uint32, time_t> lastPull;
                        if (time(0) - lastPull[guid] < 10)
                            return;
                        lastPull[guid] = time(0);
                        std::array<float, 4> spot = returnSpot();
                        sLog.outBasic("Silvermoon: bot %s was outside dungeon map %u (map %u, %.0f, %.0f), back to (%.0f, %.0f)", p->GetName(), dungeonMap, p->GetMapId(), p->GetPositionX(), p->GetPositionY(), spot[0], spot[1]);
                        if (inside)
                            p->NearTeleportTo(spot[0], spot[1], spot[2], spot[3]);
                        else
                        {
                            rejoinParty(p);
                            p->TeleportTo(dungeonMap, spot[0], spot[1], spot[2], spot[3]);
                        }
                        return;
                    }
                    // heading for a spot outside the dungeon's area (the void): stop
                    {
                        float mx, my, mz;
                        if (p->GetMotionMaster()->GetDestination(mx, my, mz) && (mx < b[0] || mx > b[1] || my < b[2] || my > b[3]))
                        {
                            sLog.outBasic("Silvermoon: bot %s was heading out of dungeon map %u to (%.0f, %.0f), stopped", p->GetName(), dungeonMap, mx, my);
                            p->GetMotionMaster()->Clear();
                            p->StopMoving();
                        }
                    }
                    silvermoonLastGood[guid] = { p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), float(dungeonMap) };
                    // pets don't start fights in a dungeon: passive while the owner is out of combat
                    // (they ran ahead and pulled before the tank), defensive once it fights
                    if (Pet* pet = p->GetPet())
                        if (pet->AI())
                            pet->AI()->SetReactState(p->IsInCombat() ? REACT_DEFENSIVE : REACT_PASSIVE);
                    if (p->IsInCombat())
                    {
                        // the fight just started: remember where it stood right before it
                        if (silvermoonFighting.insert(guid).second)
                        {
                            auto calm = silvermoonLastCalm.find(guid);
                            if (calm != silvermoonLastCalm.end() && calm->second[3] == float(dungeonMap))
                                silvermoonEngageSpot[guid] = calm->second;
                        }
                    }
                    else
                        silvermoonFighting.erase(guid);
                    if (!p->IsInCombat())
                    {
                        silvermoonLastCalm[guid] = { p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), float(dungeonMap) };
                        auto& trail = silvermoonCalmTrail[guid];
                        if (trail.empty() || std::abs(trail.back()[0] - p->GetPositionX()) + std::abs(trail.back()[1] - p->GetPositionY()) > 4.0f)
                            trail.push_back({ p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), float(dungeonMap) });
                        while (trail.size() > 90)
                            trail.pop_front();
                    }
                }

                // (re)apply on commit, and again whenever a reset - e.g. after being revived - put
                // the wandering strategies back. Grinding is off too: bots don't pick fights on their
                // own in a dungeon, the leader pulls (below) and the party fights what comes.
                if (committed.insert(guid).second || pai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) || pai->HasStrategy("grind", BotState::BOT_STATE_NON_COMBAT) || pai->HasStrategy("emote", BotState::BOT_STATE_NON_COMBAT))
                {
                    // no emotes, jumping or combat chatter during a run: downtime is for eating and
                    // drinking only (normal behaviour comes back when the run ends)
                    pai->ChangeStrategy("-travel,-rpg,-tfish,-gather,-explore,-grind,-emote,-rpg jump,+dungeon", BotState::BOT_STATE_NON_COMBAT);
                    pai->ChangeStrategy("+cc,-aoe,-emote,-custom::say", BotState::BOT_STATE_COMBAT);
                    // Roles, the way ".bot co" sets them by hand. Tank: tank spec, and "tank assist"
                    // (it goes for whatever is hitting a party member - the healer first - to take
                    // it back). Healer: healing spec, still helping on the tank's target when free.
                    // Damage: assist the tank's target and watch their threat ("threat" makes them
                    // ease off before they pull a mob off the tank).
                    switch (GetEventValue(guid, "silvermoon_dungeon_role"))
                    {
                    case 1:
                        pai->ChangeStrategy("-arms,-fury,-retribution,-cat,-balance,-dps feral,-dps assist,+tank,+tank assist,-threat", BotState::BOT_STATE_COMBAT);
                        pai->ChangeStrategy("+tank assist", BotState::BOT_STATE_NON_COMBAT);
                        break;
                    case 2:
                        pai->ChangeStrategy("-shadow,-enhancement,-elemental,-retribution,-cat,-balance,+heal,+dps assist,+threat", BotState::BOT_STATE_COMBAT);
                        break;
                    default:
                        pai->ChangeStrategy("+dps assist,+threat", BotState::BOT_STATE_COMBAT);
                        break;
                    }
                    sLog.outBasic("Silvermoon: bot %s committed to dungeon map %u", p->GetName(), dungeonMap);
                }
                if (TravelTarget* t = pai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                    if (t->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED && t->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE)
                        t->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                // Keep two bag slots free during a run so a dropped upgrade (or a won roll) always
                // fits: when fewer are free, the cheapest junk goes - gray items first, then the
                // lowest vendor value. Quest items, bags, gear the bot could equip and trade goods
                // it can use are kept.
                if (checkBoss && !p->IsInCombat())
                {
                    uint32 freeSlots = 0;
                    std::vector<Item*> candidates;
                    auto consider = [&](Item* item)
                    {
                        if (!item)
                        {
                            ++freeSlots;
                            return;
                        }
                        ItemPrototype const* proto = item->GetProto();
                        if (!proto || proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_CONTAINER || proto->StartQuest)
                            return;
                        ItemUsage usage = pai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", ItemQualifier(item).GetQualifier())->Get();
                        if (usage == ItemUsage::ITEM_USAGE_EQUIP || usage == ItemUsage::ITEM_USAGE_QUEST || usage == ItemUsage::ITEM_USAGE_SKILL ||
                            usage == ItemUsage::ITEM_USAGE_USE || usage == ItemUsage::ITEM_USAGE_AMMO || usage == ItemUsage::ITEM_USAGE_FORCE_NEED)
                            return;
                        candidates.push_back(item);
                    };
                    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                        consider(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
                    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                        if (Bag* bag = (Bag*)p->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
                            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                                consider(bag->GetItemByPos(j));
                    if (freeSlots < 3 && !candidates.empty()) // two for loot, one for food and water
                    {
                        std::sort(candidates.begin(), candidates.end(), [](Item* a, Item* b)
                        {
                            uint32 qa = a->GetProto()->Quality, qb = b->GetProto()->Quality;
                            if (qa != qb)
                                return qa < qb;
                            return a->GetProto()->SellPrice * a->GetCount() < b->GetProto()->SellPrice * b->GetCount();
                        });
                        for (uint32 i = 0; i < candidates.size() && freeSlots < 3; ++i, ++freeSlots)
                        {
                            sLog.outBasic("Silvermoon: bot %s makes bag room in the dungeon, destroying %s", p->GetName(), candidates[i]->GetProto()->Name1);
                            p->DestroyItem(candidates[i]->GetBagSlot(), candidates[i]->GetSlot(), true);
                        }
                    }
                    // food and water all through the run: whenever it is below 10 water (mana users) or 5
                    // food - bags were full when the party formed, or it ate and drank them - it buys
                    // more for free (only from a third free slot on, so
                    // two stay free for loot)
                    if (freeSlots >= 3)
                        SilvermoonStockUp(p);
                }
                // open doors in the party's way
                {
                    std::list<GameObject*> doors;
                    SilvermoonDoorCheck doorCheck(p, 30.0f);
                    MaNGOS::GameObjectListSearcher<SilvermoonDoorCheck> doorSearcher(doors, doorCheck);
                    Cell::VisitGridObjects(p, doorSearcher, 30.0f);
                    for (GameObject* door : doors)
                    {
                        sLog.outBasic("Silvermoon: opening %s for bot party in map %u", door->GetGOInfo()->name, dungeonMap);
                        door->UseDoorOrButton(0, false);
                    }
                }
                // Point-to-point route: the party leader walks (navmesh pathing) to the spawn point of
                // the next boss in dungeon order that is not done yet, whenever it is out of combat.
                // Spawn points come from the world database, so this works even when that part of
                // the dungeon is not loaded yet. A boss counts as done once the leader stands at its
                // spawn and it is not there alive. No progress for 3 minutes: moved up to the spot.
                if (checkBoss && p->GetGroup() && !p->GetGroup()->IsLeader(p->GetObjectGuid()) && GetEventValue(guid, "silvermoon_dungeon_role") == 1 &&
                    p->IsAlive() && !p->IsInCombat())
                {
                    // the tank leads (and pulls); leadership moved when a leader left or relogged
                    sLog.outBasic("Silvermoon: bot %s takes the lead of its dungeon party again", p->GetName());
                    p->GetGroup()->ChangeLeader(p->GetObjectGuid());
                }
                if (p->IsAlive() && !p->IsInCombat() && p->GetGroup() && p->GetGroup()->IsLeader(p->GetObjectGuid()))
                {
                    static std::map<uint32, std::pair<float, uint32>> progress;        // leader -> (best distance, stuck checks)
                    static std::map<uint32, std::set<uint32>> bossesDone;              // instance -> boss entries done
                    static std::map<uint32, std::unique_ptr<WorldLocation>> spawnOf;   // boss entry -> spawn point
                    std::set<uint32>& done = bossesDone[p->GetInstanceId()];
                    for (uint32 entry : d->bosses)
                    {
                        if (done.count(entry))
                            continue;
                        if (!spawnOf.count(entry))
                        {
                            spawnOf[entry].reset();
                            if (auto result = WorldDatabase.PQuery("SELECT `position_x`, `position_y`, `position_z` FROM `creature` WHERE `id` = '%u' AND `map` = '%u' LIMIT 1", entry, dungeonMap))
                            {
                                Field* f = result->Fetch();
                                spawnOf[entry].reset(new WorldLocation(dungeonMap, f[0].GetFloat(), f[1].GetFloat(), f[2].GetFloat(), 0.0f));
                            }
                        }
                        WorldLocation* spot = spawnOf[entry].get();
                        if (!spot)
                        {
                            done.insert(entry); // no fixed spawn (summoned boss): skip it
                            continue;
                        }
                        float dist = p->GetDistance(spot->coord_x, spot->coord_y, spot->coord_z);
                        if (dist < 50.0f)
                        {
                            std::list<Creature*> found;
                            MaNGOS::AllCreaturesOfEntryInRangeCheck bossCheck(p, entry, 80.0f);
                            MaNGOS::CreatureListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> bossSearcher(found, bossCheck);
                            Cell::VisitGridObjects(p, bossSearcher, 80.0f);
                            bool alive = false;
                            for (Creature* c : found)
                                if (c->IsAlive())
                                    alive = true;
                            if (!alive)
                            {
                                done.insert(entry);
                                sLog.outBasic("Silvermoon: dungeon map %u instance %u: boss %u done", dungeonMap, p->GetInstanceId(), entry);
                                continue;
                            }
                        }
                        auto& prog = progress[guid];
                        if (prog.first == 0.0f || dist < prog.first - 5.0f)
                            prog = std::make_pair(dist, 0u);
                        else
                            ++prog.second;
                        if (prog.second >= 90 && dist > 30.0f) // about 3 minutes without progress
                        {
                            sLog.outBasic("Silvermoon: bot %s stuck on the way to boss %u, moving it closer", p->GetName(), entry);
                            p->NearTeleportTo(spot->coord_x, spot->coord_y, spot->coord_z, p->GetOrientation());
                            prog = std::make_pair(0.0f, 0u);
                        }
                        else if (dist > 15.0f)
                        {
                            // Pull, don't charge. The leader waits while anyone in the party is dead,
                            // fighting, hurt (under 70% health), low on mana (under 60%) or lagging
                            // behind. When ready, the nearest hostile mob within 40 yards (in line of
                            // sight) is pulled back to the party - its pack comes with it - instead of
                            // the party running into the room and waking several packs at once.
                            // Something is already fighting a party member or a pet (a pet ran ahead, a
                            // patrol wandered in) while the tank stands idle: the tank takes it - the mob
                            // turns on the tank, like a taunt - so the whole party joins in instead of
                            // standing around waiting.
                            {
                                Unit* loose = nullptr;
                                for (GroupReference* ref = p->GetGroup()->GetFirstMember(); ref && !loose; ref = ref->next())
                                {
                                    Player* m = ref->getSource();
                                    if (!m || m == p || m->GetMapId() != p->GetMapId())
                                        continue;
                                    for (Unit* who : { (Unit*)m, (Unit*)m->GetPet() })
                                        if (who && !loose)
                                            for (Unit* u : who->getAttackers())
                                                if (u && u->IsAlive() && u->GetTypeId() == TYPEID_UNIT && p->GetDistance(u) < 60.0f)
                                                {
                                                    loose = u;
                                                    break;
                                                }
                                }
                                if (loose && loose->AI())
                                {
                                    loose->AI()->AttackStart(p);
                                    sLog.outBasic("Silvermoon: bot %s takes %s off the party on dungeon map %u", p->GetName(), loose->GetName(), dungeonMap);
                                    prog = std::make_pair(0.0f, 0u);
                                    break;
                                }
                            }
                            bool ready = true;
                            for (GroupReference* ref = p->GetGroup()->GetFirstMember(); ref; ref = ref->next())
                            {
                                Player* m = ref->getSource();
                                if (!m || m == p || m->GetMapId() != p->GetMapId())
                                    continue;
                                // a living, calm member far away in the same copy (revived elsewhere, or
                                // stuck) is brought to the leader instead of the leader waiting forever
                                if (m->IsAlive() && !m->IsInCombat() && m->GetMap() == p->GetMap() && m->GetDistance(p) > 45.0f && !m->IsBeingTeleported())
                                {
                                    sLog.outBasic("Silvermoon: bot %s was %.0f yards from its party leader %s, brought over", m->GetName(), m->GetDistance(p), p->GetName());
                                    m->NearTeleportTo(p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), p->GetOrientation());
                                }
                                if (!m->IsAlive() || m->IsInCombat() || m->GetHealthPercent() < 60.0f || m->GetDistance(p) > 45.0f ||
                                    (m->GetPowerType() == POWER_MANA && m->GetMaxPower(POWER_MANA) > 0 && m->GetPower(POWER_MANA) * 100 < m->GetMaxPower(POWER_MANA) * 45))
                                    ready = false;
                            }
                            if (p->GetHealthPercent() < 60.0f || (p->GetPowerType() == POWER_MANA && p->GetMaxPower(POWER_MANA) > 0 && p->GetPower(POWER_MANA) * 100 < p->GetMaxPower(POWER_MANA) * 45))
                                ready = false;
                            if (!ready)
                            {
                                if (p->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                                {
                                    p->GetMotionMaster()->Clear();
                                    p->StopMoving();
                                }
                                prog = std::make_pair(0.0f, 0u); // waiting is not being stuck
                                break;
                            }
                            Creature* pull = nullptr;
                            {
                                std::list<Unit*> near;
                                // Silvermoon patch (refined): 25 yards at most (a mob across the lava 40
                                // yards away coming for the party looked like aggro from nowhere), on
                                // about the same level, and with a walkable path no longer than 1.5x the
                                // straight line - not across a chasm or lava.
                                MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck nearCheck(p, 25.0f);
                                MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> nearSearcher(near, nearCheck);
                                Cell::VisitAllObjects(p, nearSearcher, 25.0f);
                                float best = 26.0f;
                                for (Unit* u : near)
                                {
                                    if (u->GetTypeId() != TYPEID_UNIT || !u->IsAlive() || u->IsInCombat() || ((Creature*)u)->IsPet() || ((Creature*)u)->IsTotem())
                                        continue;
                                    if (!u->CanAttackOnSight(p) || !p->IsWithinLOSInMap(u) || std::abs(u->GetPositionZ() - p->GetPositionZ()) > 6.0f)
                                        continue;
                                    float dd = p->GetDistance(u);
                                    if (dd >= best)
                                        continue;
                                    {
                                        PathFinder path(u);
                                        path.calculate(p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), false);
                                        if ((path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) != 0)
                                            continue;
                                        float length = 0.0f;
                                        PointsArray const& points = path.getPath();
                                        for (size_t i = 1; i < points.size(); ++i)
                                            length += (points[i] - points[i - 1]).length();
                                        if (length > dd * 1.5f + 3.0f)
                                            continue;
                                    }
                                    if (dd < best)
                                    {
                                        best = dd;
                                        pull = (Creature*)u;
                                    }
                                }
                            }
                            if (pull && pull->AI())
                            {
                                if (p->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                                {
                                    p->GetMotionMaster()->Clear();
                                    p->StopMoving();
                                }
                                p->SetFacingToObject(pull);
                                pull->AI()->AttackStart(p);
                                sLog.outBasic("Silvermoon: bot %s pulls %s (%.0f yards) on dungeon map %u", p->GetName(), pull->GetName(), p->GetDistance(pull), dungeonMap);
                                prog = std::make_pair(0.0f, 0u);
                                break;
                            }
                            if (p->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                            {
                                // no complete walking path to the boss (the Deadmines ship, a ledge):
                                // the leader is moved up to it instead of wandering up the nearest hill,
                                // and the party is gathered to the leader by the check above
                                PathFinder path(p);
                                path.calculate(spot->coord_x, spot->coord_y, spot->coord_z, false);
                                if ((path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) != 0 && dist > 30.0f)
                                {
                                    sLog.outBasic("Silvermoon: bot %s has no walking path to boss %u on dungeon map %u, moving it there", p->GetName(), entry, dungeonMap);
                                    p->NearTeleportTo(spot->coord_x, spot->coord_y, spot->coord_z, p->GetOrientation());
                                    prog = std::make_pair(0.0f, 0u);
                                }
                                else
                                    p->GetMotionMaster()->MovePoint(0, spot->coord_x, spot->coord_y, spot->coord_z, FORCED_MOVEMENT_RUN);
                            }
                        }
                        break;
                    }
                    if (done.count(d->finalBoss))
                        finishedMaps.insert(p->GetInstanceId());
                }
                if (checkBoss && !finishedMaps.count(p->GetInstanceId()))
                {
                    std::list<Creature*> bosses;
                    MaNGOS::AllCreaturesOfEntryInRangeCheck check(p, d->finalBoss, 2000.0f);
                    MaNGOS::CreatureListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(bosses, check);
                    Cell::VisitGridObjects(p, searcher, 2000.0f);
                    for (Creature* boss : bosses)
                        if (!boss->IsAlive())
                            finishedMaps.insert(p->GetInstanceId());
                }
            });
            // Keep each dungeon party together: every 20 seconds, committed bots in the same copy
            // of a dungeon that are not in one group (after a relog, a restart or a bot leaving)
            // are put back into the group most of them are in (or a new one).
            if (checkBoss)
            {
                std::map<uint32, std::vector<Player*>> byInstance;
                ForEachPlayerbot([&](Player* p)
                {
                    if (p && p->IsInWorld() && !p->IsBeingTeleported() && committed.count(p->GetGUIDLow()) && p->GetMapId() == GetEventValue(p->GetGUIDLow(), "silvermoon_dungeon"))
                        byInstance[p->GetInstanceId()].push_back(p);
                });
                for (auto& [instance, members] : byInstance)
                {
                    if (members.size() < 2)
                        continue;
                    std::map<Group*, uint32> counts;
                    for (Player* p : members)
                        if (Group* g = p->GetGroup())
                            if (!g->IsRaidGroup())
                                ++counts[g];
                    Group* target = nullptr;
                    uint32 best = 0;
                    for (auto const& [g, n] : counts)
                        if (n > best)
                        {
                            best = n;
                            target = g;
                        }
                    if (target && best == members.size())
                        continue;
                    if (!target)
                    {
                        Player* leader = members.front();
                        if (Group* old = leader->GetGroup())
                            old->RemoveMember(leader->GetObjectGuid(), 0);
                        target = new Group;
                        if (!target->Create(leader->GetObjectGuid(), leader->GetName()))
                        {
                            delete target;
                            continue;
                        }
                        sObjectMgr.AddGroup(target);
                    }
                    for (Player* p : members)
                    {
                        if (p->GetGroup() == target || target->IsFull())
                            continue;
                        if (Group* old = p->GetGroup())
                            old->RemoveMember(p->GetObjectGuid(), 0);
                        if (target->AddMember(p->GetObjectGuid(), p->GetName()))
                            sLog.outBasic("Silvermoon: bot %s rejoined its dungeon party on map %u", p->GetName(), p->GetMapId());
                    }
                }
            }
            if (!finishedMaps.empty())
            {
                ForEachPlayerbot([&](Player* p)
                {
                    if (p && p->IsInWorld() && committed.count(p->GetGUIDLow()) && finishedMaps.count(p->GetInstanceId()))
                    {
                        sLog.outBasic("Silvermoon: bot %s cleared dungeon map %u", p->GetName(), p->GetMapId());
                        SetEventValue(p->GetGUIDLow(), "silvermoon_dungeon", 0, 0);
                        committed.erase(p->GetGUIDLow());
                        toRelease.push_back(p);
                    }
                });
            }
            for (Player* p : toRelease)
            {
                p->GetPlayerbotAI()->ResetStrategies();
                if (Group* group = p->GetGroup())
                    if (group->IsLeader(p->GetObjectGuid()))
                        group->Disband(true);
                sLog.outBasic("Silvermoon: bot %s finished its dungeon run", p->GetName());
            }
        }
    }

    // Silvermoon patch: keep spectating GMs next to their target, once a second.
    {
        static uint32 silvermoonSpectateTimer = 0;
        silvermoonSpectateTimer += elapsed;
        if (silvermoonSpectateTimer >= 1000 && !g_silvermoonSpectate.empty())
        {
            silvermoonSpectateTimer = 0;
            for (auto it = g_silvermoonSpectate.begin(); it != g_silvermoonSpectate.end();)
            {
                Player* gm = sObjectMgr.GetPlayer(it->first);
                Player* target = sObjectMgr.GetPlayer(it->second);
                if (!gm || !target || !gm->IsInWorld())
                {
                    it = g_silvermoonSpectate.erase(it);
                    continue;
                }
                // Rate limit: after any jump, give the client 6 seconds to load the area (jumping
                // every second while it was still loading left the GM in the void or disconnected).
                // A failed dungeon join is retried at most every 30 seconds, 3 times.
                static std::map<ObjectGuid, time_t> lastJump;
                static std::map<ObjectGuid, uint32> dungeonTries;
                time_t nowJump = time(nullptr);
                bool sameInstance = gm->GetMap() == target->GetMap();
                if (sameInstance)
                    dungeonTries.erase(it->first);
                // A spectated bot that died in a dungeon: stay where it fell (by its corpse) until
                // it is revived, instead of following its ghost out to the graveyard and back in.
                if (!target->IsAlive() && gm->GetMap() && gm->GetMap()->IsDungeon() &&
                    (target->GetMapId() != gm->GetMapId() || target->GetMap() == gm->GetMap()))
                {
                    ++it;
                    continue;
                }
                if (target->IsInWorld() && !target->IsBeingTeleported() && !gm->IsBeingTeleported() &&
                    nowJump - lastJump[it->first] >= 6 &&
                    (gm->GetMapId() != target->GetMapId() || gm->GetDistance(target) > 25.0f))
                {
                    lastJump[it->first] = nowJump;
                    // (no unbinding first: running ".instance unbind" while standing in an
                    // instance threw the GM out to their graveyard)
                    if (target->GetMap() && target->GetMap()->Instanceable() && gm->GetMap() != target->GetMap())
                    {
                        // dungeons: a plain teleport would open the GM's own copy of the
                        // instance; .appear joins the target's instance instead
                        uint32& tries = dungeonTries[it->first];
                        if (tries >= 3)
                        {
                            ChatHandler(gm->GetSession()).SendSysMessage("Spectate: could not join that dungeon copy after 3 tries; stopping. Try .appear by hand.");
                            dungeonTries.erase(it->first);
                            it = g_silvermoonSpectate.erase(it);
                            continue;
                        }
                        ++tries;
                        lastJump[it->first] = nowJump + 24; // next dungeon attempt in 30 seconds at the earliest
                        // bind the GM to the target's own copy of the dungeon, then teleport to the
                        // target's position in it (".appear" could leave the GM at a stale spot
                        // outside the dungeon's geometry)
                        if (target->GetMap()->IsDungeon())
                        {
                            DungeonMap* dungeon = (DungeonMap*)target->GetMap();
                            gm->UnbindInstance(target->GetMapId());
                            gm->BindToInstance(dungeon->GetPersistanceState(), false);
                        }
                        float o = target->GetOrientation();
                        gm->TeleportTo(target->GetMapId(), target->GetPositionX() - 4.0f * cos(o),
                            target->GetPositionY() - 4.0f * sin(o), target->GetPositionZ() + 2.0f, o);
                    }
                    else
                    {
                        float o = target->GetOrientation();
                        gm->TeleportTo(target->GetMapId(), target->GetPositionX() - 8.0f * cos(o),
                            target->GetPositionY() - 8.0f * sin(o), target->GetPositionZ() + 3.0f, o);
                    }
                }
                ++it;
            }
        }
    }

    // Silvermoon patch: bots only answer a group invite from their out-of-combat
    // strategy, and an invite that arrives mid-fight can be lost. The invite then
    // hangs, and inviting again says "already in a group". Accept any invite from a
    // real player that has been pending for a few seconds.
    {
        static time_t silvermoonLastInviteCheck = 0;
        static std::map<uint32, time_t> silvermoonInviteSeen;
        time_t nowTime = time(nullptr);
        if (nowTime - silvermoonLastInviteCheck >= 2)
        {
            silvermoonLastInviteCheck = nowTime;
            std::set<uint32> pending;
            ForEachPlayerbot([&](Player* p)
            {
                if (!p || !p->IsInWorld() || !p->GetPlayerbotAI())
                    return;
                Group* invite = p->GetGroupInvite();
                if (!invite)
                    return;
                Player* inviter = sObjectMgr.GetPlayer(invite->GetLeaderGuid());
                if (!inviter || (inviter->GetPlayerbotAI() && !inviter->GetPlayerbotAI()->IsRealPlayer()))
                    return;
                uint32 guid = p->GetGUIDLow();
                pending.insert(guid);
                auto seen = silvermoonInviteSeen.find(guid);
                if (seen == silvermoonInviteSeen.end())
                {
                    silvermoonInviteSeen[guid] = nowTime;
                    return;
                }
                if (nowTime < seen->second + 3)
                    return;
                sLog.outBasic("Silvermoon: bot %s left %s's group invite unanswered, accepting it", p->GetName(), inviter->GetName());
                p->GetPlayerbotAI()->DoSpecificAction("accept invitation", Event(), true);
                if (p->GetGroupInvite())
                    seen->second = nowTime + 20; // still pending (refused?), try again later
            });
            // Same for guild invites: accept one from a guild with a real player online
            // once it has been pending a few seconds; drop any other invite left
            // hanging for over a minute so it can't block new ones.
            ForEachPlayerbot([&](Player* p)
            {
                if (!p || !p->IsInWorld() || !p->GetPlayerbotAI())
                    return;
                uint32 guildId = p->GetGuildIdInvited();
                if (!guildId)
                    return;
                uint32 key = p->GetGUIDLow() | 0x80000000;
                pending.insert(key);
                auto seen = silvermoonInviteSeen.find(key);
                if (seen == silvermoonInviteSeen.end())
                {
                    silvermoonInviteSeen[key] = nowTime;
                    return;
                }
                bool realPlayerGuild = false;
                for (auto& i : GetPlayers())
                    if (i.second && i.second->IsInWorld() && i.second->GetGuildId() == guildId)
                        realPlayerGuild = true;
                WorldPacket empty;
                if (realPlayerGuild && !p->GetGuildId() && nowTime >= seen->second + 3)
                {
                    sLog.outBasic("Silvermoon: bot %s left a guild invite unanswered, accepting it", p->GetName());
                    p->GetSession()->HandleGuildAcceptOpcode(empty);
                }
                else if (nowTime >= seen->second + 60)
                {
                    sLog.outBasic("Silvermoon: bot %s left a guild invite unanswered for a minute, declining it", p->GetName());
                    p->GetSession()->HandleGuildDeclineOpcode(empty);
                }
            });
            // Trades: when a real player has accepted and the bot has not answered for a
            // few seconds, run the bot's normal trade logic again (it still checks
            // prices and may refuse). Quest shares: clear a share left pending, which
            // otherwise makes every later share fail with "<bot> is busy".
            ForEachPlayerbot([&](Player* p)
            {
                if (!p || !p->IsInWorld() || !p->GetPlayerbotAI())
                    return;
                Player* trader = p->GetTrader();
                if (trader && p->GetTradeData() && trader->GetTradeData() && trader->GetTradeData()->IsAccepted() &&
                    !p->GetTradeData()->IsAccepted() && (!trader->GetPlayerbotAI() || trader->GetPlayerbotAI()->IsRealPlayer()))
                {
                    uint32 key = p->GetGUIDLow() | 0x40000000;
                    pending.insert(key);
                    auto seen = silvermoonInviteSeen.find(key);
                    if (seen == silvermoonInviteSeen.end())
                        silvermoonInviteSeen[key] = nowTime;
                    else if (nowTime >= seen->second + 4)
                    {
                        sLog.outBasic("Silvermoon: bot %s left %s's trade unanswered, answering it", p->GetName(), trader->GetName());
                        WorldPacket status(SMSG_TRADE_STATUS, 4);
                        status << uint32(TRADE_STATUS_TRADE_ACCEPT);
                        Event tradeEvent("trade status", status, trader);
                        p->GetPlayerbotAI()->DoSpecificAction("accept trade", tradeEvent, true);
                        seen->second = nowTime + 15;
                    }
                }
                if (!p->GetDividerGuid().IsEmpty())
                {
                    uint32 key = p->GetGUIDLow() | 0x20000000;
                    pending.insert(key);
                    auto seen = silvermoonInviteSeen.find(key);
                    if (seen == silvermoonInviteSeen.end())
                        silvermoonInviteSeen[key] = nowTime;
                    else if (nowTime >= seen->second + 8)
                    {
                        sLog.outBasic("Silvermoon: bot %s left a quest share pending, clearing it", p->GetName());
                        p->SetDividerGuid(ObjectGuid());
                    }
                }
            });
            for (auto it = silvermoonInviteSeen.begin(); it != silvermoonInviteSeen.end();)
            {
                if (pending.count(it->first))
                    ++it;
                else
                    it = silvermoonInviteSeen.erase(it);
            }
        }
    }

    {
        static time_t silvermoonLastGuildCount = 0;
        time_t nowTime = time(nullptr);
        if (nowTime - silvermoonLastGuildCount >= 60)
        {
            silvermoonLastGuildCount = nowTime;
            std::set<uint32> allianceGuilds, hordeGuilds;
            auto countGuild = [&](Player* p)
            {
                if (p && p->IsInWorld() && p->GetGuildId())
                    (p->GetTeam() == ALLIANCE ? allianceGuilds : hordeGuilds).insert(p->GetGuildId());
            };
            ForEachPlayerbot(countGuild);
            for (auto i : GetPlayers())
                countGuild(i.second);

            std::vector<uint32> allianceLevels, hordeLevels;
            ForEachPlayerbot([&](Player* p)
            {
                if (p && p->IsInWorld() && IsRandomBot(p))
                    (p->GetTeam() == ALLIANCE ? allianceLevels : hordeLevels).push_back(p->GetLevel());
            });
            auto topLevel = [](std::vector<uint32>& levels) -> uint32
            {
                if (levels.empty())
                    return 255;
                std::sort(levels.begin(), levels.end(), std::greater<uint32>());
                return std::max(uint32(6), levels[std::min<size_t>(9, levels.size() - 1)]);
            };
            g_silvermoonTopLevel[0] = topLevel(allianceLevels);
            g_silvermoonTopLevel[1] = topLevel(hordeLevels);

            // Silvermoon patch: pick and log in the priority bots (see g_silvermoonPriorityBots)
            {
                static time_t lastPriority = 0;
                if (nowTime - lastPriority >= 600)
                {
                    lastPriority = nowTime;
                    std::map<std::pair<bool, uint32>, std::pair<uint32, uint32>> best; // (alliance, class) -> (level, guid)
                    if (auto result = CharacterDatabase.Query("SELECT `guid`, `account`, `race`, `class`, `level` FROM `characters`"))
                    {
                        do
                        {
                            Field* f = result->Fetch();
                            uint32 guid = f[0].GetUInt32(), account = f[1].GetUInt32();
                            uint32 race = f[2].GetUInt32(), cls = f[3].GetUInt32(), level = f[4].GetUInt32();
                            if (!sPlayerbotAIConfig.IsInRandomAccountList(account))
                                continue;
                            auto& slot = best[std::make_pair(IsAlliance(race), cls)];
                            if (level > slot.first)
                                slot = std::make_pair(level, guid);
                        } while (result->NextRow());
                    }
                    g_silvermoonPriorityBots.clear();
                    for (auto& entry : best)
                        if (entry.second.second)
                            g_silvermoonPriorityBots.insert(entry.second.second);
                }
                if (!players.empty() || !sPlayerbotAIConfig.randomBotLoginWithPlayer)
                {
                    for (uint32 guid : g_silvermoonPriorityBots)
                    {
                        if (GetPlayerBot(guid) || GetEventValue(guid, "login"))
                            continue;
                        sLog.outBasic("Silvermoon: logging in priority bot #%u (top of its class)", guid);
                        SetEventValue(guid, "add", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
                        SetEventValue(guid, "logout", 0, 0);
                        if (std::find(currentBots.begin(), currentBots.end(), guid) == currentBots.end())
                            currentBots.push_back(guid);
                        AddPlayerBot(guid, 0);
                        SetEventValue(guid, "login", 1, -1);
                    }
                }
            }

            // Silvermoon patch: most bots never paid a trainer (they are nearly broke), so
            // they fought with level 1 abilities. Once per bot per server run, teach it
            // every class spell for its level and spend its talent points. New levels are
            // covered by AiPlayerbot.AutoLearnTrainerSpells = 1. 40 bots a minute at most.
            {
                static std::set<uint32> silvermoonSpellsDone;
                uint32 budget = 40;
                ForEachPlayerbot([&](Player* p)
                {
                    if (!budget || !p || !p->IsInWorld() || !IsRandomBot(p) || p->IsInCombat() || !p->GetPlayerbotAI())
                        return;
                    if (!silvermoonSpellsDone.insert(p->GetGUIDLow()).second)
                        return;
                    --budget;
                    // Free spell catch-up is off again (S, 2026-11-15): bots pay trainers like
                    // players do. Set to true to re-enable the one-time catch-up.
                    static const bool silvermoonFreeSpells = false;
                    if (silvermoonFreeSpells)
                    {
                        p->learnDefaultSpells();
                        p->learnClassLevelSpells(true);
                    }

                    // Every spell the bot's class trainers teach up to its level (Dual Wield,
                    // weapon skills, new ranks, ...), taught directly. The list of trainer
                    // spells per class is built once.
                    static std::map<uint8, std::vector<std::pair<uint32, uint32>>> classTrainerSpells;
                    std::vector<std::pair<uint32, uint32>>& trainerList = classTrainerSpells[p->getClass()];
                    if (trainerList.empty())
                    {
                        for (uint32 id = 0; id < sCreatureStorage.GetMaxEntry(); ++id)
                        {
                            CreatureInfo const* co = sCreatureStorage.LookupEntry<CreatureInfo>(id);
                            if (!co || co->TrainerType != TRAINER_TYPE_CLASS || co->TrainerClass != p->getClass())
                                continue;
                            uint32 trainerId = co->TrainerTemplateId ? co->TrainerTemplateId : co->Entry;
                            TrainerSpellData const* trainerSpells = sObjectMgr.GetNpcTrainerTemplateSpells(trainerId);
                            if (!trainerSpells)
                                trainerSpells = sObjectMgr.GetNpcTrainerSpells(trainerId);
                            if (!trainerSpells)
                                continue;
                            for (auto const& itr : trainerSpells->spellList)
                            {
                                TrainerSpell const& tSpell = itr.second;
                                SpellEntry const* teach = sServerFacade.LookupSpellInfo(tSpell.spell);
                                if (!teach)
                                    continue;
                                for (int j = 0; j < 3; ++j)
                                    if (teach->Effect[j] == SPELL_EFFECT_LEARN_SPELL && teach->EffectTriggerSpell[j])
                                        trainerList.push_back(std::make_pair(tSpell.reqLevel, teach->EffectTriggerSpell[j]));
                            }
                        }
                        if (trainerList.empty())
                            trainerList.push_back(std::make_pair(255u, 0u));
                    }
                    uint32 learned = 0;
                    for (auto const& entry : trainerList)
                    {
                        if (silvermoonFreeSpells && entry.second && entry.first <= p->GetLevel() && !p->HasSpell(entry.second))
                        {
                            p->learnSpell(entry.second, false);
                            ++learned;
                        }
                    }

                    // a one-time gear upgrade with white (common) items only: slots are only
                    // replaced when the white item is better than what the bot wears
                    uint32 before[EQUIPMENT_SLOT_END];
                    for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
                    {
                        Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                        before[slot] = item ? item->GetEntry() : 0;
                    }
                    SilvermoonVendorGear(p);
                    uint32 changed = 0;
                    for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
                    {
                        Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                        if ((item ? item->GetEntry() : 0) != before[slot])
                            ++changed;
                    }
                    p->GetPlayerbotAI()->DoSpecificAction("auto talents", Event(), true);
                    sLog.outBasic("Silvermoon: backfill %s (level %u): learned %u trainer spells, %u gear slots upgraded",
                        p->GetName(), p->GetLevel(), learned, changed);
                    p->SaveToDB();
                });
            }

            // Silvermoon patch: break up all-bot raid groups out in the open world (left
            // over from before the 5-member cap): they get almost no XP or quest credit.
            {
                std::vector<Group*> toDisband;
                ForEachPlayerbot([&](Player* p)
                {
                    Group* group = p ? p->GetGroup() : nullptr;
                    if (!group || !group->IsRaidGroup() || group->GetLeaderGuid() != p->GetObjectGuid() || !IsRandomBot(p))
                        return;
                    if (p->InBattleGround() || (p->GetMap() && p->GetMap()->IsRaid()))
                        return;
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* member = ref->getSource();
                        if (member && (!member->GetPlayerbotAI() || member->GetPlayerbotAI()->IsRealPlayer()))
                            return;
                    }
                    toDisband.push_back(group);
                });
                for (Group* group : toDisband)
                {
                    sLog.outBasic("Silvermoon: disbanding an all-bot open-world raid group of %u", group->GetMembersCount());
                    group->Disband(true);
                }
            }

            // Silvermoon patch: bots found guilds directly, without a charter, for a flat 10
            // silver. Every 10 minutes each guildless random bot of level 10+ may decide to
            // found one; the chance grows with level, is higher when its faction has few guilds
            // online, and triples for bots whose diary says they want a guild (the website
            // records those in silvermoon_guild_wish).
            {
                static time_t lastFounding = 0;
                if (nowTime - lastFounding >= 600)
                {
                    lastFounding = nowTime;
                    std::set<uint32> wishes;
                    if (auto result = CharacterDatabase.Query("SELECT `guid` FROM `silvermoon_guild_wish`"))
                    {
                        do
                            wishes.insert(result->Fetch()[0].GetUInt32());
                        while (result->NextRow());
                    }
                    // Recruiting first. The leader of a young guild (fewer than 10 members) takes in
                    // up to two guildless random bots of its faction that are in the same zone, every
                    // 10 minutes. (The bots' own invite logic refuses guilds much smaller than a bot
                    // likes, so a freshly founded one-member guild never grew.)
                    uint32 youngGuilds[2] = { 0, 0 }; // per faction: guilds still under 10 members
                    {
                        std::set<uint32> seenGuilds;
                        std::vector<Player*> leaders;
                        ForEachPlayerbot([&](Player* p)
                        {
                            if (!p || !p->IsInWorld() || !p->GetGuildId())
                                return;
                            Guild* guild = sGuildMgr.GetGuildById(p->GetGuildId());
                            if (!guild || guild->GetMemberSize() >= 10)
                                return;
                            if (seenGuilds.insert(p->GetGuildId()).second)
                                ++youngGuilds[p->GetTeam() == ALLIANCE ? 0 : 1];
                            if (guild->GetLeaderGuid() == p->GetObjectGuid() && IsRandomBot(p))
                                leaders.push_back(p);
                        });
                        for (Player* leader : leaders)
                        {
                            Guild* guild = sGuildMgr.GetGuildById(leader->GetGuildId());
                            if (!guild)
                                continue;
                            uint32 taken = 0;
                            ForEachPlayerbot([&](Player* q)
                            {
                                if (taken >= 2 || guild->GetMemberSize() >= 10 || !q || q == leader || !q->IsInWorld() || !IsRandomBot(q) ||
                                    q->GetGuildId() || q->GetGuildIdInvited() || q->GetLevel() < 5 || q->GetTeam() != leader->GetTeam() ||
                                    q->GetMapId() != leader->GetMapId() || q->GetZoneId() != leader->GetZoneId() ||
                                    !q->GetPlayerbotAI() || q->GetPlayerbotAI()->HasRealPlayerMaster() ||
                                    q->GetPlayerbotAI()->GetGuilderType() == GuilderType::SOLO)
                                    return;
                                if (guild->AddMember(q->GetObjectGuid(), guild->GetLowestRank()))
                                {
                                    ++taken;
                                    sLog.outBasic("Silvermoon: bot %s joined the guild <%s> of %s (%u members)", q->GetName(), guild->GetName().c_str(), leader->GetName(), guild->GetMemberSize());
                                }
                            });
                        }
                    }
                    // Founding: none while the faction already has 3 or more young guilds still
                    // looking for members, and at most one new guild per faction per round.
                    std::vector<Player*> founders;
                    uint32 foundersOf[2] = { 0, 0 };
                    ForEachPlayerbot([&](Player* p)
                    {
                        if (p && (youngGuilds[p->GetTeam() == ALLIANCE ? 0 : 1] >= 3 || foundersOf[p->GetTeam() == ALLIANCE ? 0 : 1] >= 1))
                            return;
                        if (!p || !p->IsInWorld() || !IsRandomBot(p) || p->GetGuildId() || p->GetGuildIdInvited() ||
                            p->GetLevel() < 10 || p->GetMoney() < 1000 || !p->GetPlayerbotAI() || p->GetPlayerbotAI()->HasRealPlayerMaster())
                            return;
                        // per-check chance in tenths of a percent: 0.5% at 10-14, 1% at 15-19,
                        // 2% at 20-29, 3% at 30+
                        uint32 chance = p->GetLevel() >= 30 ? 30 : p->GetLevel() >= 20 ? 20 : p->GetLevel() >= 15 ? 10 : 5;
                        uint32 online = g_silvermoonOnlineGuilds[p->GetTeam() == ALLIANCE ? 0 : 1];
                        if (online == 0)
                            chance *= 3;
                        else if (online < 3)
                            chance *= 2;
                        if (wishes.count(p->GetGUIDLow()))
                            chance *= 3;
                        if (urand(1, 1000) <= chance)
                        {
                            founders.push_back(p);
                            ++foundersOf[p->GetTeam() == ALLIANCE ? 0 : 1];
                        }
                    });
                    for (Player* p : founders)
                    {
                        std::string guildName = RandomPlayerbotFactory::CreateRandomGuildName();
                        if (guildName.empty() || p->GetGuildId())
                            continue;
                        Guild* guild = new Guild();
                        if (!guild->Create(p, guildName))
                        {
                            delete guild;
                            continue;
                        }
                        sGuildMgr.AddGuild(guild);
                        guild->SetEmblem(urand(0, 180), urand(0, 17), urand(0, 7), urand(0, 17), urand(0, 51));
                        p->ModifyMoney(-1000);
                        CharacterDatabase.PExecute("DELETE FROM `silvermoon_guild_wish` WHERE `guid` = '%u'", p->GetGUIDLow());
                        sLog.outBasic("Silvermoon: bot %s (level %u) founded the guild <%s>", p->GetName(), p->GetLevel(), guildName.c_str());
                    }
                }
            }

            // Silvermoon patch: get out of fights it cannot win. A random bot that is not with a
            // real player and not on a dungeon run runs away (the "flee" action) when it is
            // fighting 3 or more mobs, or 2 or more below 35% health, or anything 4+ levels above
            // it below 50% health. Checked here once a minute and in the fast loop below.
            // (see SilvermoonDisengage)

            // Silvermoon patch: bot-only groups with a wide level spread. A level 21 bot in a
            // group with level 11-13 bots follows them around low zones for hours (and the
            // level-zone teleport leaves grouped bots alone). Every 10 minutes, in a group with
            // no real player and not on a dungeon run, every bot more than 5 levels above the
            // group's lowest member leaves it.
            {
                static time_t lastSpread = 0;
                if (nowTime - lastSpread >= 600)
                {
                    lastSpread = nowTime;
                    std::vector<std::pair<Group*, Player*>> leavers;
                    std::set<Group*> seen;
                    ForEachPlayerbot([&](Player* p)
                    {
                        Group* g = p && p->IsInWorld() ? p->GetGroup() : nullptr;
                        if (!g || g->IsRaidGroup() || !seen.insert(g).second)
                            return;
                        uint32 lowest = 255;
                        std::vector<Player*> members;
                        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                        {
                            Player* m = ref->getSource();
                            if (!m)
                                continue;
                            if (!m->GetPlayerbotAI() || !IsRandomBot(m) || m->GetPlayerbotAI()->HasRealPlayerMaster() || GetEventValue(m->GetGUIDLow(), "silvermoon_dungeon"))
                                return; // a real player (or a dungeon run) is involved: leave it alone
                            lowest = std::min(lowest, uint32(m->GetLevel()));
                            members.push_back(m);
                        }
                        for (Player* m : members)
                            if (m->GetLevel() > lowest + 5)
                            {
                                leavers.push_back({ g, m }); // one per group per round: removing can disband it
                                break;
                            }
                    });
                    for (auto const& [g, m] : leavers)
                    {
                        sLog.outBasic("Silvermoon: bot %s (level %u) left a group of much lower-level bots", m->GetName(), m->GetLevel());
                        g->RemoveMember(m->GetObjectGuid(), 0);
                        SetEventValue(m->GetGUIDLow(), "silvermoon_level_zone", 0, 0); // re-check its zone soon
                    }
                }
            }

            // Silvermoon patch: server load, for the bot ramp and the website. Every 5 minutes the
            // world update time (average and worst, in ms) and the number of bots online go into
            // characters.silvermoon_perf.
            {
                static time_t lastPerf = 0;
                if (nowTime - lastPerf >= 300)
                {
                    lastPerf = nowTime;
                    uint32 online = 0;
                    ForEachPlayerbot([&](Player* p) { if (p && p->IsInWorld()) ++online; });
                    CharacterDatabase.PExecute("INSERT INTO silvermoon_perf (ts, bots, players, avg_diff, max_diff) VALUES (UNIX_TIMESTAMP(), %u, %u, %u, %u)",
                        online, uint32(GetPlayers().size()), uint32(sWorld.GetAverageDiff()), uint32(sWorld.GetMaxDiff()));
                }
            }

            // Silvermoon patch: where the time goes. Every 5 minutes, every random bot of level 15+
            // that is not with a real player or on a dungeon run is put in one bucket (dead,
            // fighting, flying, sitting to eat/drink, in a city, or travelling - by the purpose of
            // its travel target - or idle) and the counts go into characters.silvermoon_activity.
            {
                static time_t lastActivity = 0;
                if (nowTime - lastActivity >= 300)
                {
                    lastActivity = nowTime;
                    std::map<std::string, uint32> buckets;
                    ForEachPlayerbot([&](Player* p)
                    {
                        if (!p || !p->IsInWorld() || p->GetLevel() < 15 || !IsRandomBot(p) || !p->GetPlayerbotAI() ||
                            p->GetPlayerbotAI()->HasRealPlayerMaster() || GetEventValue(p->GetGUIDLow(), "silvermoon_dungeon"))
                            return;
                        std::string b;
                        if (!p->IsAlive())
                            b = "dead";
                        else if (p->IsInCombat())
                            b = "combat";
                        else if (p->IsTaxiFlying())
                            b = "flying";
                        else if (p->IsSitState())
                            b = "eat_drink";
                        else if (p->GetPlayerbotAI()->IsInCapitalCity())
                            b = "city";
                        else
                        {
                            b = "idle";
                            if (TravelTarget* t = p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                                if (t->GetDestination() && t->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED && t->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE)
                                    b = "travel_" + std::to_string(uint32(t->GetDestination()->GetPurpose()));
                            if (b == "idle" && p->IsMoving())
                                b = "moving";
                        }
                        ++buckets[b];
                    });
                    for (auto const& [name, n] : buckets)
                        CharacterDatabase.PExecute("INSERT INTO silvermoon_activity (ts, bucket, bots) VALUES (UNIX_TIMESTAMP(), '%s', %u)", name.c_str(), n);
                }
            }

            // Silvermoon patch: hunter bots name their pets, like players do. Every 10 minutes,
            // a random bot hunter whose pet still carries its species' name gives it a name made
            // from two syllable lists - "Snarlfang", "Mistpaw", ...
            {
                static time_t lastPetNames = 0;
                if (nowTime - lastPetNames >= 600)
                {
                    lastPetNames = nowTime;
                    static const std::vector<std::string> first = { "Ash", "Bris", "Cin", "Dusk", "Ember", "Fen", "Gar", "Grim", "Hal", "Iron", "Jax", "Kip",
                        "Lu", "Mist", "Nib", "Ol", "Pip", "Quill", "Rus", "Snarl", "Thorn", "Umb", "Vex", "Whis", "Yip", "Zar", "Bram", "Chomp", "Dax", "Fizz",
                        "Gnash", "Hob", "Moss", "Nox", "Pounce", "Rumble", "Scruff", "Tuft", "Waggle", "Biscuit" };
                    static const std::vector<std::string> last = { "", "", "", "fang", "paw", "claw", "tail", "ears", "bit", "y", "ie", "o", "kin", "le",
                        "nose", "tooth", "whisker", "hide", "mane", "stripe" };
                    ForEachPlayerbot([&](Player* p)
                    {
                        if (!p || !p->IsInWorld() || !IsRandomBot(p) || p->getClass() != CLASS_HUNTER)
                            return;
                        Pet* pet = p->GetPet();
                        // still called by its species name ("Mottled Boar") = never named (the bots' pets
                        // come pre-marked as renamed, so the rename flag could not be used)
                        if (!pet || pet->getPetType() != HUNTER_PET || !pet->GetCharmInfo() || !pet->GetCreatureInfo() ||
                            std::string(pet->GetName()) != pet->GetCreatureInfo()->Name)
                            return;
                        std::string name = first[urand(0, first.size() - 1)] + last[urand(0, last.size() - 1)];
                        if (name.size() < 3 || name.size() > 12)
                            return;
                        std::string old = pet->GetName();
                        pet->SetName(name);
                        pet->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PET_RENAME);
                        pet->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(time(nullptr)));
                        CharacterDatabase.PExecute("UPDATE character_pet SET name = '%s', renamed = '1' WHERE owner = '%u' AND id = '%u'",
                            name.c_str(), p->GetGUIDLow(), pet->GetCharmInfo()->GetPetNumber());
                        sLog.outBasic("Silvermoon: hunter %s named its pet (%s) %s", p->GetName(), old.c_str(), name.c_str());
                    });
                }
            }

            // Silvermoon patch: broken gear for 20 minutes or more (two checks in a row) while it
            // has money: the repair trip has failed (bad path, unreachable vendor). The bot goes
            // home to its inn - every inn town has a repair npc - where the resting pass repairs it.
            {
                static time_t lastBroken = 0;
                if (nowTime - lastBroken >= 600)
                {
                    lastBroken = nowTime;
                    ForEachPlayerbot([&](Player* p)
                    {
                        if (!p || !p->IsInWorld() || !p->IsAlive() || p->IsInCombat() || p->IsTaxiFlying() || p->IsBeingTeleported() ||
                            !IsRandomBot(p) || !p->GetPlayerbotAI() || p->GetPlayerbotAI()->HasRealPlayerMaster() ||
                            GetEventValue(p->GetGUIDLow(), "silvermoon_dungeon") || !p->GetMap() || !p->GetMap()->IsContinent() || p->GetMoney() < 100)
                            return;
                        uint32 broken = 0;
                        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                                if (item->IsBroken())
                                    ++broken;
                        uint32 guid = p->GetGUIDLow();
                        if (!broken)
                        {
                            SetEventValue(guid, "silvermoon_broken", 0, 0);
                            return;
                        }
                        if (!GetEventValue(guid, "silvermoon_broken"))
                        {
                            SetEventValue(guid, "silvermoon_broken", 1, HOUR);
                            return;
                        }
                        SetEventValue(guid, "silvermoon_broken", 0, 0);
                        if (p->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) && SilvermoonRepairNearby(p))
                            return;
                        sLog.outBasic("Silvermoon: bot %s has had %u broken items for 20 minutes, going home to repair", p->GetName(), broken);
                        if (TravelTarget* t = p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                            t->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                        p->TeleportToHomebind();
                        SetEventValue(guid, "silvermoon_craft", 0, 0); // let the resting pass (repairs) run right away
                    });
                }
            }

            // Silvermoon patch: Goldshire. Every 10 minutes, when there are noticeably more
            // Alliance bots of level 5-10 in Dun Morogh than in Elwynn Forest, a few of the
            // Dun Morogh ones (not grouped, not in combat, no player nearby) are moved to
            // Goldshire and bound there.
            {
                static time_t lastBalance = 0;
                if (nowTime - lastBalance >= 600)
                {
                    lastBalance = nowTime;
                    std::vector<Player*> dunMorogh;
                    uint32 elwynn = 0;
                    ForEachPlayerbot([&](Player* p)
                    {
                        if (!p || !p->IsInWorld() || !IsRandomBot(p) || p->GetTeam() != ALLIANCE || p->GetLevel() < 5 || p->GetLevel() > 10)
                            return;
                        if (p->GetZoneId() == 12)
                            ++elwynn;
                        else if (p->GetZoneId() == 1 && p->IsAlive() && !p->IsInCombat() && !p->GetGroup() && !p->IsTaxiFlying() &&
                            p->GetPlayerbotAI() && !p->GetPlayerbotAI()->HasRealPlayerMaster() && !p->GetPlayerbotAI()->HasPlayerNearby())
                            dunMorogh.push_back(p);
                    });
                    uint32 moved = 0;
                    while (!dunMorogh.empty() && dunMorogh.size() > elwynn + 2 && moved < 6)
                    {
                        Player* p = dunMorogh.back();
                        dunMorogh.pop_back();
                        if (TravelTarget* t = p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                            t->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                        p->TeleportTo(0, -9464.0f + frand(-25.0f, 25.0f), 62.0f + frand(-25.0f, 25.0f), 56.0f, frand(0.0f, 6.28f));
                        SetEventValue(p->GetGUIDLow(), "silvermoon_bind_home", 1, 10 * MINUTE);
                        ++elwynn;
                        ++moved;
                    }
                    if (moved)
                        sLog.outBasic("Silvermoon: moved %u level 5-10 Alliance bots from Dun Morogh to Goldshire", moved);
                }
            }

            // Silvermoon patch: professions. A random bot resting in a town or city (and not on
            // a dungeon run) gets a crafting pass at most every 15 minutes; after buying
            // materials it does not buy again for 2 hours. See SilvermoonCraftPass.
            ForEachPlayerbot([&](Player* p)
            {
                if (!p || !p->IsInWorld() || !p->IsAlive() || p->IsInCombat() || p->IsBeingTeleported() || !IsRandomBot(p) || !p->GetPlayerbotAI())
                    return;
                if (!p->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || p->IsTaxiFlying())
                    return;
                uint32 guid = p->GetGUIDLow();
                if (GetEventValue(guid, "silvermoon_dungeon") || GetEventValue(guid, "silvermoon_craft"))
                    return;
                SetEventValue(guid, "silvermoon_craft", 1, (SilvermoonIsCrafter(guid) ? 8 : 15) * MINUTE);
                SilvermoonFixProfessions(p);
                SilvermoonRepairNearby(p);
                SilvermoonTrainNearby(p);
                if (SilvermoonCraftPass(p, !GetEventValue(guid, "silvermoon_craft_buy")))
                    SetEventValue(guid, "silvermoon_craft_buy", 1, 2 * HOUR);
            });

            // Silvermoon patch: bots following a player have no maintenance strategy, so
            // they never opened pouches, clams or lockboxes that clog their bags.
            ForEachPlayerbot([&](Player* p)
            {
                if (p && p->IsInWorld() && IsRandomBot(p) && p->GetPlayerbotAI() && p->GetPlayerbotAI()->HasActivePlayerMaster() &&
                    !p->IsInCombat() && !p->IsMoving())
                    p->GetPlayerbotAI()->DoSpecificAction("open random item", Event(), true);
            });

            // Silvermoon patch: a random bot grouped with a real player but without a
            // real-player master (for example, the bot invited you instead of the other
            // way round) ignores your follow, quest shares and other group commands.
            // Make the first real player in its group its master.
            ForEachPlayerbot([&](Player* p)
            {
                if (!p || !p->IsInWorld() || !IsRandomBot(p) || !p->GetGroup())
                    return;
                PlayerbotAI* pai = p->GetPlayerbotAI();
                if (!pai || pai->HasRealPlayerMaster())
                    return;
                for (GroupReference* ref = p->GetGroup()->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->getSource();
                    if (!member || member == p || !member->IsInWorld())
                        continue;
                    if (member->GetPlayerbotAI() && !member->GetPlayerbotAI()->IsRealPlayer())
                        continue;
                    sLog.outBasic("Silvermoon: bot %s is grouped with %s, making them its master", p->GetName(), member->GetName());
                    pai->SetMaster(member);
                    pai->ChangeStrategy("+" + pai->GetDefaultMovementStrategy(), BotState::BOT_STATE_NON_COMBAT);
                    pai->ResetStrategies();
                    pai->ChangeStrategy("-lfg,-bg", BotState::BOT_STATE_NON_COMBAT);
                    break;
                }
            });
            g_silvermoonOnlineGuilds[0] = allianceGuilds.size();
            g_silvermoonOnlineGuilds[1] = hordeGuilds.size();
        }
    }

#ifdef GenerateBotTests
    if (sPlayerbotAIConfig.startupRunTestsPending)
    {
        sPlayerbotAIConfig.startupRunTestsPending = false;

        for (const std::string& runTestParam : sPlayerbotAIConfig.startupRunTests)
        {
            std::list<std::string> messages = HandlePlayerbotCommand("runtest " + runTestParam, nullptr, SEC_PLAYER);
            for (const std::string& message : messages)
                sLog.outString("[Config RunTest] %s", message.c_str());
        }
    }
#endif

    if (!playersLevel)
        playersLevel = sPlayerbotAIConfig.syncLevelNoPlayer;

    ScaleBotActivity();
    if (sPlayerbotAIConfig.asyncBotLogin)
    {
        auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "AsyncBotLogin");
        sPlayerBotLoginMgr.Update(players);
        pmo.reset();
    }

    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    if (!maxAllowedBotCount || ((uint32)maxAllowedBotCount < sPlayerbotAIConfig.minRandomBots || (uint32)maxAllowedBotCount > sPlayerbotAIConfig.maxRandomBots))
    {
        maxAllowedBotCount = urand(sPlayerbotAIConfig.minRandomBots, sPlayerbotAIConfig.maxRandomBots);
        SetEventValue(0, "bot_count", maxAllowedBotCount,
            urand(sPlayerbotAIConfig.randomBotCountChangeMinInterval, sPlayerbotAIConfig.randomBotCountChangeMaxInterval));
    }

    std::list<uint32> availableBots = GetBots();    
    uint32 availableBotCount = availableBots.size();
    uint32 onlineBotCount = GetPlayerbotsAmount();
    
    SetAIInternalUpdateDelay(sPlayerbotAIConfig.randomBotUpdateInterval);

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT,
        onlineBotCount < maxAllowedBotCount ? "RandomPlayerbotMgr::Login" : "RandomPlayerbotMgr::UpdateAIInternal");

    if (time(nullptr) > (EventTimeSyncTimer + 30))
        SaveCurTime();

    if (availableBotCount < maxAllowedBotCount && !sWorld.IsShutdowning())
    {
        bool logInAllowed = true;
        if (sPlayerbotAIConfig.randomBotLoginWithPlayer)
        {
            logInAllowed = !players.empty();
        }

        if (logInAllowed)
        {
            AddRandomBots();
        }
    }

    if (sPlayerbotAIConfig.syncLevelWithPlayers && players.size())
    {
        if (time(nullptr) > (PlayersCheckTimer + 60))
            CheckPlayers();
    }

    if (sPlayerbotAIConfig.randomBotJoinLfg && players.size())
    {
        if (time(nullptr) > (LfgCheckTimer + 30))
            CheckLfgQueue();
    }

    if (sPlayerbotAIConfig.randomBotJoinBG/* && players.size()*/)
    {
        if (time(nullptr) > (BgCheckTimer + 30))
            CheckBgQueue();
    }

    if (time(nullptr) > (OfflineGroupBotsTimer + 5) && players.size())
        AddOfflineGroupBots();

    uint32 updateBots = sPlayerbotAIConfig.randomBotsPerInterval == 0 ? UINT32_MAX : sPlayerbotAIConfig.randomBotsPerInterval;

    //Update bots
    for (auto bot : availableBots)
    {
        if (GetPlayerBot(bot))
        {
            if (ProcessBot(bot))
                updateBots--;

            if (!updateBots)
                break;
        }
    }

    uint32 maxLogins = sPlayerbotAIConfig.randomBotsMaxLoginsPerInterval;

    //Log in bots
    if (sRandomPlayerbotMgr.GetDatabaseDelay("CharacterDatabase") < 10 * IN_MILLISECONDS && !sPlayerbotAIConfig.asyncBotLogin && onlineBotCount < maxAllowedBotCount && maxLogins > 0)
    {
        for (auto bot : availableBots)
        {
            if (GetPlayerBot(bot))
                continue;   

            if (!eventCache[bot].empty() && GetEventValue(bot, "login"))
            {
                onlineBotCount++;
                continue;
            }

            if (GetEventValue(bot, "login"))
                onlineBotCount++;

            if (onlineBotCount >= maxAllowedBotCount)
                break;

            if (ProcessBot(bot)) {
                --maxLogins;
            }

            if (maxLogins == 0)
                break;
        }
    }

    LoginFreeBots();

    //sLog.outString("[char %d, bot %d]", CharacterDatabase.m_threadBody->m_sqlQueue.size(), CharacterDatabase.m_threadBody->m_sqlQueue.size());
   
    LogPlayerLocation();

    DelayedFacingFix();

    MirrorAh();

    for (auto& [mapId, map] : sMapMgr.Maps())
    {
        sPerformanceMonitor.Init(map->GetId(), map->GetInstanceId());
    }

    //Ping character database.
    CharacterDatabase.AsyncPQuery(&RandomPlayerbotMgr::DatabasePing, sWorld.GetCurrentMSTime(), std::string("CharacterDatabase"), "SELECT 1");

    PlayerbotHolder::UpdateAIInternal(elapsed, minimal);
}

void RandomPlayerbotMgr::ScaleBotActivity()
{
    float activityPercentage = getActivityPercentage();

    //if (activityPercentage >= 100.0f || activityPercentage <= 0.0f) pid.reset(); //Stop integer buildup during max/min activity

    //    % increase/decrease                   wanted diff                                         , avg diff
    float activityPercentageMod = pid.calculate(sRandomPlayerbotMgr.GetPlayers().empty() ? sPlayerbotAIConfig.diffEmpty : sPlayerbotAIConfig.diffWithPlayer, sWorld.GetAverageDiff());

    activityPercentage = activityPercentageMod + 50;

    //Cap the percentage between 0 and 100.
    activityPercentage = std::max(0.0f, std::min(100.0f, activityPercentage));

    setActivityPercentage(activityPercentage);

    if (sPlayerbotAIConfig.hasLog("activity_pid.csv"))
    {
        double virtualMemUsedByMe = 0;
#if PLATFORM == PLATFORM_WINDOWS
        PROCESS_MEMORY_COUNTERS_EX pmc;
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        virtualMemUsedByMe = pmc.PrivateUsage;
#endif

        std::ostringstream out;
        out << sWorld.GetCurrentMSTime() << ", ";

        out << sWorld.GetCurrentDiff() << ",";
        out << sWorld.GetAverageDiff() << ",";
        out << sWorld.GetMaxDiff() << ",";
        out << virtualMemUsedByMe << ",";
        out << activityPercentage << ",";
        out << activityPercentageMod << ",";
        out << activeBots << ",";
        out << GetPlayerbotsAmount() << ",";

        float totalLevel = 0, totalGold = 0, totalGearscore = 0;

        if (sPlayerbotAIConfig.randomBotAutologin)
        {
            ForEachPlayerbot([&](Player* bot)
            {
                if (bot->GetPlayerbotAI()->AllowActivity())
                {
                    std::string bracket = "level:" + std::to_string(bot->GetLevel() / 10);

                    float level = bot->GetPlayerbotAI()->GetLevelFloat();
                    totalLevel += level;
                    float gold = bot->GetMoney() / 10000;
                    totalGold += gold;
                    float gearscore = bot->GetPlayerbotAI()->GetEquipGearScore(bot, false, false);
                    totalGearscore += gearscore;

                    const uint32 botGuid = bot->GetObjectGuid().GetCounter();
                    PushMetric(botPerformanceMetrics[bracket], botGuid, level);
                    PushMetric(botPerformanceMetrics["gold"], botGuid, gold);
                    PushMetric(botPerformanceMetrics["gearscore"], botGuid, gearscore);
                }
            });
        }

        out << std::fixed << std::setprecision(4);
        out << totalLevel << ",";

        for (uint8 i = 0; i < (DEFAULT_MAX_LEVEL / 10) + 1; i++)
        {
            out << GetMetricDelta(botPerformanceMetrics["level:" + std::to_string(i)]) * 12 * 60 << ",";
        }

        out << totalGold << ",";
        out << GetMetricDelta(botPerformanceMetrics["gold"]) * 12 * 60 << ",";
        out << totalGearscore << ",";
        out << GetMetricDelta(botPerformanceMetrics["gearscore"]) * 12 * 60 << ",";
        //out << CharacterDatabase.m_threadBody->m_sqlQueue.size();

        sPlayerbotAIConfig.log("activity_pid.csv", out.str().c_str());
    }
}

bool RandomPlayerbotMgr::BumpDeferredJoinTries(uint32 botGuid)
{
    uint32 tries = GetValue(botGuid, "create group tries") + 1;
    SetValue(botGuid, "create group tries", tries);

    if (tries < 300)
        return false;

    SetValue(botGuid, "create group tries", 0);
    return true;
}

void RandomPlayerbotMgr::LoginFreeBots()
{
    if (!sPlayerbotAIConfig.freeAltBots.empty() && sPlayerbotAIConfig.botAutologin != BotAutoLogin::LOGIN_ONLY_ALWAYS_ACTIVE)
    {
        std::vector<std::pair<uint32, uint32>> botsToRemove;

        for (auto [accountId, botGuid] : sPlayerbotAIConfig.freeAltBots)
        {
            ObjectGuid guid(ObjectGuid(HIGHGUID_PLAYER, botGuid));
            Player* bot = sObjectMgr.GetPlayer(guid, false);

            if (!bot)
            {
                sLog.outDetail("Add player %d", botGuid);
                AddPlayerBot(botGuid, accountId);
            }
            else if (!bot->IsBeingTeleported())
            {
                if (sRandomPlayerbotMgr.GetValue(botGuid, "create levelup"))
                {
                    PlayerbotFactory factory(bot, bot->GetLevel());
                    factory.Randomize(true, false);

                    sRandomPlayerbotMgr.SetValue(botGuid, "create levelup", 0);
                }

                Player* master = nullptr;

                if (sRandomPlayerbotMgr.GetValue(botGuid, "create group"))
                {
                    std::string groupWith = sRandomPlayerbotMgr.GetData(botGuid, "create group");

                    if (!groupWith.empty())
                    {
                        master = sObjectAccessor.FindPlayerByName(groupWith.c_str());

                        if (master)
                        {
                            Group* masterGroup = master->GetGroup();
                            bool joined = masterGroup && bot->GetGroup() == masterGroup;

                            if (!joined)
                                joined = bot->GetPlayerbotAI()->DoSpecificAction("join", Event("create group", "", master));

                            if (joined)
                            {
                                if (master->GetPlayerbotAI() && master->GetPlayerbotAI()->HasStrategy("debug heartbeat", BotState::BOT_STATE_NON_COMBAT))
                                    bot->GetPlayerbotAI()->ChangeStrategy("+debug heartbeat,+debug reactions", BotState::BOT_STATE_NON_COMBAT);
                                sRandomPlayerbotMgr.SetValue(botGuid, "create group tries", 0);
                                sRandomPlayerbotMgr.SetValue(botGuid, "create group", 0);
                            }
                            else if (sRandomPlayerbotMgr.BumpDeferredJoinTries(botGuid))
                            {
                                sRandomPlayerbotMgr.SetValue(botGuid, "create group", 0);
                            }
                        }
                        else if (sRandomPlayerbotMgr.BumpDeferredJoinTries(botGuid))
                        {
                            sRandomPlayerbotMgr.SetValue(botGuid, "create group", 0);
                        }
                    }
                    else
                    {
                        sRandomPlayerbotMgr.SetValue(botGuid, "create group", 0);
                    }
                }

                if (sRandomPlayerbotMgr.GetValue(botGuid, "create gear"))
                {
                    std::string gear = sRandomPlayerbotMgr.GetData(botGuid, "create gear");
                    if (gear == "empty")
                    {
                        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
                        {
                            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                        }
                    }

                    else if (gear == "green" || gear == "uncommon")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_UNCOMMON);
                        factory.EquipGear();
                    }
                    else if (gear == "blue" || gear == "rare")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_RARE);
                        factory.EquipGear();
                    }
                    else if (gear == "purple" || gear == "epic")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_EPIC);
                        factory.EquipGear();
                    }
                    else if (gear == "upgrade")
                    {
                        PlayerbotFactory factory(bot, master ? master->GetLevel() : bot->GetLevel(), ITEM_QUALITY_NORMAL);
                        factory.UpgradeGear(false);
                    }
                    else if (gear == "sync")
                    {
                        PlayerbotFactory factory(bot, master ? master->GetLevel() : bot->GetLevel(), ITEM_QUALITY_NORMAL);
                        factory.UpgradeGear(true);
                    }
                    else if (gear == "best")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel());
                        factory.EquipGearBest();
                    }
                    else if (gear == "partial")
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel());
                        factory.EquipGearPartialUpgrade();
                    }
                    else
                    {
                        PlayerbotFactory factory(bot, bot->GetLevel());
                        factory.EquipGear();
                    }
                }

#ifdef GenerateBotTests
                if (GetEventValue(botGuid, "test"))
                {
                    std::string testName = GetEventData(botGuid, "test");
                    testName = std::regex_replace(testName, std::regex("\\'"), "'");
                    TestRegistry::StartTest(bot->GetPlayerbotAI(), testName);

                    sRandomPlayerbotMgr.SetValue(botGuid, "test", 0);
                }
#endif

                if (!IsRandomBot(bot) && GetPlayerBot(guid)) //Place bot in player manager.
                {
                    for (auto& [mGuid, master] : players)
                    {
                        ObjectGuid masterGuid(ObjectGuid(HIGHGUID_PLAYER, mGuid));
                        if (accountId == sObjectMgr.GetPlayerAccountIdByGUID(masterGuid))
                        {
                            PlayerbotMgr* mgr = master->GetPlayerbotMgr();
                            if (mgr)
                            {
                                MovePlayerBot(guid, mgr);
                            }
                        }
                    }
                }

                if (master)
                {
                    // Only move the bot when it is genuinely not with its master. A freshly created bot is
                    // saved at the master's position - CreateBot flags a pending teleport so SaveToDB()
                    // persists that destination - so teleporting unconditionally here would be a second
                    // movement to the spot the bot is already standing on. The gate also makes the
                    // repeated per-pass teleport a no-op once the bot is in place, instead of re-issuing
                    // it every pass.
                    const bool sameMap = (bot->GetMapId() == master->GetMapId());
                    const float masterDistance = sameMap ? bot->GetDistance(master) : -1.0f;
                    if (!sameMap || masterDistance > INTERACTION_DISTANCE)
                    {
                        bot->TeleportTo(WorldPosition(master));
                    }
                }

                BotAlwaysOnline always = BotAlwaysOnline(sRandomPlayerbotMgr.GetValue(botGuid, "always"));
                if (always != BotAlwaysOnline::ACTIVE && !sRandomPlayerbotMgr.GetValue(botGuid, "create group"))
                {
                    botsToRemove.push_back({accountId, botGuid});
                }
            }
        }

        sPlayerbotAIConfig.freeAltBots.remove_if([&](const std::pair<uint32, uint32>& entry) {
            return std::find(botsToRemove.begin(), botsToRemove.end(), entry) != botsToRemove.end();
        });
    }
}

void RandomPlayerbotMgr::DelayedFacingFix()
{
    if (!sPlayerbotAIConfig.turnInRpg)
        return;

    for (auto& fMap : facingFix) {
        for (auto& fInstance : fMap.second) {
            for (auto obj : fInstance.second) {
                if (time(0) - obj.second > 5)
                {
                    if (!obj.first.IsCreature())
                        continue;

                    GuidPosition guidP(obj.first, WorldPosition(fMap.first, 0, 0, 0));

                    Creature* unit = guidP.GetCreature(fInstance.first);

                    if (!unit)
                        continue;

                    CreatureData* data = guidP.GetCreatureData();

                    if (!data)
                        continue;

                    if (unit->GetOrientation() == data->orientation)
                        continue;

                    unit->SetFacingTo(data->orientation);
                }
            }
        }
        facingFix[fMap.first].clear();
    }
}

void RandomPlayerbotMgr::DatabasePing(QueryResult* result, uint32 pingStart, std::string db)
{
    sRandomPlayerbotMgr.SetDatabaseDelay(db, sWorld.GetCurrentMSTime() - pingStart);
    delete result;
}

void RandomPlayerbotMgr::LoadNamedLocations()
{
    namedLocations.clear();

    auto result = WorldDatabase.Query("SELECT `name`, `map_id`, `position_x`, `position_y`, `position_z`, `orientation` FROM `ai_playerbot_named_location` WHERE `name` NOT LIKE 'FISH_LOCATION%'");

    if (!result)
    {
        sLog.outString(">> Loaded 0 named locations - table is empty!");
        sLog.outString();
        return;
    }

    uint32 count = 0;
    do
    {
        ++count;

        Field* fields = result->Fetch();

        std::string name = fields[0].GetCppString();
        uint32 mapId = fields[1].GetUInt32();
        float positionX = fields[2].GetFloat();
        float positionY = fields[3].GetFloat();
        float positionZ = fields[4].GetFloat();
        float orientation = fields[5].GetFloat();

        AddNamedLocation(name, WorldLocation(mapId, positionX, positionY, positionZ, orientation));
    } while (result->NextRow());

    sLog.outString(">> Loaded %u named locations", count);
    sLog.outString();
}

bool RandomPlayerbotMgr::AddNamedLocation(std::string const& name, WorldLocation const& location)
{
    if (namedLocations.find(name) != namedLocations.end())
    {
        sLog.outError("RandomPlayerbotMgr::AddNamedLocation: Failed to add named location '%s' - already exists!", name.c_str());
        return false;
    }

    namedLocations[name] = location;

    return true;
}

bool RandomPlayerbotMgr::GetNamedLocation(std::string const& name, WorldLocation& location)
{
    auto itr = namedLocations.find(name);
    if (itr == namedLocations.end())
    {
        sLog.outError("RandomPlayerbotMgr::GetNamedLocation: Named location '%s' not found! Please ensure that your ai_playerbot_named_location table is up to date.", name.c_str());
        return false;
    }

    location = itr->second;

    return true;
}

uint32 RandomPlayerbotMgr::AddRandomBots()
{
    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");    
    uint32 currentAllowedBotCount = maxAllowedBotCount;

    uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    float currentAvgLevel = 0, wantedAvgLevel = 0, randomAvgLevel = 0;

    if(sPlayerbotAIConfig.asyncBotLogin)
        return 0;
  
    if (currentBots.size() < currentAllowedBotCount)
    {
        if (sPlayerbotAIConfig.syncLevelWithPlayers)
        {
            maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel + sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)));

            wantedAvgLevel = maxLevel / 2;
            uint32 botsAmount = 0;
            ForEachPlayerbot([&](Player* bot)
            {
                currentAvgLevel += bot->GetLevel();
                botsAmount++;
            });
                

            if(currentAvgLevel)
            {
                currentAvgLevel = currentAvgLevel / botsAmount;
            }

            randomAvgLevel = (sPlayerbotAIConfig.randomBotMinLevel + std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel+ sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)))) / 2;
        }

        currentAllowedBotCount -= currentBots.size();

        int32 neededAddBots = currentAllowedBotCount;

        currentAllowedBotCount = currentAllowedBotCount*2;      

        CharacterDatabase.AllowAsyncTransactions();
        CharacterDatabase.BeginTransaction();

        bool enoughBotsForCriteria = true;

        for (uint32 noCriteria = 0; noCriteria < 3; noCriteria++)
        {
            int32  classRaceAllowed[MAX_CLASSES][MAX_RACES] = { 0 };

            for (uint32 race = 1; race < MAX_RACES; ++race)
            {
                for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
                {
                    if (sPlayerbotAIConfig.useFixedClassRaceCounts)
                    {
                        classRaceAllowed[cls][race] = sPlayerbotAIConfig.fixedClassRaceCounts[{cls, race}];
                    }
                    else
                    {
                        if (sPlayerbotAIConfig.classRaceProbability[cls][race])
                            classRaceAllowed[cls][race] = ((sPlayerbotAIConfig.classRaceProbability[cls][race] * maxAllowedBotCount / sPlayerbotAIConfig.classRaceProbabilityTotal) + 1) * (noCriteria + 1);
                    }
                }
            }

            for (std::list<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin(); i != sPlayerbotAIConfig.randomBotAccounts.end(); i++)
            {
                uint32 accountId = *i;

                std::unique_ptr<QueryResult> result;

                if (noCriteria == 2)
                {
                    result = CharacterDatabase.PQuery("SELECT guid, level, totaltime, race, class FROM characters WHERE account = '%u'", accountId);
                }
                else
                {
                    bool needToIncrease = wantedAvgLevel && currentAvgLevel + 1 < wantedAvgLevel;
                    bool needToLower = wantedAvgLevel && currentAvgLevel > wantedAvgLevel + 1;
                    bool rndCanIncrease = !sPlayerbotAIConfig.disableRandomLevels && randomAvgLevel > currentAvgLevel;
                    bool rndCanLower = !sPlayerbotAIConfig.disableRandomLevels && randomAvgLevel < currentAvgLevel;

                    std::string query = "SELECT guid, level, totaltime, race, class FROM characters WHERE account = '%u' AND level <= %u";
                    std::string wasRand = sPlayerbotAIConfig.instantRandomize ? "totaltime" : "(level > 1)";

                    if (needToIncrease) //We need more higher level bots.
                    {
                        query += " AND (level > %u";
                        if (rndCanIncrease) //Log in higher level bots or bots that will be randomized.
                            query += " OR !" + wasRand;
                        query += ")";

                        result = CharacterDatabase.PQuery(query.c_str(), accountId, maxLevel, (uint32)wantedAvgLevel);
                    }
                    else
                    {
                        if (needToLower && !rndCanLower) //Do not load unrandomized if it'll only increase level.
                            query += " AND " + wasRand;

                        result = CharacterDatabase.PQuery(query.c_str(), accountId, maxLevel);
                    }
                }

                if (!result)
                    continue;

                do
                {
                    Field* fields = result->Fetch();
                    uint32 guid = fields[0].GetUInt32();
                    uint32 level = fields[1].GetUInt32();
                    uint32 totaltime = fields[2].GetUInt32();
                    uint32 race = fields[3].GetUInt32();
                    uint32 cls = fields[4].GetUInt32();

                    if (GetEventValue(guid, "add"))
                    {
                        if (!noCriteria)
                            classRaceAllowed[cls][race]--;
                        continue;
                    }

                    if (GetEventValue(guid, "logout"))
                        continue;

                    if (GetPlayerBot(guid))
                    {
                        if (!noCriteria)
                            classRaceAllowed[cls][race]--;
                        continue;
                    }

                    if (std::find(currentBots.begin(), currentBots.end(), guid) != currentBots.end())
                    {
                        if (!noCriteria)
                            classRaceAllowed[cls][race]--;
                        continue;
                    }

                    if (classRaceAllowed[cls][race] <= 0)
                        continue;

                    SetEventValue(guid, "add", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
                    SetEventValue(guid, "logout", 0, 0);
                    currentBots.push_back(guid);

                    if(!noCriteria)
                        classRaceAllowed[cls][race]--;

                    if (wantedAvgLevel)
                    {
                        if (sPlayerbotAIConfig.instantRandomize ? totaltime : level > 1)
                            currentAvgLevel += (float)level / currentBots.size();
                        else
                            currentAvgLevel += (float)level + randomAvgLevel; //Use predicted randomized level. This will be wrong but avarage out correct.
                    }

                    currentAllowedBotCount--;
                    neededAddBots--;

                    if (!currentAllowedBotCount)
                        break;

                } while (result->NextRow());

                if (!currentAllowedBotCount)
                    break;
            }

            if (!currentAllowedBotCount)
                break;

            if (showLoginWarning && neededAddBots > 0)
            {
                sLog.outError("Not enough accounts to meet selection criteria. A random selection of bots was activated to fill the server.");

                if (sPlayerbotAIConfig.syncLevelWithPlayers)
                    sLog.outError("Only bots between level %d and %d are selected to sync with player level", uint32((currentAvgLevel + 1 < wantedAvgLevel) ? wantedAvgLevel : 1), maxLevel);

                ChatHelper chat(nullptr);

                for (uint32 race = 1; race < MAX_RACES; ++race)
                {
                    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
                    {

                            int32 moreWanted = classRaceAllowed[cls][race];
                            if (moreWanted > 0)
                            {
                                if (sPlayerbotAIConfig.useFixedClassRaceCounts)
                                {
                                    int32 totalWanted = sPlayerbotAIConfig.fixedClassRaceCounts[{cls, race}];
                                    sLog.outError("%d %s %ss needed but only %d found.", totalWanted, chat.formatRace(race).c_str(), chat.formatClass(cls).c_str(), totalWanted - moreWanted);
                                }
                                else
                                {
                                    int32 totalWanted = ((sPlayerbotAIConfig.classRaceProbability[cls][race] * maxAllowedBotCount / sPlayerbotAIConfig.classRaceProbabilityTotal) + 1);
                                    float percentage = float(sPlayerbotAIConfig.classRaceProbability[cls][race]) * 100.0f / sPlayerbotAIConfig.classRaceProbabilityTotal;
                                    sLog.outError("%d %s %ss needed to get %3.2f%% of total but only %d found.", totalWanted, chat.formatRace(race).c_str(), chat.formatClass(cls).c_str(), percentage, totalWanted - moreWanted);
                                }
                            }
                        
                    }
                }

                showLoginWarning = false;
            }
        }

        CharacterDatabase.CommitTransaction();

        if (currentAllowedBotCount)
            currentAllowedBotCount = std::max(int64(GetEventValue(0, "bot_count")) - int64(currentBots.size()), int64(0));

        if(currentAllowedBotCount && sPlayerbotAIConfig.randomBotAutoCreate && !sPlayerbotAIConfig.useFixedClassRaceCounts)
#ifdef MANGOSBOT_TWO
            sLog.outError("Not enough random bot accounts available. Need %d more!!", (uint32)ceil(currentAllowedBotCount / 10.0));
#else
            sLog.outError("Not enough random bot accounts available. Need %d more!!", (uint32)ceil(currentAllowedBotCount / 9.0));
#endif
      
    }

    return currentBots.size();
}

void RandomPlayerbotMgr::LoadBattleMastersCache()
{
    BattleMastersCache.clear();

    sLog.outString("---------------------------------------");
    sLog.outString("          Loading BattleMasters Cache  ");
    sLog.outString("---------------------------------------");
    sLog.outString();

    auto result = WorldDatabase.Query("SELECT `entry`,`bg_template` FROM `battlemaster_entry`");

    uint32 count = 0;

    if (!result)
    {
        sLog.outString(">> Loaded 0 battlemaster entries - table is empty!");
        sLog.outString();
        return;
    }

    do
    {
        ++count;

        Field* fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();
        uint32 bgTypeId = fields[1].GetUInt32();

        CreatureInfo const* bmaster = sObjectMgr.GetCreatureTemplate(entry);
        if (!bmaster)
            continue;

#ifdef MANGOS
        FactionTemplateEntry const* bmFaction = sFactionTemplateStore.LookupEntry(bmaster->FactionAlliance);
#endif
#ifdef CMANGOS
        FactionTemplateEntry const* bmFaction = sFactionTemplateStore.LookupEntry(bmaster->Faction);
#endif
        uint32 bmFactionId = bmFaction->faction;
#ifdef MANGOS
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry(bmFactionId);
#endif
#ifdef CMANGOS
#ifdef MANGOSBOT_ONE
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry<FactionEntry>(bmFactionId);
#else
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry(bmFactionId);
#endif
#endif
        uint32 bmParentTeam = bmParentFaction->team;
        Team bmTeam = TEAM_BOTH_ALLOWED;
        if (bmParentTeam == 891)
            bmTeam = ALLIANCE;
        if (bmFactionId == 189)
            bmTeam = ALLIANCE;
        if (bmParentTeam == 892)
            bmTeam = HORDE;
        if (bmFactionId == 66)
            bmTeam = HORDE;

        BattleMastersCache[bmTeam][BattleGroundTypeId(bgTypeId)].insert(BattleMastersCache[bmTeam][BattleGroundTypeId(bgTypeId)].end(), entry);
        sLog.outDetail("Cached Battmemaster #%d for BG Type %d (%s)", entry, bgTypeId, bmTeam == ALLIANCE ? "Alliance" : bmTeam == HORDE ? "Horde" : "Neutral");

    } while (result->NextRow());

    sLog.outString(">> Loaded %u battlemaster entries", count);
    sLog.outString();
}

void RandomPlayerbotMgr::CheckBgQueue()
{
    if (!BgCheckTimer)
        BgCheckTimer = time(nullptr);

    if (time(nullptr) < (BgCheckTimer + 30))
    {
        return;
    }
    else
    {
        BgCheckTimer = time(nullptr);
    }

    sLog.outDetail("Checking BG Queue...");

    for (int i = BG_BRACKET_ID_FIRST; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for (int j = BATTLEGROUND_QUEUE_AV; j < MAX_BATTLEGROUND_QUEUE_TYPES; ++j)
        {
            BgPlayers[j][i][0] = 0;
            BgPlayers[j][i][1] = 0;
            BgBots[j][i][0] = 0;
            BgBots[j][i][1] = 0;
            ArenaBots[j][i][0][0] = 0;
            ArenaBots[j][i][0][1] = 0;
            ArenaBots[j][i][1][0] = 0;
            ArenaBots[j][i][1][1] = 0;
            NeedBots[j][i][0] = false;
            NeedBots[j][i][1] = false;
        }
    }

    for (auto i : players)
    {
        Player* player = i.second;

        if (!player || !player->IsInWorld())
            continue;

        if (!player->InBattleGroundQueue())
            continue;

        if (player->InBattleGround() && player->GetBattleGround()->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        for (int i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattleGroundQueueTypeId queueTypeId = player->GetBattleGroundQueueTypeId(i);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            uint32 TeamId = player->GetTeam() == ALLIANCE ? 0 : 1;

            BattleGroundTypeId bgTypeId = sServerFacade.BgTemplateId(queueTypeId);
#ifndef MANGOSBOT_TWO
            BattleGroundBracketId bracketId = sBattleGroundMgr.GetBattleGroundBracketIdFromLevel(bgTypeId, player->GetLevel());
#endif
#ifdef MANGOSBOT_TWO
            BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
            uint32 mapId = bg->GetMapId();
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, player->GetLevel());
            if (!pvpDiff)
                continue;

            BattleGroundBracketId bracketId = pvpDiff->GetBracketId();
#endif
#ifdef MANGOSBOT_TWO
            /* to fix
            if (ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId))
            {
                BattleGroundQueue& bgQueue = sServerFacade.bgQueue(queueTypeId);
                GroupQueueInfo ginfo;
                uint32 tempT = TeamId;

                if (bgQueue.GetPlayerGroupInfoData(player->GetObjectGuid(), &ginfo))
                {
                    if (ginfo.isRated)
                    {
                        for (uint32 arena_slot = 0; arena_slot < MAX_ARENA_SLOT; ++arena_slot)
                        {
                            uint32 arena_team_id = player->GetArenaTeamId(arena_slot);
                            ArenaTeam* arenateam = sObjectMgr.GetArenaTeamById(arena_team_id);
                            if (!arenateam)
                                continue;
                            if (arenateam->GetType() != arenaType)
                                continue;

                            Rating[queueTypeId][bracketId][1] = arenateam->GetRating();
                        }
                    }
                    TeamId = ginfo.isRated ? 1 : 0;
                }
                if (player->InArena())
                {
                    if (player->GetBattleGround()->IsRated())
                        TeamId = 1;
                    else
                        TeamId = 0;
                }
                ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;
            }
         */
#endif
#ifdef MANGOSBOT_ONE
            if (ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId))
            {
                sWorld.GetBGQueue().GetMessager().AddMessage([queueTypeId, playerId = player->GetObjectGuid(), arenaType = arenaType, bracketId = bracketId, tempT = TeamId](BattleGroundQueue* bgQueue)
                    {
                        uint32 TeamId;
                        GroupQueueInfo ginfo;

                        BattleGroundQueueItem* queueItem = &bgQueue->GetBattleGroundQueue(queueTypeId);
                        Player *player = RandomPlayerbotMgr::instance().GetPlayer(playerId);

                        if (!player)
                            return;

                        if (queueItem->GetPlayerGroupInfoData(player->GetObjectGuid(), &ginfo))
                        {
                            if (ginfo.isRated)
                            {
                                for (uint32 arena_slot = 0; arena_slot < MAX_ARENA_SLOT; ++arena_slot)
                                {
                                    uint32 arena_team_id = player->GetArenaTeamId(arena_slot);
                                    ArenaTeam* arenateam = sObjectMgr.GetArenaTeamById(arena_team_id);
                                    if (!arenateam)
                                        continue;
                                    if (arenateam->GetType() != arenaType)
                                        continue;

                                    sRandomPlayerbotMgr.Rating[queueTypeId][bracketId][1] = arenateam->GetRating();
                                }
                            }
                            TeamId = ginfo.isRated ? 1 : 0;
                        }
                        if (player->InArena())
                        {
                            if (player->GetBattleGround()->IsRated()/* && (ginfo.isRated && ginfo.arenaTeamId && ginfo.arenaTeamRating && ginfo.opponentsTeamRating)*/)
                                TeamId = 1;
                            else
                                TeamId = 0;
                        }
                        sRandomPlayerbotMgr.ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;

                    }
                );
            }
#endif
            if (player->GetPlayerbotAI())
                BgBots[queueTypeId][bracketId][TeamId]++;
            else
                BgPlayers[queueTypeId][bracketId][TeamId]++;

            if (!player->IsInvitedForBattleGroundQueueType(queueTypeId) && (!player->InBattleGround() || player->GetBattleGround()->GetTypeId() != sServerFacade.BgTemplateId(queueTypeId)))
            {
#ifndef MANGOSBOT_ZERO
                if (ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId))
                {
                    NeedBots[queueTypeId][bracketId][TeamId] = true;
                }
                else
                {
                    NeedBots[queueTypeId][bracketId][0] = true;
                    NeedBots[queueTypeId][bracketId][1] = true;
                }
#else
                NeedBots[queueTypeId][bracketId][0] = true;
                NeedBots[queueTypeId][bracketId][1] = true;
#endif
            }
        }
    }

    ForEachPlayerbot([&](Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;

        if (!bot->InBattleGroundQueue())
            return;

        if (!IsFreeBot(bot))
            return;

        if (bot->InBattleGround() && bot->GetBattleGround() && bot->GetBattleGround()->GetStatus() == STATUS_WAIT_LEAVE)
            return;

        for (int i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattleGroundQueueTypeId queueTypeId = bot->GetBattleGroundQueueTypeId(i);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            uint32 TeamId = bot->GetTeam() == ALLIANCE ? 0 : 1;

            BattleGroundTypeId bgTypeId = sServerFacade.BgTemplateId(queueTypeId);

#ifndef MANGOSBOT_TWO
            BattleGroundBracketId bracketId = sBattleGroundMgr.GetBattleGroundBracketIdFromLevel(bgTypeId, bot->GetLevel());;
#endif
#ifdef MANGOSBOT_TWO
            BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
            uint32 mapId = bg->GetMapId();
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
            if (!pvpDiff)
                continue;

            BattleGroundBracketId bracketId = pvpDiff->GetBracketId();
#endif
#ifdef MANGOSBOT_TWO
            /* to fix
            ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId);
            if (arenaType != ARENA_TYPE_NONE)
            {
                BattleGroundQueue& bgQueue = sServerFacade.bgQueue(queueTypeId);
                GroupQueueInfo ginfo;
                uint32 tempT = TeamId;
                if (bgQueue.GetPlayerGroupInfoData(bot->GetObjectGuid(), &ginfo))
                {
                    TeamId = ginfo.isRated ? 1 : 0;
                }
                if (bot->InArena())
                {
                    if (bot->GetBattleGround()->IsRated())
                        TeamId = 1;
                    else
                        TeamId = 0;
                }
                ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;
            }
        */
#endif
#ifdef MANGOSBOT_ONE
            ArenaType arenaType = sServerFacade.BgArenaType(queueTypeId);
            if (arenaType != ARENA_TYPE_NONE)
            {
                sWorld.GetBGQueue().GetMessager().AddMessage([queueTypeId, botId = bot->GetObjectGuid(), arenaType = arenaType, bracketId = bracketId, tempT = TeamId](BattleGroundQueue* bgQueue)
                    {
                        uint32 TeamId;
                        GroupQueueInfo ginfo;

                        BattleGroundQueueItem* queueItem = &bgQueue->GetBattleGroundQueue(queueTypeId);
                        Player *bot = RandomPlayerbotMgr::instance().GetPlayer(botId);
                        if (!bot)
                            return;

                        if (queueItem->GetPlayerGroupInfoData(bot->GetObjectGuid(), &ginfo))
                        {
                            TeamId = ginfo.isRated ? 1 : 0;
                        }
                        if (bot->InArena())
                        {
                            if (bot->GetBattleGround()->IsRated()/* && (ginfo.isRated && ginfo.arenaTeamId && ginfo.arenaTeamRating && ginfo.opponentsTeamRating)*/)
                                TeamId = 1;
                            else
                                TeamId = 0;
                        }

                        

                        sRandomPlayerbotMgr.ArenaBots[queueTypeId][bracketId][TeamId][tempT]++;

                    }
                );
            }
#endif
            BgBots[queueTypeId][bracketId][TeamId]++;
        }
    });

    for (int i = BG_BRACKET_ID_FIRST; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for (int j = BATTLEGROUND_QUEUE_AV; j < MAX_BATTLEGROUND_QUEUE_TYPES; ++j)
        {
            BattleGroundQueueTypeId queueTypeId = BattleGroundQueueTypeId(j);

            if ((BgPlayers[j][i][0] + BgBots[j][i][0] + BgPlayers[j][i][1] + BgBots[j][i][1]) == 0)
                continue;

#ifndef MANGOSBOT_ZERO
            if (ArenaType type = sServerFacade.BgArenaType(queueTypeId))
            {
                sLog.outDetail("ARENA:%s %s: Plr (Skirmish:%d, Rated:%d) Bot (Skirmish:%d, Rated:%d) Total (Skirmish:%d Rated:%d)",
                    type == ARENA_TYPE_2v2 ? "2v2" : type == ARENA_TYPE_3v3 ? "3v3" : "5v5",
                    i == 0 ? "10-19" : i == 1 ? "20-29" : i == 2 ? "30-39" : i == 3 ? "40-49" : i == 4 ? "50-59" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 6) ? "60" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 7) ? "60-69" : i == 6 ? (i == 6 && MAX_BATTLEGROUND_BRACKETS == 16) ? "70-79" : "70" : "80",
                    BgPlayers[j][i][0],
                    BgPlayers[j][i][1],
                    BgBots[j][i][0],
                    BgBots[j][i][1],
                    BgPlayers[j][i][0] + BgBots[j][i][0],
                    BgPlayers[j][i][1] + BgBots[j][i][1]
                );
                continue;
            }
#endif
            BattleGroundTypeId bgTypeId = sServerFacade.BgTemplateId(queueTypeId);
            std::string _bgType;
            switch (bgTypeId)
            {
            case BATTLEGROUND_AV:
                _bgType = "AV";
                break;
            case BATTLEGROUND_WS:
                _bgType = "WSG";
                break;
            case BATTLEGROUND_AB:
                _bgType = "AB";
                break;
#ifndef MANGOSBOT_ZERO
            case BATTLEGROUND_EY:
                _bgType = "EotS";
                break;
#endif
#ifdef MANGOSBOT_TWO
            case BATTLEGROUND_RB:
                _bgType = "Random";
                break;
            case BATTLEGROUND_SA:
                _bgType = "SotA";
                break;
            case BATTLEGROUND_IC:
                _bgType = "IoC";
                break;
#endif
            default:
                _bgType = "Other";
                break;
            }
            sLog.outDetail("BG:%s %s: Plr (%d:%d) Bot (%d:%d) Total (A:%d H:%d)",
                _bgType.c_str(),
                i == 0 ? "10-19" : i == 1 ? "20-29" : i == 2 ? "30-39" : i == 3 ? "40-49" : i == 4 ? "50-59" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 6) ? "60" : (i == 5 && MAX_BATTLEGROUND_BRACKETS == 7) ? "60-69" : i == 6 ? (i == 6 && MAX_BATTLEGROUND_BRACKETS == 16) ? "70-79" : "70" : "80",
                BgPlayers[j][i][0],
                BgPlayers[j][i][1],
                BgBots[j][i][0],
                BgBots[j][i][1],
                BgPlayers[j][i][0] + BgBots[j][i][0],
                BgPlayers[j][i][1] + BgBots[j][i][1]
            );
        }
    }

    sLog.outDetail("BG Queue check finished");
    return;
}

void RandomPlayerbotMgr::CheckLfgQueue()
{
    if (!LfgCheckTimer || time(NULL) > (LfgCheckTimer + 30))
        LfgCheckTimer = time(NULL);

    if (sPlayerbotAIConfig.logRandomBotJoinLfg)
    {
        sLog.outDetail("Checking LFG Queue...");
    }

    // Clear LFG list
    LfgDungeons[HORDE].clear();
    LfgDungeons[ALLIANCE].clear();

    for (auto i : players)
    {
        Player* player = i.second;

        if (!player || !player->IsInWorld())
            continue;

        bool isLFG = false;

#ifdef MANGOSBOT_ZERO
        WorldSafeLocsEntry const* ClosestGrave = player->GetMap()->GetGraveyardManager().GetClosestGraveYard(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), player->GetTeam());
        uint32 zoneId = ClosestGrave ? ClosestGrave->ID : 0;

        Group* group = player->GetGroup();
        if (group)
        {
            if (sWorld.GetLFGQueue().IsGroupInQueue(group->GetId()))
            {
                isLFG = true;
                LFGGroupQueueInfo lfgInfo;
                sWorld.GetLFGQueue().GetGroupQueueInfo(&lfgInfo, group->GetId());
                uint32 lfgType = (zoneId << 16) | lfgInfo.areaId;
                LfgDungeons[player->GetTeam()].push_back(lfgType);
            }
        }
        else
        {
            if (sWorld.GetLFGQueue().IsPlayerInQueue(player->GetObjectGuid()))
            {
                isLFG = true;
                LFGPlayerQueueInfo lfgInfo;
                sWorld.GetLFGQueue().GetPlayerQueueInfo(&lfgInfo, player->GetObjectGuid());
                uint32 lfgType = (zoneId << 16) | lfgInfo.areaId;
                LfgDungeons[player->GetTeam()].push_back(lfgType);
            }
        }
#endif

#ifdef MANGOSBOT_ONE
        /* todo: Fix with new system
        WorldSafeLocsEntry const* ClosestGrave = player->GetMap()->GetGraveyardManager().GetClosestGraveYard(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), player->GetTeam());
        uint32 zoneId = ClosestGrave ? ClosestGrave->ID : 0;

        Group* group = player->GetGroup();
        if (group && !group->IsFull())
        {
            if (group->IsLeader(player->GetObjectGuid()))
            {
                if (player->GetSession()->m_lfgInfo.queued && player->GetSession()->LookingForGroup_auto_add && player->m_lookingForGroup.more.isAuto())
                {
                    uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(player->m_lookingForGroup.more.entry));
                    LfgDungeons[player->GetTeam()].push_back(lfgType);
                    isLFG = true;
                }
            }
        }
        else if (!group)
        {
            for (int i = 0; i < MAX_LOOKING_FOR_GROUP_SLOT; ++i)
                if (!player->m_lookingForGroup.group[i].empty() && player->GetSession()->LookingForGroup_auto_join && player->m_lookingForGroup.group[i].isAuto())
                {
                    isLFG = true;
                    uint32 lfgType = (zoneId << 16) | ((0 << 8) | uint8(player->m_lookingForGroup.group[i].entry));
                    LfgDungeons[player->GetTeam()].push_back(lfgType);
                }

            if (!player->m_lookingForGroup.more.empty() && player->GetSession()->LookingForGroup_auto_add && player->m_lookingForGroup.more.isAuto())
            {
                uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(player->m_lookingForGroup.more.entry));
                LfgDungeons[player->GetTeam()].push_back(lfgType);
                isLFG = true;
            }
        }
        */
#endif

#ifdef MANGOSBOT_TWO
        Group* group = player->GetGroup();
        if (group)
        {
            if (group->IsLFGGroup())
            {
                isLFG = true;
                LFGQueueData& lfgData = sWorld.GetLFGQueue().GetQueueData(group->GetObjectGuid());
                if (lfgData.GetState() != LFG_STATE_NONE && lfgData.GetState() < LFG_STATE_DUNGEON)
                {
                    LfgDungeonSet dList = lfgData.GetDungeons();
                    for (auto dungeon : dList)
                    {
                        LfgDungeons[player->GetTeam()].push_back(dungeon);
                    }
                }
            }
        }
        else
        {
            if (player->GetLfgData().GetState() != LFG_STATE_NONE)
            {
                LFGQueueData& lfgData = sWorld.GetLFGQueue().GetQueueData(player->GetObjectGuid());
                isLFG = true;
                if (lfgData.GetState() < LFG_STATE_DUNGEON)
                {
                    LfgDungeonSet dList = lfgData.GetDungeons();
                    for (auto dungeon : dList)
                    {
                        LfgDungeons[player->GetTeam()].push_back(dungeon);
                    }
                }
            }
        }
#endif
    }

#ifdef MANGOSBOT_ONE
    /* todo: Fix with new system
    ForEachPlayerbot([&](Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;

        if (LfgDungeons[bot->GetTeam()].empty())
            return;

        WorldSafeLocsEntry const* ClosestGrave = bot->GetMap()->GetGraveyardManager().GetClosestGraveYard(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(), bot->GetTeam());
        uint32 zoneId = ClosestGrave ? ClosestGrave->ID : 0;

        Group* group = bot->GetGroup();
        if (group && !group->IsFull())
        {
            if (group->IsLeader(bot->GetObjectGuid()))
            {
                if (bot->GetSession()->m_lfgInfo.queued && bot->GetSession()->m_lfgInfo.autofill)
                {
                    uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(bot->m_lookingForGroup.more.entry));
                    LfgDungeons[bot->GetTeam()].push_back(lfgType);
                }
            }
        }
        else if (!group)
        {
            if (!bot->m_lookingForGroup.more.empty() && bot->GetSession()->LookingForGroup_auto_add && bot->m_lookingForGroup.more.isAuto())
            {
                uint32 lfgType = (zoneId << 16) | ((1 << 8) | uint8(bot->m_lookingForGroup.more.entry));
                LfgDungeons[bot->GetTeam()].push_back(lfgType);
            }
        }
    });
    */
#endif

    if (sPlayerbotAIConfig.logRandomBotJoinLfg)
    {
       if (LfgDungeons[ALLIANCE].size() || LfgDungeons[HORDE].size())
            sLog.outDetail("LFG Queue check finished. There are real players in queue.");
       else
           sLog.outDetail("LFG Queue check finished. No real players in queue.");
    }
    return;
}

void RandomPlayerbotMgr::AddOfflineGroupBots()
{
    if (!OfflineGroupBotsTimer || time(NULL) > (OfflineGroupBotsTimer + 5))
        OfflineGroupBotsTimer = time(NULL);

    uint32 totalCounter = 0;
    for (const auto& i : players)
    {
        Player* player = i.second;

        if (!player || !player->IsInWorld() || !player->GetGroup())
            continue;

        Group* group = player->GetGroup();
        if (group && group->IsLeader(player->GetObjectGuid()))
        {
            std::vector<uint32> botsToAdd;
            Group::MemberSlotList const& slots = group->GetMemberSlots();
            for (Group::MemberSlotList::const_iterator i = slots.begin(); i != slots.end(); ++i)
            {
                ObjectGuid member = i->guid;
                if (member == player->GetObjectGuid())
                    continue;

                if (!IsFreeBot(member.GetCounter()))
                    continue;

                if (sObjectMgr.GetPlayer(member))
                    continue;

                if (GetPlayerBot(member))
                    continue;

                botsToAdd.push_back(member.GetCounter());
            }

            if (botsToAdd.empty())
                return;

            uint32 maxToAdd = urand(1, 5);
            uint32 counter = 0;
            for (auto& guid : botsToAdd)
            {
                if (counter >= maxToAdd)
                    break;

                if (sPlayerbotAIConfig.IsFreeAltBot(guid))
                {
                    for (auto& bot : sPlayerbotAIConfig.freeAltBots)
                    {
                        if (bot.second == guid)
                        {
                            Player* player = GetPlayerBot(bot.second);
                            if (!player)
                            {
                                AddPlayerBot(bot.second, bot.first);
                            }
                        }
                    }
                }
                else
                    AddRandomBot(guid);

                counter++;
                totalCounter++;
            }
        }
    }

    if (totalCounter)
        sLog.outDetail("Added %u offline bots from groups", totalCounter);
}

Item* RandomPlayerbotMgr::CreateTempItem(uint32 item, uint32 count, Player const* player, uint32 randomPropertyId)
{
    if (count < 1)
        return nullptr;                                        // don't create item at zero count

    if (ItemPrototype const* pProto = ObjectMgr::GetItemPrototype(item))
    {
        if (count > pProto->GetMaxStackSize())
            count = pProto->GetMaxStackSize();

        MANGOS_ASSERT(count != 0 && "pProto->Stackable == 0 but checked at loading already");

        Item* pItem = NewItemOrBag(pProto);
        if (pItem->Create(0, item, player))
        {
            pItem->SetCount(count);
            if (int32 randId = randomPropertyId ? randomPropertyId : Item::GenerateItemRandomPropertyId(item))
                pItem->SetItemRandomProperties(randId);

            return pItem;
        }
        delete pItem;
    }
    return nullptr;
}

InventoryResult RandomPlayerbotMgr::CanEquipUnseenItem(Player* player, uint8 slot, uint16& dest, uint32 item)
{
    dest = 0;
    Item* pItem = RandomPlayerbotMgr::CreateTempItem(item, 1, player);

    if (pItem)
    {
        InventoryResult result = player->CanEquipItem(slot, dest, pItem, true, false);

        pItem->RemoveFromUpdateQueueOf(player);

        if (!player->GetItemUpdateQueue().empty() && !player->GetItemUpdateQueue().back()) //Prevent queue overflow.
            player->GetItemUpdateQueue().pop_back();

        delete pItem;
        return result;
    }

    return EQUIP_ERR_ITEM_NOT_FOUND;
}

void RandomPlayerbotMgr::SaveCurTime()
{
    if (!EventTimeSyncTimer || time(NULL) > (EventTimeSyncTimer + 60))
        EventTimeSyncTimer = time(NULL);

    SetValue(uint32(0), "current_time", uint32(time(nullptr)));
}

void RandomPlayerbotMgr::SyncEventTimers()
{
    uint32 oldTime = GetValue(uint32(0), "current_time");
    if (oldTime)
    {
        uint32 curTime = time(nullptr);
        uint32 timeDiff = curTime - oldTime;
        CharacterDatabase.PExecute("UPDATE ai_playerbot_random_bots SET time = time + %u WHERE owner = 0 AND bot <> 0", timeDiff);
    }
}

void RandomPlayerbotMgr::CheckPlayers()
{
    if (!PlayersCheckTimer || time(NULL) > (PlayersCheckTimer + 60))
        PlayersCheckTimer = time(NULL);

    sLog.outDetail("Checking Players...");

    uint32 newPlayersLevel = 0;

    for (auto i : players)
    {
        Player* player = i.second;

        if (player->IsGameMaster())
            continue;

        //if (player->GetSession()->GetSecurity() > SEC_PLAYER)
        //    continue;

        if (player->GetLevel() > newPlayersLevel)
            newPlayersLevel = player->GetLevel();
    }

    if(playersLevel!= newPlayersLevel)
        sLog.outDetail("Max player level is %d, max bot level changed from %d to %d", newPlayersLevel, playersLevel, newPlayersLevel);
    else
        sLog.outDetail("Max player level is %d, max bot level set to %d", newPlayersLevel, newPlayersLevel);

    playersLevel = newPlayersLevel;

    return;
}

void RandomPlayerbotMgr::ScheduleRandomize(uint32 bot, uint32 time)
{
    SetEventValue(bot, "randomize", 1, time);
}

void RandomPlayerbotMgr::ScheduleTeleport(uint32 bot, uint32 time)
{
    if (!time)
        time = 60 + urand(sPlayerbotAIConfig.randomBotTeleportMinInterval, sPlayerbotAIConfig.randomBotTeleportMaxInterval);
    SetEventValue(bot, "teleport", 1, time);
}

void RandomPlayerbotMgr::ScheduleChangeStrategy(uint32 bot, uint32 time)
{
    if (!time)
        time = urand(sPlayerbotAIConfig.minRandomBotChangeStrategyTime, sPlayerbotAIConfig.maxRandomBotChangeStrategyTime);
    SetEventValue(bot, "change_strategy", 1, time);
}

bool RandomPlayerbotMgr::AddRandomBot(uint32 bot)
{
    Player* player = GetPlayerBot(bot);
    if (player)
        return true;

    uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER, bot));

    if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
    {
        sLog.outError("Bot #%d login fail: Not random bot!", bot);
        return false;
    }

    if (!GetEventValue(bot, "login"))
    {
        AddPlayerBot(bot, 0);
        SetEventValue(bot, "add", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        SetEventValue(bot, "logout", 0, 0);
        SetEventValue(bot, "login", 1, -1);
        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
        SetEventValue(bot, "update", 1, randomTime);
        currentBots.push_back(bot);
        sLog.outDetail("Random bot added #%d", bot);
    }

    return true;
}

void RandomPlayerbotMgr::MovePlayerBot(uint32 guid, PlayerbotHolder* newHolder)
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    players[guid] = this->GetPlayerBot(guid);
    PlayerbotHolder::MovePlayerBot(guid, newHolder);
}

bool RandomPlayerbotMgr::ProcessBot(uint32 bot)
{
    Player* player = GetPlayerBot(bot);
    if (player && sPlayerbotAIConfig.IsFreeAltBot(player))
    {
        return false;
    }

    PlayerbotAI* ai = player ? player->GetPlayerbotAI() : NULL;

#ifdef GenerateBotTests
    if (player && ai)
    {
        // Suppress the roam lifecycle (logout / randomize / strategy churn / grind teleports) for
        // scenario hosts AND their party members: a mid-run member logout leaves the group short a
        // slot and trips the "group size" abort monitor with everyone still alive in the instance.
        bool isTestProtected = false;
        AiObjectContext* ctx = ai->GetAiObjectContext();
        if (ctx && ctx->GetValue<bool>("manual bool", "is running test")->Get())
            isTestProtected = true;
        else if (Group* group = player->GetGroup())
        {
            Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
            PlayerbotAI* leaderAi = leader ? leader->GetPlayerbotAI() : NULL;
            AiObjectContext* leaderCtx = leaderAi ? leaderAi->GetAiObjectContext() : NULL;
            if (leaderCtx && leaderCtx->GetValue<bool>("manual bool", "is running test")->Get())
                isTestProtected = true;
        }

        if (isTestProtected)
            return false;
    }
#endif

    bool botsAllowedInWorld = !sPlayerbotAIConfig.randomBotLoginWithPlayer || (!players.empty() && sWorld.GetActiveSessionCount() > 0);

    bool isValid = true;
   
    if (sPlayerbotAIConfig.randomBotTimedLogout && !GetEventValue(bot, "add") && !sPlayerbotAIConfig.asyncBotLogin) // RandomBotInWorldTime is expired.
        isValid = false;
    else if(!botsAllowedInWorld)                                               // Logout if all players logged out
        isValid = false;

    // Silvermoon patch: priority bots (their class's leader on their faction) and bots on a
    // dungeon run stay online
    if (!isValid && botsAllowedInWorld && (g_silvermoonPriorityBots.count(bot) || GetEventValue(bot, "silvermoon_dungeon")))
    {
        SetEventValue(bot, "add", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        isValid = true;
    }

    //Log out bot
    if (!isValid)
    {
        if (botsAllowedInWorld && player && player->GetGroup())
        {
            SetEventValue(bot, "add", 1, 120);                                 // Delay logout for 2 minutes while in group.
            return false;
        }

        if (!player || !player->IsInWorld())
            sLog.outDetail("Bot #%d: log out", bot);
        else
            sLog.outDetail("Bot #%d %s:%d <%s>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H", player->GetLevel(), player->GetName());

        currentBots.remove(bot);
        SetEventValue(bot, "add", 0, 0);

        if (!player)
        {
            return false;
        }    

        LogoutPlayerBot(bot);

        if (sPlayerbotAIConfig.randomBotTimedOffline)
        {
            uint32 logout = GetEventValue(bot, "logout");

            if (!logout)
                SetEventValue(bot, "logout", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        }

        return false;
    }

    //Log in bot (Added in AddRandomBots)
    if (!player)
    {
        if (!botsAllowedInWorld)
            return false;

        if (GetEventValue(bot, "login"))
            return true;

        AddPlayerBot(bot, 0);

        SetEventValue(bot, "login", 1, -1); // This will be reset to 0 on server startup. Check RandomPlayerbotMgr constructor

        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
        SetEventValue(bot, "update", 1, randomTime);

        return true;
    }

    if (!player->IsInWorld() || player->IsBeingTeleported() || player->GetSession()->isLogingOut()) //Skip bots that are in limbo.
        return false;

    if(GetEventValue(bot, "login"))
        SetEventValue(bot, "login", 0, 0); //Bot is no longer loggin in.

    uint32 update = GetEventValue(bot, "update");
    //Update the bot
    if (!update)
    {
        //Clean up expired values
        if (ai && !ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
            ai->GetAiObjectContext()->ClearExpiredValues();

        // Silvermoon patch: a random bot that ends up inside a zone (or sub-area)
        // owned by the enemy faction is sent back to its home bind point, unless a
        // real player is leading it. This catches every route into enemy territory
        // (exploring, grinding, gathering, travel teleports) and runs on every bot
        // update, also when AiPlayerbot.DisableRandomLevels = 1.
        if (ai && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsAlive() && !player->IsTaxiFlying() &&
            !player->InBattleGround() && !ai->HasRealPlayerMaster() && player->GetMap() && player->GetMap()->IsContinent())
        {
            auto isEnemyTerritory = [player](uint32 zone, uint32 area)
            {
                for (uint32 id : { zone, area })
                {
                    AreaTableEntry const* entry = id ? GetAreaEntryByAreaID(id) : nullptr;
                    if (entry && ((entry->team == AREATEAM_ALLY && player->GetTeam() == HORDE) ||
                                  (entry->team == AREATEAM_HORDE && player->GetTeam() == ALLIANCE)))
                        return true;
                }
                return false;
            };

            uint32 zoneId = 0, areaId = 0;
            player->GetZoneAndAreaId(zoneId, areaId);

            // Only low-level bots are sent home from enemy territory, plus anyone inside an
            // enemy capital. From level 20 on, contested-but-enemy zones such as the
            // Barrens are normal leveling ground (Ratchet, Alliance quests there).
            AreaTableEntry const* zoneEntry = zoneId ? GetAreaEntryByAreaID(zoneId) : nullptr;
            AreaTableEntry const* areaEntry = areaId ? GetAreaEntryByAreaID(areaId) : nullptr;
            bool inCapital = (zoneEntry && (zoneEntry->flags & AREA_FLAG_CAPITAL)) || (areaEntry && (areaEntry->flags & AREA_FLAG_CAPITAL));
            // enemy starter and early zones stay off-limits at any level (their guards and
            // quest givers are not leveling content): Elwynn, Dun Morogh, Teldrassil, Westfall,
            // Loch Modan, Darkshore, Durotar, Mulgore, Tirisfal, Silverpine
            static const std::set<uint32> earlyZones = { 12, 1, 141, 40, 38, 148, 14, 215, 85, 130 };
            if (isEnemyTerritory(zoneId, areaId) && (player->GetLevel() < 20 || inCapital || earlyZones.count(zoneId)))
            {
                TravelTarget* travelTarget = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                if (travelTarget)
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);

                // If the hearthstone is bound inside enemy territory too, "home" would
                // just send the bot back there, so use the race's starting area instead
                // and rebind the hearthstone to it.
                float hx = 0.0f, hy = 0.0f, hz = 0.0f;
                uint32 hmap = 0, hzone = 0, harea = 0;
                player->GetHomebindLocation(hx, hy, hz, hmap);
                sTerrainMgr.GetZoneAndAreaId(hzone, harea, hmap, hx, hy, hz);
                PlayerInfo const* start = sObjectMgr.GetPlayerInfo(player->getRace(), player->getClass());

                if (isEnemyTerritory(hzone, harea) && start)
                {
                    sLog.outBasic("Silvermoon: bot %s (level %u) was in enemy zone %u area %u and its home is too, sending it to its starting area",
                        player->GetName(), player->GetLevel(), zoneId, areaId);
                    WorldLocation startLoc(start->mapId, start->positionX, start->positionY, start->positionZ, start->orientation);
                    player->SetHomebindToLocation(startLoc, start->areaId);
                    player->TeleportTo(start->mapId, start->positionX, start->positionY, start->positionZ, start->orientation);
                }
                else
                {
                    sLog.outBasic("Silvermoon: bot %s (level %u) was in enemy zone %u area %u, sending it home",
                        player->GetName(), player->GetLevel(), zoneId, areaId);
                    player->TeleportToHomebind();
                }
            }
        }

        // Silvermoon patch: level-appropriate zones. With DisableRandomLevels = 1 the
        // stock "teleport for level" code below never runs, so bots only leave their
        // starter zone by wandering and the leaders end up grinding grey mobs. A bot
        // whose area is well below its level is teleported to a spot that fits its
        // level. Each faction's front-runners are checked more often, with a smaller
        // margin, so they keep progressing steadily.
        if (ai && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsInWorld() && player->IsAlive() &&
            !player->IsTaxiFlying() && !player->IsBeingTeleported() && !player->InBattleGround() &&
            !player->InBattleGroundQueue() && !player->IsInCombat() && !player->GetGroup() &&
            !ai->HasRealPlayerMaster() && !ai->HasPlayerNearby() && player->GetLevel() >= 6 &&
            player->GetMap() && player->GetMap()->IsContinent() &&
            !GetEventValue(bot, "silvermoon_level_zone"))
        {
            bool frontRunner = player->GetLevel() >= g_silvermoonTopLevel[player->GetTeam() == ALLIANCE ? 0 : 1];
            SetEventValue(bot, "silvermoon_level_zone", 1, frontRunner ? urand(15 * MINUTE, 25 * MINUTE) : urand(45 * MINUTE, 90 * MINUTE));

            uint32 zoneId = 0, areaId = 0;
            player->GetZoneAndAreaId(zoneId, areaId);
            int32 areaLevel = 0;
            for (uint32 id : { areaId, zoneId })
            {
                AreaTableEntry const* entry = id ? GetAreaEntryByAreaID(id) : nullptr;
                if (entry && entry->area_level > 0)
                {
                    areaLevel = entry->area_level;
                    break;
                }
            }

            // Cities: a short visit (trainer, auction house, bank) is fine, but a bot still
            // in a capital at the next check has been idling there; send it out to level.
            bool lingering = false;
            if (ai->IsInCapitalCity())
            {
                lingering = GetEventValue(bot, "silvermoon_in_city") != 0;
                SetEventValue(bot, "silvermoon_in_city", lingering ? 0 : 1, lingering ? 0 : 3 * HOUR);
                areaLevel = 0;
            }
            else
                SetEventValue(bot, "silvermoon_in_city", 0, 0);

            int32 margin = frontRunner ? 3 : 5;
            bool outleveled = areaLevel > 0 && int32(player->GetLevel()) > areaLevel + margin;
            // An area's map level is often its town (Lakeshire 15) while the zone's mobs run
            // higher. If there are still several mobs worth XP nearby, the bot stays put.
            if (outleveled && !lingering)
            {
                std::list<Unit*> nearby;
                MaNGOS::AnyUnitInObjectRangeCheck check(player, 120.0f);
                MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
                Cell::VisitAllObjects(player, searcher, 120.0f);
                uint32 worthwhile = 0;
                for (Unit* unit : nearby)
                    if (unit->GetTypeId() == TYPEID_UNIT && unit->IsAlive() && unit->CanAttack(player) &&
                        int32(unit->GetLevel()) >= int32(player->GetLevel()) - 3 && int32(unit->GetLevel()) <= int32(player->GetLevel()) + 2)
                        ++worthwhile; // Silvermoon patch: barely-green mobs gave next to no XP and kept bots in old zones
                if (worthwhile >= 5)
                    outleveled = false;
            }
            if (lingering || outleveled)
            {
                sLog.outBasic("Silvermoon: bot %s (level %u%s) outleveled area %u (level %d), teleporting it to a level-appropriate zone",
                    player->GetName(), player->GetLevel(), frontRunner ? ", front-runner" : "", areaId, areaLevel);
                TravelTarget* travelTarget = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                if (travelTarget)
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                RandomTeleportForLevel(player, false);
                SetEventValue(bot, "silvermoon_bind_home", 1, 10 * MINUTE);
            }
        }

        // Silvermoon patch: the reverse case. A bot (alive or dead) in an area well above
        // its level - wandered in, or followed a quest too far - dies over and over. Every
        // 3 minutes: if the area is more than 4 levels above it, revive it if needed and
        // send it to a level-appropriate zone. Left alone when a real player leads it or
        // when a group member is strong enough for the area.
        if (ai && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsInWorld() && !ai->HasRealPlayerMaster() &&
            !player->IsTaxiFlying() && !player->IsBeingTeleported() && !player->InBattleGround() &&
            player->GetMap() && player->GetMap()->IsContinent() && !GetEventValue(bot, "silvermoon_too_high"))
        {
            SetEventValue(bot, "silvermoon_too_high", 1, 3 * MINUTE);
            uint32 zoneId = 0, areaId = 0;
            player->GetZoneAndAreaId(zoneId, areaId);
            int32 areaLevel = 0;
            for (uint32 id : { areaId, zoneId })
            {
                AreaTableEntry const* entry = id ? GetAreaEntryByAreaID(id) : nullptr;
                if (entry && entry->area_level > 0)
                {
                    areaLevel = entry->area_level;
                    break;
                }
            }
            bool covered = false;
            if (Group* group = player->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->getSource())
                        if (member != player && int32(member->GetLevel()) + 4 >= areaLevel)
                            covered = true;
            if (areaLevel > 0 && int32(player->GetLevel()) + 4 < areaLevel && !covered)
            {
                sLog.outBasic("Silvermoon: bot %s (level %u%s) is in area %u (level %d), far too dangerous, moving it",
                    player->GetName(), player->GetLevel(), player->IsAlive() ? "" : ", dead", areaId, areaLevel);
                if (!player->IsAlive())
                {
                    player->ResurrectPlayer(0.5f);
                    player->SpawnCorpseBones();
                }
                TravelTarget* travelTarget = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                if (travelTarget)
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                RandomTeleportForLevel(player, false);
                SetEventValue(bot, "silvermoon_bind_home", 1, 10 * MINUTE);
            }
        }

        // Silvermoon patch: once a level-zone teleport has landed, make the new area the
        // bot's home. Before, the hearthstone stayed in the starter zone (the stock inn
        // rebind never worked), so bots hearthed back to Goldshire, Razor Hill or Kharanos
        // to sell or repair and stayed there.
        if (ai && player->IsInWorld() && !player->IsBeingTeleported() && player->IsAlive() &&
            GetEventValue(bot, "silvermoon_bind_home") && player->GetMap() && player->GetMap()->IsContinent())
        {
            uint32 zoneId = 0, areaId = 0;
            player->GetZoneAndAreaId(zoneId, areaId);
            int32 hereLevel = 0;
            for (uint32 id : { areaId, zoneId })
            {
                AreaTableEntry const* entry = id ? GetAreaEntryByAreaID(id) : nullptr;
                if (entry && entry->area_level > 0)
                {
                    hereLevel = entry->area_level;
                    break;
                }
            }
            // still in the old, low-level area: the teleport has not landed yet
            if (hereLevel <= 0 || hereLevel + 5 < int32(player->GetLevel()))
                goto silvermoonBindDone;
            SetEventValue(bot, "silvermoon_bind_home", 0, 0);
            WorldLocation home(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation());
            player->SetHomebindToLocation(home, areaId ? areaId : zoneId);
            sLog.outBasic("Silvermoon: bot %s (level %u) now calls area %u home", player->GetName(), player->GetLevel(), areaId);
        }
        silvermoonBindDone:;

        //Randomize/teleport bot
        if (!sPlayerbotAIConfig.disableRandomLevels)
        {
            if (player->GetGroup() || player->IsTaxiFlying())
                return false;

            bool update = true;
            if (ai)
            {
                if (!sRandomPlayerbotMgr.IsRandomBot(player))
                    update = false;

                if (player->GetGroup() && ai->GetGroupMaster() && (!ai->GetGroupMaster()->GetPlayerbotAI() || ai->GetGroupMaster()->GetPlayerbotAI()->IsRealPlayer()))
                    update = false;

                if (ai->HasPlayerNearby())
                    update = false;
            }
            if (update)
                ProcessBot(player);
        }

        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime * 5);
        SetEventValue(bot, "update", 1, randomTime);
        return true;
    }

    return false;
}

bool RandomPlayerbotMgr::ProcessBot(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsBeingTeleported() || player->GetSession()->isLogingOut())
        return false;

    uint32 bot = player->GetGUIDLow();

    if (player->InBattleGround())
        return false;

    if (player->InBattleGroundQueue())
        return false;

    // Silvermoon patch: a bot on a dungeon run is never randomly teleported or re-rolled (its
    // travel target is kept empty during the run, which made it look idle and got it sent
    // out of the dungeon)
    if (GetEventValue(bot, "silvermoon_dungeon"))
        return false;

    // only teleport idle bots
    bool idleBot = false;
    TravelTarget* target = player->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (target)
    {
        if (target->GetTravelState() == TravelState::TRAVEL_STATE_IDLE)
            idleBot = true;
    }
    else
        idleBot = true;

    if (idleBot)
    {
        uint32 randomize = GetEventValue(bot, "randomize");
        if (!randomize)
        {
            bool randomiser = true;
            if (player->GetGuildId())
            {
                Guild* guild = sGuildMgr.GetGuildById(player->GetGuildId());
                uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guild->GetLeaderGuid());
                if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
                {
                    int32 rank = guild->GetRank(player->GetObjectGuid());
                    randomiser = rank < 4 ? false : true;
                }
            }

            if (randomiser)
            {
                Randomize(player);
                return true;
            }
        }

        uint32 changeStrategy = GetEventValue(bot, "change_strategy");
        if (!changeStrategy)
        {
            if (sPlayerbotAIConfig.enableRandomTeleports)
            {
                sLog.outDetail("Changing strategy for bot #%d %s:%d <%s>", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
                ChangeStrategy(player);
                ScheduleChangeStrategy(bot);
            }
            else
            {
                sLog.outDetail("Changing strategy for bot #%d %s:%d <%s> is supposed to happen, but enableRandomTeleports = false", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
            }
            return true;
        }

        uint32 teleport = GetEventValue(bot, "teleport");
        if (!teleport && players.size())
        {
            if (sPlayerbotAIConfig.enableRandomTeleports)
            {
                sLog.outDetail("Bot #%d %s:%d <%s>: sent to grind", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
                RandomTeleportForLevel(player, true);
                ScheduleTeleport(bot);
            }
            else
            {
                sLog.outDetail("Bot #%d %s:%d <%s>: supposed to be sent to grind, but enableRandomTeleports = false", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
            }
            return true;
        }
    }

    return false;
}

void RandomPlayerbotMgr::Revive(Player* player)
{
    uint32 bot = player->GetGUIDLow();

    //sLog.outString("Bot %d revived", bot);
    SetEventValue(bot, "dead", 0, 0);
    SetEventValue(bot, "revive", 0, 0);

    if (sServerFacade.GetDeathState(player) == CORPSE)
    {
        RandomTeleport(player);
    }
    else
    {
        RandomTeleportForLevel(player, false);
    }
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot, std::vector<WorldLocation>& locs, bool hearth, bool activeOnly)
{
    if (bot->IsBeingTeleported())
        return;

    if (bot->InBattleGround())
        return;

    if (bot->InBattleGroundQueue())
        return;

    if (bot->GetLevel() < 5)
        return;

    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetObjectGuid()))
        return;

    if (bot->IsTaxiFlying() && bot->GetPlayerbotAI()->HasPlayerNearby())
        return;

    if (locs.empty())
    {
        sLog.outError("Cannot teleport bot %s - no locations available", bot->GetName());
        return;
    }

    std::vector<WorldPosition> tlocs;
    tlocs.reserve(locs.size());

    for (auto& loc : locs)
        tlocs.push_back(WorldPosition(loc));

    // Do not teleport to maps disabled in config.
    tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [](const WorldPosition& l)
    {
        return std::find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(), l.getMapId()) == sPlayerbotAIConfig.randomBotMaps.end();
    }), tlocs.end());

    // Closer points are more likely to end up at the front. This does not touch terrain.
    tlocs = WorldPosition(bot).GetNextPoint(tlocs, 0);

    // Active-only used to call GetZoneId on every candidate, which loads map and mmap tiles.
    // Keep only points on a continent that is already created and has an active zone.
    // The specific zone is checked once, on the candidate actually teleported to.
    if (sPlayerbotAIConfig.randomBotTeleportNearPlayer && activeOnly)
    {
        tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(), [](const WorldPosition& l)
        {
            Map* tMap = sMapMgr.FindMap(l.getMapId(), 0);
            return !tMap || !tMap->IsContinent() || !tMap->HasActiveZones();
        }), tlocs.end());
    }

    if (tlocs.empty())
    {
        sLog.outError("Cannot teleport bot %s - no locations available", bot->GetName());
        return;
    }

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "RandomTeleportByLocations");

    for (size_t i = 0; i < tlocs.size(); ++i)
    {
        for (int attemtps = 0; attemtps < 3; ++attemtps)
        {
            WorldLocation loc = tlocs[i];

#ifdef MANGOSBOT_ONE
            // Teleport to Dark Portal area if event is in progress
            if (sWorldState.GetExpansion() == EXPANSION_NONE && bot->GetLevel() > 54 && urand(0, 100) > 20)
            {
                if (urand(0, 1))
                    loc = WorldLocation(uint32(0), -11772.43f, -3272.84f, -17.9f, 3.32447f);
                else
                    loc = WorldLocation(uint32(0), -11741.70f, -3130.3f, -11.7936f, 3.32447f);
            }
#endif

            float x = loc.coord_x + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) - sPlayerbotAIConfig.grindDistance / 2 : 0);
            float y = loc.coord_y + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) - sPlayerbotAIConfig.grindDistance / 2 : 0);
            float z = loc.coord_z;

            Map* map = sMapMgr.FindMap(loc.mapid, 0);
            if (!map)
                continue;

            // One area lookup per attempt, and only after the map already exists.
            // GetZoneAndAreaId on the whole candidate list was the mmap dtAlloc path.
            uint32 zoneId = 0;
            uint32 areaId = 0;
            sTerrainMgr.GetZoneAndAreaId(zoneId, areaId, loc.mapid, x, y, z);
            AreaTableEntry const* area = GetAreaEntryByAreaID(areaId);
            if (!area)
                continue;

            if (activeOnly && sPlayerbotAIConfig.randomBotTeleportNearPlayer)
            {
                uint32 activeZoneId = zoneId ? zoneId : areaId;
                if (!map->HasActiveZone(activeZoneId))
                    continue;

                if (sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmount > 0 && sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmountRadius > 0.0f)
                {
                    uint32 botsNearTeleportPoint = 0;
                    WorldPosition dest(loc.mapid, x, y, z, 0.0f);
                    ForEachPlayerbot([&](Player* otherBot)
                    {
                        if (otherBot && otherBot->IsInWorld() && !otherBot->IsBeingTeleported() && activeZoneId == otherBot->GetZoneId())
                        {
                            if (dest.fDist(WorldPosition(otherBot)) <= sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmountRadius)
                                botsNearTeleportPoint++;
                        }
                    });

                    if (botsNearTeleportPoint >= sPlayerbotAIConfig.randomBotTeleportNearPlayerMaxAmount)
                        continue;
                }
            }

            // Starter-zone and enemy-faction checks used to run on every stored location.
            if (zoneId && zoneId != areaId)
            {
                AreaTableEntry const* zone = GetAreaEntryByAreaID(zoneId);
                if (!zone)
                    continue;

                bool isEnemyZone = false;
                switch (zone->team)
                {
                case AREATEAM_ALLY:
                    isEnemyZone = bot->GetTeam() != ALLIANCE;
                    break;
                case AREATEAM_HORDE:
                    isEnemyZone = bot->GetTeam() != HORDE;
                    break;
                default:
                    break;
                }
                if (isEnemyZone && (bot->GetLevel() < 21 || (zone->flags & AREA_FLAG_CAPITAL)))
                    continue;

                if (bot->GetLevel() < 30)
                {
                    if ((zoneId == 12 || zoneId == 40) && bot->getRace() != RACE_HUMAN)
                        continue;
                    if ((zoneId == 1 || zoneId == 38) && bot->getRace() != RACE_DWARF)
                        continue;
                    if ((zoneId == 85 || zoneId == 130) && bot->getRace() != RACE_UNDEAD)
                        continue;
                    if ((zoneId == 141 || zoneId == 148) && bot->getRace() != RACE_NIGHTELF)
                        continue;
                    if ((zoneId == 14 || zoneId == 17) && !(bot->getRace() == RACE_ORC || bot->getRace() == RACE_TROLL))
                        continue;
                    if (zoneId == 215 && bot->getRace() != RACE_TAUREN)
                        continue;
                    if ((zoneId == 44 || zoneId == 10) && bot->GetTeam() != ALLIANCE)
                        continue;
#ifndef MANGOSBOT_ZERO
                    if ((zoneId == 3524 || zoneId == 3525) && bot->getRace() != RACE_DRAENEI)
                        continue;
                    if ((zoneId == 3430 || zoneId == 3433) && bot->getRace() != RACE_BLOODELF)
                        continue;
#endif
                }
            }

            bool isEnemyArea = false;
            switch (area->team)
            {
            case AREATEAM_ALLY:
                isEnemyArea = bot->GetTeam() != ALLIANCE;
                break;
            case AREATEAM_HORDE:
                isEnemyArea = bot->GetTeam() != HORDE;
                break;
            default:
                break;
            }
            if (isEnemyArea && bot->GetLevel() < 21)
                continue;

#ifndef MANGOSBOT_ZERO
            // Do not teleport to outland before portal opening (allow new races zones)
            if (sWorldState.GetExpansion() == EXPANSION_NONE && (loc.mapid == 571 || (loc.mapid == 530 && area->team != 2 && area->team != 4)))
                continue;
#endif

#ifdef MANGOSBOT_TWO
            float ground = map->GetHeight(bot->GetPhaseMask(), x, y, z + 0.5f);
#else
            float ground = map->GetHeight(x, y, z + 0.5f);
#endif
            if (ground <= INVALID_HEIGHT)
                continue;

            z = 0.05f + ground;
            sLog.outDetail("Random teleporting bot %s to %s %f,%f,%f (%u/%zu locations)",
                bot->GetName(), area->area_name[0], x, y, z, attemtps, tlocs.size());

            if (bot->IsTaxiFlying())
                bot->GetMotionMaster()->MovementExpired();

            if (hearth)
                bot->SetHomebindToLocation(loc, area->ID);

            bot->GetMotionMaster()->Clear();
            bot->TeleportTo(loc.mapid, x, y, z, 0);
            bot->SendHeartBeat();
            bot->GetPlayerbotAI()->Reset(true);

            if (bot->GetGroup())
            {
                for (GroupReference* gref = bot->GetGroup()->GetFirstMember(); gref; gref = gref->next())
                {
                    Player* member = gref->getSource();
                    if (!member || member == bot)
                        continue;

                    // RandomTeleport is only reached from ProcessBot, which runs on the world
                    // thread with every map worker parked, so members can be touched directly
                    // even when they are on another map.
                    if (member->IsTaxiFlying())
                        member->GetMotionMaster()->MovementExpired();
                    if (hearth)
                        member->SetHomebindToLocation(loc, area->ID);

                    member->GetMotionMaster()->Clear();
                    member->TeleportTo(loc.mapid, x, y, z, 0);
                    member->SendHeartBeat();
                    if (PlayerbotAI* memberAi = member->GetPlayerbotAI())
                        memberAi->Reset(true);
                }
            }
            return;
        }
    }

    sLog.outError("Cannot teleport bot %s - no locations available", bot->GetName());
}

std::vector<std::pair<uint32, uint32>> RandomPlayerbotMgr::RpgLocationsNear(WorldLocation pos, const std::map<uint32, std::map<uint32, std::vector<std::string>>>& areaNames, uint32 radius)
{
    std::vector<std::pair<uint32, uint32>> results;
    float minDist = FLT_MAX;
    WorldPosition areaPos(pos);
    std::string hasZone = "-", wantZone = areaPos.getAreaName(true, true);

    for (uint32 level = 1; level < sPlayerbotAIConfig.randomBotMaxLevel + 1; level++)
    {
        for (uint32 r = 1; r < MAX_RACES; r++)
        {
            uint32 i = 0;
            for (auto p : rpgLocsCacheLevel[r][level])
            {
                std::string currentZone = areaNames.at(level).at(r)[i];
                i++;

                if (currentZone != wantZone && hasZone == wantZone) //If we already have the right id but this location isn't in the right id. Skip it.
                    continue;

                if (currentZone == wantZone && hasZone != wantZone) //If this is the first spot with a good area id use this now.
                    minDist = FLT_MAX;

                float dist = WorldPosition(pos).fDist(p);

                if (dist > radius || dist > minDist)
                    continue;

                if (dist < minDist)
                    results.clear();

                results.push_back(std::make_pair(r, level));

                hasZone = currentZone;

                minDist = dist;
            }
        }
    }

    return results;
}

void RandomPlayerbotMgr::PrepareTeleportCache()
{
    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

    auto results = CharacterDatabase.PQuery("SELECT `map_id`, `x`, `y`, `z`, `level` FROM `ai_playerbot_tele_cache`");
    if (results)
    {
        sLog.outString("Loading random teleport caches for %d levels...", maxLevel);
        do
        {
            Field* fields = results->Fetch();
            uint16 mapId = fields[0].GetUInt16();
            float x = fields[1].GetFloat();
            float y = fields[2].GetFloat();
            float z = fields[3].GetFloat();
            uint16 level = fields[4].GetUInt16();
            WorldLocation loc(mapId, x, y, z, 0);
            locsPerLevelCache[level].push_back(loc);
        } while (results->NextRow());
    }
    else
    {
        sLog.outString("Preparing random teleport caches for %d levels...", maxLevel);
        BarGoLink bar(maxLevel);
        for (uint8 level = 1; level <= maxLevel; level++)
        {
            auto results = WorldDatabase.PQuery("SELECT `map`, `position_x`, `position_y`, `position_z` "
                "FROM (SELECT `map`, `position_x`, `position_y`, `position_z`, t.maxlevel, t.minlevel, "
                "%u - (t.maxlevel + t.minlevel) / 2 delta "
                "FROM creature c INNER JOIN creature_template t ON c.id = t.entry WHERE t.CreatureType != 8 AND t.NpcFlags = 0 AND t.Rank = 0 AND NOT (t.extraFlags & 1024 OR t.extraFlags & 65536 OR t.extraflags & 64 OR t.unitFlags & 256 OR t.unitFlags & 512) AND t.lootid != 0) q "
                "WHERE delta >= 0 AND delta <= %u AND map in (%s)",
                level,
                sPlayerbotAIConfig.randomBotTeleLevel,
                sPlayerbotAIConfig.randomBotMapsAsString.c_str()
            );
            if (results)
            {
                CharacterDatabase.BeginTransaction();
                do
                {
                    Field* fields = results->Fetch();
                    uint16 mapId = fields[0].GetUInt16();
                    float x = fields[1].GetFloat();
                    float y = fields[2].GetFloat();
                    float z = fields[3].GetFloat();
                    WorldLocation loc(mapId, x, y, z, 0);
                    locsPerLevelCache[level].push_back(loc);

                    CharacterDatabase.PExecute("INSERT INTO `ai_playerbot_tele_cache` (`level`, `map_id`, `x`, `y`, `z`) VALUES (%u, %u, %f, %f, %f)",
                        level, mapId, x, y, z);
                } while (results->NextRow());
                CharacterDatabase.CommitTransaction();
            }
            bar.step();
        }
    }

    sLog.outString("Preparing RPG teleport caches for %d factions...", sFactionTemplateStore.GetNumRows());

    results = WorldDatabase.PQuery("SELECT map, position_x, position_y, position_z, "
        "r.race, r.minl, r.maxl "
        "FROM creature c INNER JOIN ai_playerbot_rpg_races r ON c.id = r.entry "
        "WHERE r.race < 15");

    if (results)
    {
        do
        {
            for (uint32 level = 1; level < sPlayerbotAIConfig.randomBotMaxLevel + 1; level++)
            {
                Field* fields = results->Fetch();
                uint16 mapId = fields[0].GetUInt16();
                float x = fields[1].GetFloat();
                float y = fields[2].GetFloat();
                float z = fields[3].GetFloat();
                //uint32 faction = fields[4].GetUInt32();
                //string name = fields[5].GetCppString();
                uint32 race = fields[4].GetUInt32();
                uint32 minl = fields[5].GetUInt32();
                uint32 maxl = fields[6].GetUInt32();

                if (level > maxl || level < minl) continue;

                WorldLocation loc(mapId, x, y, z, 0);
                for (uint32 r = 1; r < MAX_RACES; r++)
                {
                    if (race == r || race == 0) rpgLocsCacheLevel[r][level].push_back(loc);
                }
            }
            //bar.step();
        } while (results->NextRow());
    }

    sLog.outString("Enhancing RPG teleport cache");

    std::map<uint32, std::map<uint32, std::vector<std::string>>> areaNames;

    for (uint32 level = 1; level < sPlayerbotAIConfig.randomBotMaxLevel + 1; level++)
    {
        for (uint32 r = 1; r < MAX_RACES; r++)
        {
            for (auto p : rpgLocsCacheLevel[r][level])
            {
                areaNames[level][r].push_back(WorldPosition(p).getAreaName(true, true));
            }
        }
    }

    std::vector<std::pair<std::pair<uint32, uint32>, WorldPosition>> newPoints;
    std::vector<std::pair<std::pair<uint32, uint32>, GuidPosition>> innPoints;

    //Static portals.
    for (auto& goData : WorldPosition().getGameObjectsNear(0, 0))
    {
        GuidPosition go(goData);

        auto data = sGOStorage.LookupEntry<GameObjectInfo>(go.GetEntry());

        if (!data)
            continue;

        if (data->type != GAMEOBJECT_TYPE_SPELLCASTER)
            continue;

        const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(data->spellcaster.spellId);

        if (pSpellInfo->EffectTriggerSpell[0])
            pSpellInfo = sServerFacade.LookupSpellInfo(pSpellInfo->EffectTriggerSpell[0]);

        if (pSpellInfo->Effect[0] != SPELL_EFFECT_TELEPORT_UNITS && pSpellInfo->Effect[1] != SPELL_EFFECT_TELEPORT_UNITS && pSpellInfo->Effect[2] != SPELL_EFFECT_TELEPORT_UNITS)
            continue;

        SpellTargetPosition const* pos = sSpellMgr.GetSpellTargetPosition(pSpellInfo->Id);

        if (!pos)
            continue;

        std::vector<std::pair<uint32, uint32>> ranges = RpgLocationsNear(WorldPosition(pos), areaNames);

        for (auto& range : ranges)
            newPoints.push_back(std::make_pair(std::make_pair(range.first, range.second), pos));
    }

    //Creatures.
    for (auto& creatureData : WorldPosition().getCreaturesNear(0, 0))
    {
        CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(creatureData->second.id);

        if (!cInfo)
            continue;

        if (cInfo->ExtraFlags & CREATURE_EXTRA_FLAG_INVISIBLE)
            continue;

        std::vector<uint32> allowedNpcFlags;

        allowedNpcFlags.push_back(UNIT_NPC_FLAG_BATTLEMASTER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_BANKER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_AUCTIONEER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_TRAINER);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_VENDOR);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_REPAIR);
        allowedNpcFlags.push_back(UNIT_NPC_FLAG_INNKEEPER);

        for (auto flag : allowedNpcFlags)
        {          
            if ((cInfo->NpcFlags & flag) != 0)
            {
                std::vector<std::pair<uint32, uint32>> ranges = RpgLocationsNear(WorldPosition(creatureData), areaNames);

                if (cInfo->NpcFlags & UNIT_NPC_FLAG_INNKEEPER)
                {
                    for (auto& range : ranges)
                        innPoints.push_back(std::make_pair(std::make_pair(range.first, range.second), creatureData));
                }
                else
                {
                    for (auto& range : ranges)
                        newPoints.push_back(std::make_pair(std::make_pair(range.first, range.second), creatureData));
                }
                break;
            }
        }
    }

    for (auto newPoint : newPoints)
        rpgLocsCacheLevel[newPoint.first.first][newPoint.first.second].push_back(newPoint.second);
    
    for (auto innPoint : innPoints)
        innCacheLevel[innPoint.first.first][innPoint.first.second].push_back(std::make_pair(innPoint.second, innPoint.second));
}

void RandomPlayerbotMgr::PrintTeleportCache()
{
    sPlayerbotAIConfig.openLog("telecache.csv", "w");

    for (auto& l : sRandomPlayerbotMgr.locsPerLevelCache)
    {
        uint32 level = l.first;
        for (auto& p : l.second)
        {
            std::ostringstream out;
            out << level << ",";
            WorldPosition(p).printWKT(out);
            out << "LEVEL" << ",0," << WorldPosition(p).getAreaName(true, true);
            sPlayerbotAIConfig.log("telecache.csv", out.str().c_str());
        }
    }

    for (auto r : sRandomPlayerbotMgr.rpgLocsCacheLevel)
    {
        uint32 race =  r.first;
        for (auto& l : r.second)
        {
            uint32 level = l.first;
            for (auto& p : l.second)
            {
                std::ostringstream out;
                out << level << ",";
                WorldPosition(p).printWKT(out);
                out << "RPG" << "," << race << "," << WorldPosition(p).getAreaName(true, true);
                sPlayerbotAIConfig.log("telecache.csv", out.str().c_str());
            }
        }
    }
}

void RandomPlayerbotMgr::RandomTeleportForLevel(Player* bot, bool activeOnly)
{
    if (bot->InBattleGround())
        return;

    sLog.outDetail("Preparing location to random teleporting bot %s for level %u", bot->GetName(), bot->GetLevel());
    RandomTeleport(bot, locsPerLevelCache[bot->GetLevel()], false, activeOnly);
    Refresh(bot);

    WorldPosition botPos(bot);

    ObjectGuid closestInn;
    float minDistance = -1.0f;
    for (auto& [innGuid, innPosition] : innCacheLevel[bot->getRace()][bot->GetLevel()])
    {
        float distance = botPos.sqDistance(innPosition);
        if (minDistance > 0 || distance >= minDistance)
            continue;

        minDistance = distance;
        closestInn = innGuid;
    }

    if (closestInn)
    {
        WorldPacket data(SMSG_TRAINER_BUY_SUCCEEDED, (8 + 4));
        data << closestInn;
        data << uint32(3286);                                   // Bind
        bot->GetSession()->SendPacket(data);
    }
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot)
{
    if (bot->InBattleGround())
        return;

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "RandomTeleport");
    std::vector<WorldLocation> locs;

    std::list<Unit*> targets;
    float range = sPlayerbotAIConfig.randomBotTeleportDistance;
    MaNGOS::AnyUnitInObjectRangeCheck u_check(bot, range);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(targets, u_check);
    Cell::VisitAllObjects(bot, searcher, range);

    if (!targets.empty())
    {
        for (std::list<Unit *>::iterator i = targets.begin(); i != targets.end(); ++i)
        {
            Unit* unit = *i;
            bot->SetPosition(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), 0);
            FleeManager manager(bot, sPlayerbotAIConfig.sightDistance, 0, true);
            float rx, ry, rz;
            if (manager.CalculateDestination(&rx, &ry, &rz))
            {
                WorldLocation loc(bot->GetMapId(), rx, ry, rz);
                locs.push_back(loc);
            }
        }
    }
    else
    {
        RandomTeleportForLevel(bot, true);
    }

    pmo.reset();

    Refresh(bot);
}

void RandomPlayerbotMgr::InstaRandomize(Player* bot)
{
    sRandomPlayerbotMgr.Randomize(bot);

    if(bot->GetLevel() > sWorld.getConfig(CONFIG_UINT32_START_PLAYER_LEVEL))
        sRandomPlayerbotMgr.RandomTeleportForLevel(bot, false);
}

void RandomPlayerbotMgr::Randomize(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->GetSession()->isLogingOut())
        return;

    bool initialRandom = false;
    if (bot->GetLevel() <= sPlayerbotAIConfig.randombotStartingLevel)
        initialRandom = true;
#ifdef MANGOSBOT_TWO
    else if (bot->GetLevel() < 60 && bot->getClass() == CLASS_DEATH_KNIGHT)
        initialRandom = true;
#endif

    // give bot random level if is above or below level sync
    if (!initialRandom && players.size() && sPlayerbotAIConfig.syncLevelWithPlayers)
    {
        uint32 maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel + sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)));
        if (bot->GetLevel() > maxLevel || (bot->GetLevel() + sPlayerbotAIConfig.syncLevelMaxAbove) < playersLevel)
            initialRandom = true;
    }

    if (initialRandom)
    {
        RandomizeFirst(bot);
        sLog.outDetail("Bot #%d %s:%d <%s>: gear/level randomised", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
    }
    else if (sPlayerbotAIConfig.randomGearUpgradeEnabled)
    {
        UpdateGearSpells(bot);
        sLog.outDetail("Bot #%d %s:%d <%s>: gear upgraded", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
    }
    else
    {
        // schedule randomise
        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
        SetEventValue(bot->GetGUIDLow(), "randomize", 1, randomTime);
    }

    //SetValue(bot, "version", MANGOSBOT_VERSION);
}

void RandomPlayerbotMgr::UpdateGearSpells(Player* bot)
{
    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "UpgradeGear");

    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

    uint32 lastLevel = GetValue(bot, "level");
    uint32 level = bot->GetLevel();
    PlayerbotFactory factory(bot, level);
    factory.Randomize(true, false);

    if (lastLevel != level)
        SetValue(bot, "level", level);

    // schedule randomise
    uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    SetEventValue(bot->GetGUIDLow(), "randomize", 1, randomTime);
}

void RandomPlayerbotMgr::RandomizeFirst(Player* bot)
{
    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

    // if lvl sync is enabled, max level is limited by online players lvl
    if (sPlayerbotAIConfig.syncLevelWithPlayers)
        maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel, std::min(playersLevel+ sPlayerbotAIConfig.syncLevelMaxAbove, sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL)));

    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "RandomizeFirst");
    uint32 level = urand(std::max(uint32(sWorld.getConfig(CONFIG_UINT32_START_PLAYER_LEVEL)), sPlayerbotAIConfig.randomBotMinLevel), maxLevel);

#ifdef MANGOSBOT_TWO
    if (bot->getClass() == CLASS_DEATH_KNIGHT)
        level = urand(std::max(bot->GetLevel(), sWorld.getConfig(CONFIG_UINT32_START_HEROIC_PLAYER_LEVEL)), std::max(sWorld.getConfig(CONFIG_UINT32_START_HEROIC_PLAYER_LEVEL), maxLevel));
#endif

    if (urand(0, 100) < 100 * sPlayerbotAIConfig.randomBotMaxLevelChance && level < maxLevel)
        level = maxLevel;

#ifndef MANGOSBOT_ZERO
    if (sWorldState.GetExpansion() == EXPANSION_NONE && level > 60)
        level = 60;
#endif

#ifdef MANGOSBOT_TWO
    // do not allow level down death knights
    if (bot->getClass() == CLASS_DEATH_KNIGHT && level < sWorld.getConfig(CONFIG_UINT32_START_HEROIC_PLAYER_LEVEL))
        return;

    // only randomise death knights to min lvl 60
    if (bot->getClass() == CLASS_DEATH_KNIGHT && level < 60)
        level = 60;
#endif

    if (level == sWorld.getConfig(CONFIG_UINT32_START_PLAYER_LEVEL))
        return;

    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false, false);

    // schedule randomise
    uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    SetEventValue(bot->GetGUIDLow(), "randomize", 1, randomTime);

    bool hasPlayer = bot->GetPlayerbotAI()->HasRealPlayerMaster();
    bot->GetPlayerbotAI()->Reset(!hasPlayer);

    if (bot->GetGroup() && !hasPlayer)
        bot->RemoveFromGroup();
}

uint32 RandomPlayerbotMgr::GetZoneLevel(uint16 mapId, float teleX, float teleY, float teleZ)
{
	uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

	uint32 level;
    auto results = WorldDatabase.PQuery("SELECT AVG(t.minlevel) minlevel, AVG(t.maxlevel) maxlevel FROM creature c "
            "INNER JOIN creature_template t ON c.id = t.entry "
            "WHERE map = '%u' AND minlevel > 1 AND abs(position_x - '%f') < '%u' AND abs(position_y - '%f') < '%u'",
            mapId, teleX, sPlayerbotAIConfig.randomBotTeleportDistance / 2, teleY, sPlayerbotAIConfig.randomBotTeleportDistance / 2);

    if (results)
    {
        Field* fields = results->Fetch();
        uint8 minLevel = fields[0].GetUInt8();
        uint8 maxLevel = fields[1].GetUInt8();
        level = urand(minLevel, maxLevel);
        if (level > maxLevel)
            level = maxLevel;
    }
    else
    {
        level = urand(1, maxLevel);
    }

    return level;
}

void RandomPlayerbotMgr::Refresh(Player* bot)
{
    if (bot->IsBeingTeleportedFar() || !bot->IsInWorld())
        return;

    if (sServerFacade.UnitIsDead(bot))
    {
        bot->ResurrectPlayer(1.0f);
        bot->SpawnCorpseBones();
        bot->GetPlayerbotAI()->ResetStrategies();
    }

    if (sPlayerbotAIConfig.disableRandomLevels)
        return;

    if (bot->InBattleGround())
        return;

    sLog.outDetail("Refreshing bot #%d <%s>", bot->GetGUIDLow(), bot->GetName());
    auto pmo = sPerformanceMonitor.start(PERF_MON_RNDBOT, "Refresh");

    bot->GetPlayerbotAI()->Reset();

    bot->DurabilityRepairAll(false, 1.0f
#ifndef MANGOSBOT_ZERO
        , false
#endif
    );
	bot->SetHealthPercent(100);
	bot->SetPvP(true);

    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.Refresh();

    if (bot->GetMaxPower(POWER_MANA) > 0)
        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

    if (bot->GetMaxPower(POWER_ENERGY) > 0)
        bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));

    uint32 money = bot->GetMoney();
    bot->SetMoney(money + 500 * sqrt(urand(1, bot->GetLevel() * 5)));
}

bool RandomPlayerbotMgr::IsRandomBot(Player* bot)
{
    if (bot && bot->GetPlayerbotAI())
    {
        if (bot->GetPlayerbotAI()->IsRealPlayer())
            return false;
    }
    if (bot)
    {
        if (sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
            return true;

        return IsRandomBot(bot->GetGUIDLow());
    }

    return false;
}

bool RandomPlayerbotMgr::IsRandomBot(uint32 bot)
{
    ObjectGuid guid = ObjectGuid(HIGHGUID_PLAYER, bot);
    if (sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(guid)))
        return true;

    return GetEventValue(bot, "add");
}

std::list<uint32> RandomPlayerbotMgr::GetBots()
{
    if (!currentBots.empty()) return currentBots;

    auto results = CharacterDatabase.Query(
            "SELECT bot FROM ai_playerbot_random_bots WHERE owner = 0 AND event = 'add'");

    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint32 bot = fields[0].GetUInt32();
            currentBots.push_back(bot);
        } while (results->NextRow());
    }

    return currentBots;
}

std::list<uint32> RandomPlayerbotMgr::GetBgBots(uint32 bracket)
{
    //if (!currentBgBots.empty()) return currentBgBots;

    auto results = CharacterDatabase.PQuery(
        "SELECT bot FROM ai_playerbot_random_bots WHERE event = 'bg' AND value = '%d'", bracket);
    std::list<uint32> BgBots;
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint32 bot = fields[0].GetUInt32();
            BgBots.push_back(bot);
        } while (results->NextRow());
    }

    return BgBots;
}

uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, std::string event)
{
    // load all events at once on first event load
    if (eventCache[bot].empty())
    {
        auto results = CharacterDatabase.PQuery("SELECT `event`, `value`, `time`, validIn, `data` FROM ai_playerbot_random_bots WHERE owner = 0 AND bot = '%u'", bot);
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                std::string eventName = fields[0].GetString();
                CachedEvent e;
                e.value = fields[1].GetUInt32();
                e.lastChangeTime = fields[2].GetUInt32();
                e.validIn = fields[3].GetUInt32();
                e.data = fields[4].GetString();
                eventCache[bot][eventName] = e;
            } while (results->NextRow());
        }
    }
    CachedEvent e = eventCache[bot][event];

    if ((time(0) - e.lastChangeTime) >= e.validIn && event != "specNo" && event != "specLink" && event != "init" && event != "current_time" && event != "always" && event != "selfbot")
        e.value = 0;

    return e.value;
}

int32 RandomPlayerbotMgr::GetValueValidTime(uint32 bot, std::string event)
{
    if (eventCache.find(bot) == eventCache.end())
        return 0;

    if (eventCache[bot].find(event) == eventCache[bot].end())
        return 0;

    CachedEvent e = eventCache[bot][event];

    return e.validIn-(time(0) - e.lastChangeTime);
}

std::string RandomPlayerbotMgr::GetEventData(uint32 bot, std::string event)
{
    std::string data = "";
    if (GetEventValue(bot, event))
    {
        CachedEvent e = eventCache[bot][event];
        data = e.data;
    }
    return data;
}

uint32 RandomPlayerbotMgr::SetEventValue(uint32 bot, std::string event, uint32 value, uint32 validIn, std::string data)
{
    CharacterDatabase.PExecute("DELETE FROM ai_playerbot_random_bots WHERE owner = 0 AND bot = '%u' AND event = '%s'",
            bot, event.c_str());
    if (value)
    {
        if (data != "")
        {
            CharacterDatabase.PExecute(
                "INSERT INTO ai_playerbot_random_bots (owner, bot, `time`, validIn, event, `value`, `data`) VALUES ('%u', '%u', '%u', '%u', '%s', '%u', '%s')",
                0, bot, (uint32)time(0), validIn, event.c_str(), value, data.c_str());
        }
        else
        {
            CharacterDatabase.PExecute(
                "INSERT INTO ai_playerbot_random_bots (owner, bot, `time`, validIn, event, `value`) VALUES ('%u', '%u', '%u', '%u', '%s', '%u')",
                0, bot, (uint32)time(0), validIn, event.c_str(), value);
        }
    }

    CachedEvent e(value, (uint32)time(0), validIn, data);
    eventCache[bot][event] = e;
    return value;
}

uint32 RandomPlayerbotMgr::GetValue(uint32 bot, std::string type)
{
    return GetEventValue(bot, type);
}

uint32 RandomPlayerbotMgr::GetValue(Player* bot, std::string type)
{
    return GetValue(bot->GetObjectGuid().GetCounter(), type);
}

std::string RandomPlayerbotMgr::GetData(uint32 bot, std::string type)
{
    return GetEventData(bot, type);
}

void RandomPlayerbotMgr::SetValue(uint32 bot, std::string type, uint32 value, std::string data, int32 validIn)
{
    SetEventValue(bot, type, value, validIn == -1 ? 15*24*3600 : validIn, data);
}

void RandomPlayerbotMgr::SetValue(Player* bot, std::string type, uint32 value, std::string data, int32 validIn)
{
    SetValue(bot->GetObjectGuid().GetCounter(), type, value, data, validIn);
}

bool RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.enabled)
    {
        sLog.outError("Playerbot system is currently disabled!");
        return false;
    }

    bool isRA = false;
    
    if (handler->GetSession()) //Client command
        isRA = true;
    else if (static_cast<CliHandler*>(handler) && static_cast<CliHandler*>(handler)->GetAccountId()) //RA call with account.
        isRA = true;

    if (!args || !*args)
    {
        sLog.outError("Usage: rndbot help/stats/update/reset/init/refresh/add/remove/more..");
        if (isRA)
            handler->SendSysMessage("Usage: rndbot help/stats/update/reset/init/refresh/add/remove/more..");

        std::list<std::string> messages = sRandomPlayerbotMgr.HandleHelp("");

        for (auto& msg : messages)
        {
            sLog.outString("%s", msg.c_str());
            if (isRA)
                handler->SendSysMessage(msg.c_str());
        }

        return true;
    }

    // Silvermoon patch: ".rndbot inspect <name>" lists any character's equipped gear in chat
    // as item links (any faction, any distance, online or offline).
    if (std::string(args).rfind("inspect", 0) == 0)
    {
        std::string name = std::string(args).size() > 8 ? std::string(args).substr(8) : "";
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name.empty() || !normalizePlayerName(name))
        {
            handler->SendSysMessage("Usage: .rndbot inspect <name>");
            return true;
        }
        static const char* slotNames[] = { "Head", "Neck", "Shoulders", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrists",
            "Hands", "Finger", "Finger", "Trinket", "Trinket", "Back", "Main hand", "Off hand", "Ranged", "Tabard" };
        static const char* qualityColor[] = { "ff9d9d9d", "ffffffff", "ff1eff00", "ff0070dd", "ffa335ee", "ffff8000", "ffe6cc80" };
        ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
        if (!guid)
        {
            handler->PSendSysMessage("No character named %s.", name.c_str());
            return true;
        }
        std::vector<std::pair<uint32, uint32>> gear; // slot, item entry
        uint32 level = 0;
        Player* target = sObjectMgr.GetPlayer(guid);
        if (target)
        {
            level = target->GetLevel();
            for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    gear.push_back(std::make_pair(slot, item->GetEntry()));
        }
        else
        {
            if (auto result = CharacterDatabase.PQuery("SELECT `level` FROM `characters` WHERE `guid` = '%u'", guid.GetCounter()))
                level = result->Fetch()[0].GetUInt32();
            if (auto result = CharacterDatabase.PQuery("SELECT `slot`, `item_template` FROM `character_inventory` WHERE `guid` = '%u' AND `bag` = 0 AND `slot` < %u ORDER BY `slot`", guid.GetCounter(), uint32(EQUIPMENT_SLOT_END)))
            {
                do
                    gear.push_back(std::make_pair(result->Fetch()[0].GetUInt32(), result->Fetch()[1].GetUInt32()));
                while (result->NextRow());
            }
        }
        handler->PSendSysMessage("|cff88ccff%s|r (level %u%s):", name.c_str(), level, target ? "" : ", offline");
        for (auto const& entry : gear)
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry.second);
            if (!proto || entry.first >= EQUIPMENT_SLOT_END)
                continue;
            handler->PSendSysMessage("  %s: |c%s|Hitem:%u:0:0:0|h[%s]|h|r", slotNames[entry.first],
                qualityColor[proto->Quality < 7 ? proto->Quality : 1], proto->ItemId, proto->Name1);
        }
        return true;
    }

    // Silvermoon patch: ".rndbot lootwatch all|off" - in-game loot messages for every dungeon
    // and raid on the realm (spectating a character already shows its party's loot).
    if (std::string(args).rfind("lootwatch", 0) == 0)
    {
        Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!gm)
            return true;
        bool off = std::string(args).find("off") != std::string::npos;
        {
            std::lock_guard<std::mutex> guard(g_silvermoonLootWatchLock);
            if (off)
                g_silvermoonLootWatch.erase(gm->GetGUIDLow());
            else
                g_silvermoonLootWatch[gm->GetGUIDLow()] = 0;
        }
        handler->SendSysMessage(off ? "Dungeon loot messages off." : "Dungeon loot messages on for every dungeon and raid.");
        return true;
    }

    // Silvermoon patch: ".rndbot spectate <name>" keeps the GM next to that character
    // wherever it goes (teleports along when it gets 25+ yards away or changes map).
    // ".rndbot spectate off" (or no name) stops.
    if (std::string(args).rfind("spectate", 0) == 0)
    {
        Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!gm)
            return true;
        std::string name = std::string(args).size() > 9 ? std::string(args).substr(9) : "";
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name.empty() || name == "off")
        {
            {
                std::lock_guard<std::mutex> guard(g_silvermoonLootWatchLock);
                auto watch = g_silvermoonLootWatch.find(gm->GetGUIDLow());
                if (watch != g_silvermoonLootWatch.end() && watch->second)
                    g_silvermoonLootWatch.erase(watch);
            }
            g_silvermoonSpectate.erase(gm->GetObjectGuid());
            handler->SendSysMessage("Spectating stopped.");
            return true;
        }
        if (!normalizePlayerName(name))
            return true;
        Player* target = sObjectAccessor.FindPlayerByName(name.c_str());
        if (!target || target == gm)
        {
            handler->PSendSysMessage("%s is not online.", name.c_str());
            return true;
        }
        g_silvermoonSpectate[gm->GetObjectGuid()] = target->GetObjectGuid();
        {
            // dungeon loot of the party you follow shows up in your chat
            std::lock_guard<std::mutex> guard(g_silvermoonLootWatchLock);
            g_silvermoonLootWatch[gm->GetGUIDLow()] = target->GetGUIDLow();
        }
        handler->PSendSysMessage("Spectating %s. Stop with .rndbot spectate off (tip: .gm on, .gm visible off, .gm fly on).", target->GetName());
        return true;
    }

    // Silvermoon patch: ".rndbot join <bot> <party member>" puts an online random bot into the
    // dungeon party of another bot (same faction, party not full): it joins the group, commits
    // to the same run and is brought to that member inside the party's copy of the dungeon.
    if (std::string(args).rfind("join", 0) == 0)
    {
        std::vector<std::string> words;
        split(words, std::string(args), " ");
        if (words.size() < 3 || !normalizePlayerName(words[1]) || !normalizePlayerName(words[2]))
        {
            handler->SendSysMessage("Usage: .rndbot join <bot> <party member>");
            return true;
        }
        Player* joiner = sObjectMgr.GetPlayer(sObjectMgr.GetPlayerGuidByName(words[1]));
        Player* member = sObjectMgr.GetPlayer(sObjectMgr.GetPlayerGuidByName(words[2]));
        if (!joiner || !member || !joiner->IsInWorld() || !member->IsInWorld())
        {
            handler->SendSysMessage("Both characters must be online.");
            return true;
        }
        if (!joiner->GetPlayerbotAI() || !sRandomPlayerbotMgr.IsRandomBot(joiner) || joiner->GetPlayerbotAI()->HasRealPlayerMaster())
        {
            handler->PSendSysMessage("%s is not a free random bot.", joiner->GetName());
            return true;
        }
        uint32 map = sRandomPlayerbotMgr.GetEventValue(member->GetGUIDLow(), "silvermoon_dungeon");
        Group* group = member->GetGroup();
        if (!map || !group)
        {
            handler->PSendSysMessage("%s is not in a dungeon party.", member->GetName());
            return true;
        }
        if (joiner->GetTeam() != member->GetTeam())
        {
            handler->SendSysMessage("They are not on the same faction.");
            return true;
        }
        if (joiner->GetGroup() != group)
        {
            if (group->IsFull())
            {
                handler->PSendSysMessage("%s's party is full.", member->GetName());
                return true;
            }
            if (Group* old = joiner->GetGroup())
                old->RemoveMember(joiner->GetObjectGuid(), 0);
            if (joiner->GetMapId() != map)
                joiner->UnbindInstance(map);
            if (!group->AddMember(joiner->GetObjectGuid(), joiner->GetName()))
            {
                handler->SendSysMessage("Could not add the bot to the party.");
                return true;
            }
        }
        uint32 partyId = sRandomPlayerbotMgr.GetEventValue(member->GetGUIDLow(), "silvermoon_dungeon_party");
        if (!partyId)
            partyId = member->GetGUIDLow();
        sRandomPlayerbotMgr.SetEventValue(joiner->GetGUIDLow(), "silvermoon_dungeon", map, 150 * MINUTE);
        sRandomPlayerbotMgr.SetEventValue(joiner->GetGUIDLow(), "silvermoon_dungeon_party", partyId, 150 * MINUTE);
        sRandomPlayerbotMgr.SetEventValue(joiner->GetGUIDLow(), "silvermoon_dungeon_wipes", 0, 0);
        if (!joiner->IsAlive())
        {
            joiner->ResurrectPlayer(1.0f);
            joiner->SpawnCorpseBones();
        }
        if (member->GetMapId() == map)
            joiner->TeleportTo(map, member->GetPositionX(), member->GetPositionY(), member->GetPositionZ(), member->GetOrientation());
        handler->PSendSysMessage("%s joined %s's dungeon party (%u members).", joiner->GetName(), member->GetName(), group->GetMembersCount());
        return true;
    }

    // Silvermoon patch: ".rndbot dungeon <rfc|wc|dm> <horde|alliance>" forms a party of five
    // online random bots of the right level (a tank, a healer and three others where the
    // classes allow), makes the highest-level one leader and teleports them inside.
    if (std::string(args).rfind("dungeon", 0) == 0)
    {
        std::vector<std::string> words;
        split(words, std::string(args), " ");
        auto dungeon = words.size() > 1 ? g_silvermoonDungeons.find(words[1]) : g_silvermoonDungeons.end();
        bool alliance = words.size() > 2 && words[2] == "alliance";
        if (dungeon == g_silvermoonDungeons.end())
        {
            {
                std::string keys;
                for (auto const& entry : g_silvermoonDungeons)
                    keys += (keys.empty() ? "" : "|") + entry.first;
                handler->PSendSysMessage("Usage: .rndbot dungeon <%s> <horde|alliance>", keys.c_str());
            }
            return true;
        }
        SilvermoonDungeon const& d = dungeon->second;
        std::vector<Player*> pool;
        sRandomPlayerbotMgr.ForEachPlayerbot([&](Player* p)
        {
            if (p && p->IsInWorld() && p->IsAlive() && !p->InBattleGround() && p->GetMap() && p->GetMap()->IsContinent() &&
                IsAlliance(p->getRace()) == alliance && p->GetLevel() >= d.minLevel + 2 && p->GetLevel() <= d.maxLevel && // Silvermoon: +2, the 13-14s kept dying to the Searing Blade
                p->GetPlayerbotAI() && !p->GetPlayerbotAI()->HasRealPlayerMaster() &&
                !sRandomPlayerbotMgr.SilvermoonDungeonOf(p->GetGUIDLow())) // already on a dungeon run: leave that party alone
                pool.push_back(p);
        });
        std::sort(pool.begin(), pool.end(), [](Player* a, Player* b) { return a->GetLevel() > b->GetLevel(); });
        std::vector<Player*> party;
        auto take = [&](std::function<bool(Player*)> want)
        {
            for (auto it = pool.begin(); it != pool.end(); ++it)
                if (want(*it)) { party.push_back(*it); pool.erase(it); return; }
        };
        // The tank comes first: the highest-level warrior or paladin available (the pool is
        // sorted by level), and it leads and pulls. The rest are then picked at or below the
        // tank's level where possible, so the tank is the highest level in the party; only when
        // there are not enough such bots do higher-level ones fill the remaining places.
        take([](Player* p) { return p->getClass() == CLASS_WARRIOR || p->getClass() == CLASS_PALADIN; });                           // tank
        uint32 tankLevel = party.empty() ? 255 : party.front()->GetLevel();
        auto isHealer = [](Player* p) { return p->getClass() == CLASS_PRIEST || p->getClass() == CLASS_SHAMAN || p->getClass() == CLASS_DRUID; };
        size_t before = party.size();
        take([&](Player* p) { return isHealer(p) && p->GetLevel() <= tankLevel; });                                               // healer
        if (party.size() == before)
            take(isHealer);
        while (party.size() < 5 && !pool.empty())
        {
            before = party.size();
            take([&](Player* p) { return p->GetLevel() <= tankLevel; });
            if (party.size() == before)
                take([](Player*) { return true; });
        }
        if (party.size() < 3)
        {
            handler->PSendSysMessage("Only %u suitable bots online for that dungeon.", uint32(party.size()));
            return true;
        }
        for (Player* p : party)
        {
            if (Group* old = p->GetGroup())
                old->RemoveMember(p->GetObjectGuid(), 0);
            // a fresh copy of the dungeon for the new party, so several parties can run at once
            p->UnbindInstance(d.map);
        }
        Player* leader = party.front();
        Group* group = new Group;
        if (!group->Create(leader->GetObjectGuid(), leader->GetName()))
        {
            delete group;
            handler->SendSysMessage("Could not create the group.");
            return true;
        }
        sObjectMgr.AddGroup(group);
        std::ostringstream names;
        for (Player* p : party)
        {
            if (p != leader)
                group->AddMember(p->GetObjectGuid(), p->GetName());
            names << p->GetName() << " (" << p->GetLevel() << ") ";
        }
        for (Player* p : party)
        {
            if (TravelTarget* t = p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                t->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            p->TeleportTo(d.map, d.x, d.y, d.z, d.o);
            // commit to the dungeon for up to 2.5 hours (see the dungeon watchdog)
            sRandomPlayerbotMgr.SetEventValue(p->GetGUIDLow(), "silvermoon_dungeon", d.map, 150 * MINUTE);
            sRandomPlayerbotMgr.SetEventValue(p->GetGUIDLow(), "silvermoon_dungeon_party", leader->GetGUIDLow(), 150 * MINUTE);
            sRandomPlayerbotMgr.SetEventValue(p->GetGUIDLow(), "silvermoon_dungeon_wipes", 0, 0);
            // roles: 1 = tank (the first pick, a warrior/paladin/druid), 2 = healer (the second
            // pick when it is a priest/shaman/druid/paladin), 3 = damage
            uint32 role = 3;
            uint8 cls = p->getClass();
            if (p == party.front() && (cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID))
                role = 1;
            else if (party.size() > 1 && p == party[1] && (cls == CLASS_PRIEST || cls == CLASS_SHAMAN || cls == CLASS_DRUID || cls == CLASS_PALADIN))
                role = 2;
            sRandomPlayerbotMgr.SetEventValue(p->GetGUIDLow(), "silvermoon_dungeon_role", role, 150 * MINUTE);
            SilvermoonStockUp(p);
        }
        sLog.outBasic("Silvermoon: dungeon party for map %u: %s", d.map, names.str().c_str());
        handler->PSendSysMessage("Sent into the dungeon: %s", names.str().c_str());
        return true;
    }

    std::string cmd = args;

    std::map<std::string, ConsoleCommandHandler> handlers;
    handlers["help"] = &RandomPlayerbotMgr::HandleHelp;
    handlers["reset"] = &RandomPlayerbotMgr::HandleConsoleReset;
    handlers["stats"] = &RandomPlayerbotMgr::HandleConsoleStats;
    handlers["update"] = &RandomPlayerbotMgr::HandleConsoleUpdate;
    handlers["pid "] = &RandomPlayerbotMgr::HandleConsolePid;
    handlers["diff"] = &RandomPlayerbotMgr::HandleConsoleDiff;
    handlers["diff "] = &RandomPlayerbotMgr::HandleConsoleDiff;
    handlers["grid"] = &RandomPlayerbotMgr::HandleConsoleGrid;
    handlers["mem"] = &RandomPlayerbotMgr::HandleConsoleMem;
    handlers["sample"] = &RandomPlayerbotMgr::HandleConsoleSample;
    handlers["find"] = &RandomPlayerbotMgr::HandleConsoleFind;
    handlers["history"] = &RandomPlayerbotMgr::HandleConsoleHistory;
    handlers["clean map"] = &RandomPlayerbotMgr::HandleConsoleCleanMap;
    handlers["login debug"] = &RandomPlayerbotMgr::HandleConsoleLoginDebug;
    handlers["taxtest"] = &RandomPlayerbotMgr::HandleConsoleTaxTest;
    handlers["zoneupd"] = &RandomPlayerbotMgr::HandleConsoleZoneUpd;
#ifdef GenerateBotTests
    handlers["travelsample"] = &RandomPlayerbotMgr::HandleConsoleTravelSample;
    handlers["traveltrace"] = &RandomPlayerbotMgr::HandleConsoleTravelTrace;
#endif

    for (auto& [prefix, consoleHandler] : handlers)
    {
        if (cmd.find(prefix) != 0)
            continue;

        size_t prefixLen = prefix.size();
        std::string param = cmd.size() > prefixLen + 1 ? cmd.substr(prefixLen + 1) : "";

        if (prefix == "stats")
            param = handler->GetSession() ? std::to_string(handler->GetSession()->GetPlayer()->GetObjectGuid()) : "";

        std::list<std::string> messages = (sRandomPlayerbotMgr.*consoleHandler)(param);
        for (auto& msg : messages)
        {
            sLog.outString("%s", msg.c_str());
            if(isRA)
                handler->SendSysMessage(msg.c_str());      
        }

        if (!messages.empty() && (prefix != "help" || param != "commands"))
            return true;
    }

    std::map<std::string, ConsolePlayerCommandHandler> playerHandlers;
    playerHandlers["init"] = &RandomPlayerbotMgr::HandleRandomizeFirst;
    playerHandlers["upgrade"] = &RandomPlayerbotMgr::HandleUpdateGearSpells;
    playerHandlers["refresh"] = &RandomPlayerbotMgr::HandleRefresh;
    playerHandlers["teleport"] = &RandomPlayerbotMgr::HandleRandomTeleportForLevel;
    playerHandlers["rpg"] = &RandomPlayerbotMgr::HandleRandomTeleportForRpg;
    playerHandlers["revive"] = &RandomPlayerbotMgr::HandleRevive;
    playerHandlers["grind"] = &RandomPlayerbotMgr::HandleRandomTeleport;
    playerHandlers["change_strategy"] = &RandomPlayerbotMgr::HandleChangeStrategy;
    playerHandlers["remove"] = &RandomPlayerbotMgr::HandleRemove;

    for (auto& [prefix, playerHandler] : playerHandlers)
    {
        if (cmd.find(prefix) != 0)
            continue;

        size_t prefixLen = prefix.size();
        std::string nameAndParams = cmd.size() > prefixLen + 1 ? cmd.substr(prefixLen + 1) : "";

        // Silvermoon patch: no name means all bots. The old default "%" (an SQL
        // wildcard) never matched a bot name here, so ".rndbot teleport" did nothing.
        std::string name = "";
        std::string params = "";

        if (!nameAndParams.empty())
        {
            size_t spacePos = nameAndParams.find(' ');
            if (spacePos != std::string::npos)
            {
                name = nameAndParams.substr(0, spacePos);
                params = nameAndParams.substr(spacePos + 1);
            }
            else
            {
                name = nameAndParams;
            }
        }

        sRandomPlayerbotMgr.consoleCmdParams = params;

        bool hasRandomBotCommand = false;

        ConsolePlayerCommandHandler handler_copy = playerHandler;

        sRandomPlayerbotMgr.ForEachPlayerbot([&](Player* bot) {
            std::string botName = bot->GetName();
            if (botName.find(name) == 0)
            {

                std::list<std::string> messages = (sRandomPlayerbotMgr.*handler_copy)(bot);
                for (auto& msg : messages)
                {
                    sLog.outString("%s", msg.c_str());
                    if (isRA)
                        handler->SendSysMessage(msg.c_str());
                    hasRandomBotCommand = true;
                }
            }
        });

        if (hasRandomBotCommand)
            return true;
    }

    std::list<std::string> messages = sRandomPlayerbotMgr.HandlePlayerbotCommand(args, handler->GetSession() ? handler->GetSession()->GetPlayer():nullptr, static_cast<CliHandler*>(handler) ? static_cast<CliHandler*>(handler)->GetAccessLevel() : SEC_PLAYER);
    for (std::list<std::string>::iterator i = messages.begin(); i != messages.end(); ++i)
    {
        sLog.outString("%s", i->c_str());
        if (isRA)
            handler->SendSysMessage(i->c_str());
    }

    if (!messages.empty())
        return true;

    if (isRA)
        handler->SendSysMessage("usage: help/list/reload/more.. or add/init/remove/more.. PLAYERNAME");

    return true;
}

void RandomPlayerbotMgr::HandleCommand(uint32 type, const std::string& text, Player& fromPlayer, std::string channelName, Team team, uint32 lang)
{
    ForEachPlayerbot([&](Player* bot)
    {
        if (type == CHAT_MSG_SAY)
        {
            if (bot->GetMapId() != fromPlayer.GetMapId() || sServerFacade.GetDistance2d(bot, &fromPlayer) > 25)
            {
                return;
            }
        }

        if (type == CHAT_MSG_YELL)
        {
            if (bot->GetMapId() != fromPlayer.GetMapId() || sServerFacade.GetDistance2d(bot, &fromPlayer) > 300)
            {
                return;
            }
        }

        if (team != TEAM_BOTH_ALLOWED && bot->GetTeam() != team)
        {
            return;
        }

        if (type == CHAT_MSG_GUILD && bot->GetGuildId() != fromPlayer.GetGuildId())
        {
            return;
        }

        if (!channelName.empty())
        {
            if (ChannelMgr* cMgr = channelMgr(bot->GetTeam()))
            {
                Channel* chn = cMgr->GetChannel(channelName, bot);
                if (!chn)
                {
                    return;
                }
            }
        }

        bot->GetPlayerbotAI()->HandleCommand(type, text, fromPlayer, lang);
    });
}

void RandomPlayerbotMgr::OnPlayerLogout(Player* player)
{
    bool hadPlayerBot = GetPlayerBot(player->GetGUIDLow());

    DisablePlayerBot(player->GetGUIDLow());

    if (!hadPlayerBot && player->GetPlayerbotAI() && player->GetPlayerbotAI()->IsRealPlayer() && player->GetGroup() && sPlayerbotAIConfig.IsFreeAltBot(player))
        player->GetSession()->SetOffline(); //Prevent groupkick

    ForEachPlayerbot([&](Player* bot) {
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (player == ai->GetMaster())
        {
            ai->SetMaster(NULL);
            if (!bot->InBattleGround())
            {
                ai->ResetStrategies();
            }
        }
    });

    players.erase(player->GetGUIDLow());
}

void RandomPlayerbotMgr::OnBotLoginInternal(Player * const bot)
{
    sLog.outDetail("%u/%d Bot %s logged in", GetPlayerbotsAmount(), sRandomPlayerbotMgr.GetMaxAllowedBotCount(), bot->GetName());

    ApplyActionHistorySize(bot);
	//if (loginProgressBar && playerBots.size() < sRandomPlayerbotMgr.GetMaxAllowedBotCount()) { loginProgressBar->step(); }
	//if (loginProgressBar && playerBots.size() >= sRandomPlayerbotMgr.GetMaxAllowedBotCount() - 1) {
    //if (loginProgressBar && playerBots.size() + 1 >= sRandomPlayerbotMgr.GetMaxAllowedBotCount()) {
	//	sLog.outString("All bots logged in");
    //    delete loginProgressBar;
	//}
}

void RandomPlayerbotMgr::OnPlayerLogin(Player* player)
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    ForEachPlayerbot([&](Player* bot)
    {
        if (player == bot)
            return;

        Group* group = bot->GetGroup();
        if (!group)
            return;

        for (GroupReference *gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->getSource();
            PlayerbotAI* ai = bot->GetPlayerbotAI();
            if (member == player && (!ai->GetMaster() || ai->GetMaster()->GetPlayerbotAI()))
            {
                if (!bot->InBattleGround())
                {
                    ai->SetMaster(player);
                    ai->ResetStrategies();
                    ai->TellPlayer(ai->GetMaster(), BOT_TEXT("hello"));
                }
                break;
            }
        }
    });

    if (IsFreeBot(player))
    {
        uint32 guid = player->GetGUIDLow();
        if (!sPlayerbotAIConfig.IsFreeAltBot(player))
           SetEventValue(guid, "login", 0, 0);
    }
    else
    {
        players[player->GetGUIDLow()] = player;
        sLog.outDebug("Including non-random bot player %s into random bot update", player->GetName());
    }
}

void RandomPlayerbotMgr::OnPlayerLoginError(uint32 bot)
{
    SetEventValue(bot, "add", 0, 0);
    SetEventValue(bot, "login", 0, 0);
    currentBots.remove(bot);
}

Player* RandomPlayerbotMgr::GetRandomPlayer()
{
    if (players.empty())
        return NULL;

    uint32 index = urand(0, players.size() - 1);
    return players[index];
}

Player* RandomPlayerbotMgr::GetPlayer(uint32 playerGuid)
{
    PlayerBotMap::const_iterator it = players.find(playerGuid);
    return (it == players.end()) ? nullptr : it->second ? it->second : nullptr;
}

RandomPlayerbotMgr::BotStats RandomPlayerbotMgr::GatherBotStats()
{
    BotStats stats;

    ForEachPlayerbot([this, &stats](Player* bot)
    {
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai)
            return;

        stats.total++;

        if (IsAlliance(bot->getRace()))
            stats.alliance[bot->GetLevel() / 10]++;
        else
            stats.horde[bot->GetLevel() / 10]++;

        stats.perRace[bot->getRace()]++;
        stats.perClass[bot->getClass()]++;

        if (ai->AllowActivity())
            stats.active++;

        if (bot->IsTaxiFlying())
            stats.taxi++;

        bool isMoving = bot->IsMoving() && !bot->IsTaxiFlying() && !bot->IsFlying();
        if (isMoving)
            stats.moving++;

        if (bot->IsMounted() && !bot->IsTaxiFlying())
            stats.mounted++;

        if (bot->IsInCombat())
            stats.combat++;

        if (bot->isAFK())
            stats.afk++;

        if (sServerFacade.UnitIsDead(bot))
            stats.dead++;

        int spec = AiFactory::GetPlayerSpecTab(bot);
        switch (bot->getClass())
        {
        case CLASS_DRUID:
            if (spec == 2)
                stats.roleHeal++;
            else
                stats.roleDps++;
            break;
        case CLASS_PALADIN:
            if (spec == 1)
                stats.roleTank++;
            else if (spec == 0)
                stats.roleHeal++;
            else
                stats.roleDps++;
            break;
        case CLASS_PRIEST:
            if (spec != 2)
                stats.roleHeal++;
            else
                stats.roleDps++;
            break;
        case CLASS_SHAMAN:
            if (spec == 2)
                stats.roleHeal++;
            else
                stats.roleDps++;
            break;
        case CLASS_WARRIOR:
            if (spec == 2)
                stats.roleTank++;
            else
                stats.roleDps++;
            break;
#ifdef MANGOSBOT_TWO
        case CLASS_DEATH_KNIGHT:
            if (spec == 0)
                stats.roleTank++;
            else
                stats.roleDps++;
            break;
#endif
        default:
            stats.roleDps++;
            break;
        }

        stats.perState[(uint8)ai->GetState()]++;

        AiObjectContext* context = ai->GetAiObjectContext();

        TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
        if (target)
            stats.perTravelState[(uint8)target->GetTravelState()]++;

        // Shared classification (also used by 'find'/'sample') so the definitions cannot drift.
        stats.activity[GetBotActivity(bot)]++;
        if (GetBotStuck(bot))
            stats.stuck++;

        // Only in-world bots have a map. GetZoneId() -> GetTerrain() asserts m_currMap, so a bot that
        // is mid-teleport / logging out aborts the whole process. Skip it (and count it separately).
        if (bot->IsInWorld())
            stats.perZone[bot->GetZoneId()]++;
        else
            stats.notInWorld++;
    });

    return stats;
}

std::list<std::string> RandomPlayerbotMgr::FormatBotStats(const BotStats& stats, bool includeAllZones)
{
    std::list<std::string> lines;

    auto pct = [&stats](uint32 n) -> std::string
    {
        if (!stats.total)
            return "0%";
        return std::to_string((uint32)((n * 100.0 / stats.total) + 0.5)) + "%";
    };

    lines.push_back("Playerbots: " + std::to_string(stats.total) + " online");

    // Bot state
    {
        static const std::pair<int, const char*> stateNames[] =
        {
            { (int)BotState::BOT_STATE_NON_COMBAT, "Non Combat" },
            { (int)BotState::BOT_STATE_COMBAT, "Combat" },
            { (int)BotState::BOT_STATE_DEAD, "Dead" },
            { (int)BotState::BOT_STATE_REACTION, "Reaction" },
        };

        std::ostringstream s;
        s << "  state:    ";
        bool first = true;
        for (auto& n : stateNames)
        {
            auto it = stats.perState.find((uint8)n.first);
            uint32 c = (it == stats.perState.end()) ? 0 : it->second;
            if (!c)
                continue;

            if (!first) s << ", ";
            s << n.second << " " << c << " (" << pct(c) << ")";
            first = false;
        }
        if (first) s << "none";
        lines.push_back(s.str());
    }

    // Activity
    {
        static const char* order[] = { "idle", "moving", "traveling", "combat" };
        std::ostringstream s;
        s << "  activity: ";
        for (int i = 0; i < 4; ++i)
        {
            auto it = stats.activity.find(order[i]);
            uint32 c = (it == stats.activity.end()) ? 0 : it->second;
            if (i) s << ", ";
            s << order[i] << " " << c;
        }
        lines.push_back(s.str());
    }

    lines.push_back("  stuck:    " + std::to_string(stats.stuck));

    // Bots counted in the total but excluded from the zone histogram because they have no map.
    if (stats.notInWorld)
        lines.push_back("  not in world: " + std::to_string(stats.notInWorld) + " (excluded from zone counts)");

    // Level bands
    {
        uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        std::ostringstream s;
        s << "  level:    ";
        bool first = true;
        for (uint32 i = 0; i < 10; ++i)
        {
            if (!stats.alliance[i] && !stats.horde[i])
                continue;

            uint32 from = i * 10;
            uint32 to = std::min(from + 9, maxLevel);
            if (!from) from = 1;

            if (!first) s << ", ";
            s << from << ".." << to << " " << (stats.alliance[i] + stats.horde[i]) << " (" << stats.alliance[i] << "A/" << stats.horde[i] << "H)";
            first = false;
        }
        if (first) s << "none";
        lines.push_back(s.str());
    }

    // Zones (top N by count; group by zone id, resolve names only for the shown rows)
    {
        std::vector<std::pair<uint32, uint32>> zones(stats.perZone.begin(), stats.perZone.end());
        std::sort(zones.begin(), zones.end(), [](const std::pair<uint32, uint32>& a, const std::pair<uint32, uint32>& b) { return a.second > b.second; });

        uint32 total = 0, covered = 0;
        for (auto& z : zones)
            total += z.second;

        std::ostringstream s;
        s << "  zone:     ";
        size_t shown = 0;
        for (auto& z : zones)
        {
            if (!includeAllZones && shown >= 8)
                break;

            AreaTableEntry const* area = GetAreaEntryByAreaID(z.first);
            std::string name = area ? area->area_name[0] : ("zone " + std::to_string(z.first));

            if (shown) s << ", ";
            s << name << " " << z.second;
            covered += z.second;
            ++shown;
        }
        if (!includeAllZones && zones.size() > shown)
            s << ", other " << (total - covered);
        if (!shown) s << "none";
        lines.push_back(s.str());
    }

    // Misc counts
    {
        std::ostringstream s;
        s << "  counts:   dead " << stats.dead << ", combat " << stats.combat
          << ", mounted " << stats.mounted << ", taxi " << stats.taxi
          << ", afk " << stats.afk << ", active " << stats.active << ", moving " << stats.moving;
        lines.push_back(s.str());
    }

    // Class
    {
        std::ostringstream s;
        s << "  class:    ";
        bool first = true;
        for (auto& c : stats.perClass)
        {
            if (!first) s << ", ";
            s << ChatHelper::formatClass(c.first) << " " << c.second;
            first = false;
        }
        if (first) s << "none";
        lines.push_back(s.str());
    }

    // Race
    {
        std::ostringstream s;
        s << "  race:     ";
        bool first = true;
        for (auto& r : stats.perRace)
        {
            if (!first) s << ", ";
            s << ChatHelper::formatRace(r.first) << " " << r.second;
            first = false;
        }
        if (first) s << "none";
        lines.push_back(s.str());
    }

    // Role
    lines.push_back("  role:     tank " + std::to_string(stats.roleTank) + ", heal " + std::to_string(stats.roleHeal) + ", dps " + std::to_string(stats.roleDps));

    // Questing
    {
        auto sc = [&stats](TravelState st) -> uint32
        {
            auto it = stats.perTravelState.find((uint8)st);
            return (it == stats.perTravelState.end()) ? 0 : it->second;
        };

        uint32 pick = sc(TravelState::TRAVEL_STATE_TRAVEL_PICK_UP_QUEST) + sc(TravelState::TRAVEL_STATE_WORK_PICK_UP_QUEST);
        uint32 doing = sc(TravelState::TRAVEL_STATE_TRAVEL_DO_QUEST) + sc(TravelState::TRAVEL_STATE_WORK_DO_QUEST);
        uint32 handin = sc(TravelState::TRAVEL_STATE_TRAVEL_HAND_IN_QUEST) + sc(TravelState::TRAVEL_STATE_WORK_HAND_IN_QUEST);
        uint32 idle = sc(TravelState::TRAVEL_STATE_IDLE);

        lines.push_back("  questing: picking " + std::to_string(pick) + ", doing " + std::to_string(doing) + ", completing " + std::to_string(handin) + ", idling " + std::to_string(idle));
    }

    return lines;
}

namespace
{
    std::string BotTravelStatusText(int status)
    {
        switch ((TravelStatus)status)
        {
            case TravelStatus::TRAVEL_STATUS_NONE: return "none";
            case TravelStatus::TRAVEL_STATUS_PREPARE: return "prepare";
            case TravelStatus::TRAVEL_STATUS_WORK: return "work";
            case TravelStatus::TRAVEL_STATUS_TRAVEL: return "travel";
            case TravelStatus::TRAVEL_STATUS_READY: return "ready";
            case TravelStatus::TRAVEL_STATUS_EXPIRED: return "expired";
            case TravelStatus::TRAVEL_STATUS_COOLDOWN: return "cooldown";
            default: return "unknown";
        }
    }

    bool IsValidBotFilter(const std::string& filter)
    {
        static const char* valid[] =
        {
            "stuck", "idle", "moving", "traveling", "travel", "combat",
            "dead", "notarget", "nomove", "nofree", "group", "quest"
        };

        for (auto v : valid)
            if (filter == v)
                return true;

        return false;
    }

    std::string ValidBotFilters()
    {
        return "stuck, idle, moving, traveling, travel, combat, dead, notarget, nomove, nofree, group, quest";
    }
}

std::string RandomPlayerbotMgr::GetBotActivity(Player* bot)
{
    if (bot->IsInCombat())
        return "combat";

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return "idle";

    AiObjectContext* context = ai->GetAiObjectContext();
    TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
    if (target && context->GetValue<bool>("travel target traveling")->Get())
        return "traveling";

    if (bot->IsMoving() && !bot->IsTaxiFlying() && !bot->IsFlying())
        return "moving";

    return "idle";
}

bool RandomPlayerbotMgr::GetBotStuck(Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();

    TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
    if (!target)
        return false;

    if (!context->GetValue<bool>("travel target active")->Get())
        return false;

    uint32 posLastChange = 0;
    if (MemoryCalculatedValue<WorldPosition>* pos = dynamic_cast<MemoryCalculatedValue<WorldPosition>*>(context->GetUntypedValue("current position")))
        posLastChange = pos->LastChangeDelay();

    return posLastChange > 60;
}

std::string RandomPlayerbotMgr::FormatBotLine(Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    BotState state = ai->GetState();
    AiObjectContext* context = ai->GetAiObjectContext();

    std::string zone = "unknown";

    // GetZoneId() -> GetTerrain() asserts m_currMap. A bot that is mid-teleport or logging out has no
    // map, and this runs for every bot over RA ('find'/'sample'), so guard before touching it.
    if (bot->IsInWorld())
    {
        if (AreaTableEntry const* area = GetAreaEntryByAreaID(bot->GetZoneId()))
            zone = area->area_name[0];
    }

    std::string lastExecuted = ai->GetLastExecutedActionName(state);
    if (lastExecuted.empty()) lastExecuted = "none";

    std::string decision = ai->GetLastActionDecision(state);

    // The engine tick log's final segment is usually "A:<action> - <RESULT>"; keep just the result.
    std::string decisionResult;
    size_t dash = decision.rfind(" - ");
    if (decision.compare(0, 2, "A:") == 0 && dash != std::string::npos)
        decisionResult = decision.substr(dash + 3);

    Unit* target = ai->GetUnit(context->GetValue<ObjectGuid>("current target")->Get());

    int selfHp = bot->GetMaxHealth() ? (int)((float)bot->GetHealth() / bot->GetMaxHealth() * 100) : 0;
    std::string hp = std::to_string(selfHp) + "%";
    if (target && target->GetMaxHealth())
        hp += "/" + std::to_string((int)((float)target->GetHealth() / target->GetMaxHealth() * 100)) + "%";

    std::ostringstream out;
    out << bot->GetName() << " lv" << (uint32)bot->GetLevel() << " " << ChatHelper::formatClass(bot->getClass())
        << " " << PlayerbotAI::BotStateToString(state)
        << " | zone=" << zone
        << " | act=" << lastExecuted;
    if (!decisionResult.empty())
        out << " (" << decisionResult << ")";
    out << " | tgt=" << (target ? target->GetName() : "none")
        << " | hp=" << hp;

    TravelTarget* travel = context->GetValue<TravelTarget*>("travel target")->Get();
    if (travel)
    {
        int travelStatus = (int)travel->GetStatus();
        out << " | travel=" << BotTravelStatusText(travelStatus);

        // Only show time/distance for a live target; NONE/EXPIRED carry stale position data.
        if (travelStatus != (int)TravelStatus::TRAVEL_STATUS_NONE && travelStatus != (int)TravelStatus::TRAVEL_STATUS_EXPIRED)
        {
            if (travel->GetTimeLeft() > 0)
                out << " " << (travel->GetTimeLeft() / 1000) << "s";
            if (travel->GetPosition())
            {
                WorldPosition botPos(bot);
                out << " " << (uint32)botPos.distance(*travel->GetPosition()) << "y";
            }
        }
    }
    else
    {
        out << " | travel=none";
    }

    out << " | stuck=" << (GetBotStuck(bot) ? "yes" : "no");

    return out.str();
}

bool RandomPlayerbotMgr::BotMatchesFilter(Player* bot, const std::string& filter)
{
    if (filter.empty())
        return true;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();

    for (auto& f : Qualified::getMultiQualifiers(filter, ","))
    {
        if (f == "stuck")
        {
            if (!GetBotStuck(bot)) return false;
        }
        else if (f == "idle" || f == "moving" || f == "traveling")
        {
            if (GetBotActivity(bot) != f) return false;
        }
        else if (f == "travel")
        {
            if (!context->GetValue<bool>("travel target active")->Get()) return false;
        }
        else if (f == "combat")
        {
            if (!bot->IsInCombat()) return false;
        }
        else if (f == "dead")
        {
            if (!sServerFacade.UnitIsDead(bot)) return false;
        }
        else if (f == "notarget")
        {
            if (ai->GetUnit(context->GetValue<ObjectGuid>("current target")->Get())) return false;
        }
        else if (f == "nomove")
        {
            if (context->GetValue<bool>("can move around")->Get()) return false;
        }
        else if (f == "nofree")
        {
            TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
            if (!target) return false;
            if (context->GetValue<bool>("can free move", target->GetPosStr())->Get()) return false;
        }
        else if (f == "group")
        {
            if (!bot->GetGroup()) return false;
        }
        else if (f == "quest")
        {
            TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
            if (!target) return false;

            switch (target->GetTravelState())
            {
                case TravelState::TRAVEL_STATE_TRAVEL_PICK_UP_QUEST:
                case TravelState::TRAVEL_STATE_WORK_PICK_UP_QUEST:
                case TravelState::TRAVEL_STATE_TRAVEL_DO_QUEST:
                case TravelState::TRAVEL_STATE_WORK_DO_QUEST:
                case TravelState::TRAVEL_STATE_TRAVEL_HAND_IN_QUEST:
                case TravelState::TRAVEL_STATE_WORK_HAND_IN_QUEST:
                    break;
                default:
                    return false;
            }
        }
        else
        {
            return false;
        }
    }

    return true;
}

std::list<std::string> RandomPlayerbotMgr::SampleBots(std::string param, bool exhaustive)
{
    int limit = -1;
    std::string filter;

    for (auto& token : Qualified::getMultiQualifiers(param, " "))
    {
        if (token.empty())
            continue;

        if (Qualified::isValidNumberString(token))
            limit = std::stoi(token);
        else
            filter = filter.empty() ? token : (filter + "," + token);
    }

    if (!filter.empty())
    {
        for (auto& f : Qualified::getMultiQualifiers(filter, ","))
        {
            if (!IsValidBotFilter(f))
            {
                std::list<std::string> messages;
                messages.push_back("Unknown filter: " + f);
                messages.push_back("Valid filters: " + ValidBotFilters());
                return messages;
            }
        }
    }

    int defaultLimit = exhaustive ? 100 : 10;
    int maxLimit = exhaustive ? 200 : 100;
    if (limit <= 0)
        limit = defaultLimit;
    if (limit > maxLimit)
        limit = maxLimit;

    std::list<std::string> lines;
    uint32 matched = 0;

    ForEachPlayerbot([&](Player* bot)
    {
        if (!bot->GetPlayerbotAI())
            return;

        if (!BotMatchesFilter(bot, filter))
            return;

        matched++;

        if ((int)lines.size() < limit)
            lines.push_back(FormatBotLine(bot));
    });

    std::ostringstream summary;
    summary << "showing " << lines.size() << " of " << matched << (filter.empty() ? " online" : " matches");
    if ((int)lines.size() < (int)matched)
        summary << " (truncated; raise N or narrow filter)";
    lines.push_back(summary.str());

    return lines;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleSample(std::string param)
{
    return SampleBots(param, false);
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleFind(std::string param)
{
    return SampleBots(param, true);
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleTaxTest(std::string param)
{
    std::list<std::string> messages;

    std::vector<std::string> parts = Qualified::getMultiQualifiers(param, " ");
    if (parts.size() < 2)
    {
        messages.push_back("usage: taxtest <bot> <node> [node...]");
        return messages;
    }

    Player* bot = sObjectAccessor.FindPlayerByName(parts[0].c_str());
    if (!bot || !bot->GetPlayerbotAI())
    {
        messages.push_back("taxtest: bot not found: " + parts[0]);
        return messages;
    }

    std::vector<uint32> nodes;
    for (size_t i = 1; i < parts.size(); ++i)
        nodes.push_back(uint32(atoi(parts[i].c_str())));

    TaxiNodesEntry const* start = sTaxiNodesStore.LookupEntry(nodes[0]);
    if (!start)
    {
        messages.push_back("taxtest: bad start node");
        return messages;
    }

    if (bot->GetMapId() != start->map_id)
    {
        bot->TeleportTo(start->map_id, start->x, start->y, start->z, bot->GetOrientation());
        messages.push_back("taxtest: teleported to start map; rerun the command");
        return messages;
    }

    bot->CombatStop(true);
    bot->GetMotionMaster()->Clear();
    bot->SetPosition(start->x, start->y, start->z, bot->GetOrientation(), true);

    bool ok = bot->ActivateTaxiPathTo(nodes, nullptr, 0);
    messages.push_back(std::string("taxtest ") + bot->GetName() + ": " + (ok ? "taxi started" : "ActivateTaxiPathTo FAILED"));
    return messages;
}

#ifdef GenerateBotTests
std::list<std::string> RandomPlayerbotMgr::HandleConsoleTravelSample(std::string param)
{
    std::list<std::string> messages;

    std::vector<std::string> parts = Qualified::getMultiQualifiers(param, " ");
    if (parts.empty())
    {
        messages.push_back("usage: travelsample <bot> <levels> <purposes> [runs]");
        return messages;
    }

    Player* bot = sObjectAccessor.FindPlayerByName(parts[0].c_str());
    if (!bot || !bot->GetPlayerbotAI())
    {
        messages.push_back("travelsample: bot not found: " + parts[0]);
        return messages;
    }

    size_t nameLen = parts[0].size();
    std::string rest = param.size() > nameLen + 1 ? param.substr(nameLen + 1) : "";
    return TravelProbe::HandleTravelSample(bot, rest);
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleTravelTrace(std::string param)
{
    return TravelProbe::HandleTravelTrace(param);
}
#endif

std::list<std::string> RandomPlayerbotMgr::HandleConsoleZoneUpd(std::string param)
{
    std::list<std::string> messages;

    std::vector<std::string> parts = Qualified::getMultiQualifiers(param, " ");
    if (parts.size() < 2)
    {
        messages.push_back("usage: zoneupd <bot> <zone>");
        return messages;
    }

    Player* bot = sObjectAccessor.FindPlayerByName(parts[0].c_str());
    if (!bot || !bot->GetSession())
    {
        messages.push_back("zoneupd: bot not found: " + parts[0]);
        return messages;
    }

    uint32 zone = uint32(atoi(parts[1].c_str()));
    WorldPacket data(CMSG_ZONEUPDATE, 4);
    data << zone;
    bot->GetSession()->HandleZoneUpdateOpcode(data);

    messages.push_back("zoneupd " + std::string(bot->GetName()) + ": sent CMSG_ZONEUPDATE " + std::to_string(zone));
    return messages;
}

void RandomPlayerbotMgr::PrintStats(uint32 requesterGuid)
{
    Player* requester = GetPlayer(requesterGuid);
    BotStats stats = GatherBotStats();

    auto emit = [requester](const std::string& line)
    {
        sLog.outString("%s", line.c_str());
        if (requester)
            requester->SendMessageToPlayer(line);
    };

    emit(std::to_string(stats.total) + " Random Bots online");

    emit("Bots level:");
    {
        uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        for (uint32 i = 0; i < 10; ++i)
        {
            if (!stats.alliance[i] && !stats.horde[i])
                continue;

            uint32 from = i * 10;
            uint32 to = std::min(from + 9, maxLevel);
            if (!from) from = 1;

            std::ostringstream ss;
            ss << "    " << from << ".." << to << ": " << stats.alliance[i] << " alliance, " << stats.horde[i] << " horde";
            emit(ss.str());
        }
    }

    emit("Bots race:");
    for (auto& r : stats.perRace)
    {
        std::ostringstream ss;
        ss << "    " << ChatHelper::formatRace(r.first) << ": " << r.second;
        emit(ss.str());
    }

    emit("Bots class:");
    for (auto& c : stats.perClass)
    {
        std::ostringstream ss;
        ss << "    " << ChatHelper::formatClass(c.first) << ": " << c.second;
        emit(ss.str());
    }

    emit("Bots role:");
    {
        std::ostringstream ss;
        ss << "    tank: " << stats.roleTank << ", heal: " << stats.roleHeal << ", dps: " << stats.roleDps;
        emit(ss.str());
    }

    emit("Bots status:");
    emit("    Active: " + std::to_string(stats.active));
    emit("    Moving: " + std::to_string(stats.moving));
    emit("    On taxi: " + std::to_string(stats.taxi));
    emit("    On mount: " + std::to_string(stats.mounted));
    emit("    In combat: " + std::to_string(stats.combat));
    emit("    Dead: " + std::to_string(stats.dead));
    emit("    AFK: " + std::to_string(stats.afk));

    emit("Bots questing:");
    {
        auto sc = [&stats](TravelState st) -> uint32
        {
            auto it = stats.perTravelState.find((uint8)st);
            return (it == stats.perTravelState.end()) ? 0 : it->second;
        };

        emit("    Picking quests: " + std::to_string(sc(TravelState::TRAVEL_STATE_TRAVEL_PICK_UP_QUEST) + sc(TravelState::TRAVEL_STATE_WORK_PICK_UP_QUEST)));
        emit("    Doing quests: " + std::to_string(sc(TravelState::TRAVEL_STATE_TRAVEL_DO_QUEST) + sc(TravelState::TRAVEL_STATE_WORK_DO_QUEST)));
        emit("    Completing quests: " + std::to_string(sc(TravelState::TRAVEL_STATE_TRAVEL_HAND_IN_QUEST) + sc(TravelState::TRAVEL_STATE_WORK_HAND_IN_QUEST)));
        emit("    Idling: " + std::to_string(sc(TravelState::TRAVEL_STATE_IDLE)));
    }
}

double RandomPlayerbotMgr::GetBuyMultiplier(Player* bot)
{
    uint32 id = bot->GetGUIDLow();
    uint32 value = GetEventValue(id, "buymultiplier");
    if (!value)
    {
        value = urand(50, 120);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval, sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "buymultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

double RandomPlayerbotMgr::GetSellMultiplier(Player* bot)
{
    uint32 id = bot->GetGUIDLow();
    uint32 value = GetEventValue(id, "sellmultiplier");
    if (!value)
    {
        value = urand(80, 250);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval, sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "sellmultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

void RandomPlayerbotMgr::AddTradeDiscount(Player* bot, Player* master, int32 value)
{
    if (!master) return;
    uint32 discount = GetTradeDiscount(bot, master);
    int32 result = (int32)discount + value;
    discount = (result < 0 ? 0 : result);

    SetTradeDiscount(bot, master, discount);
}

void RandomPlayerbotMgr::SetTradeDiscount(Player* bot, Player* master, uint32 value)
{
    if (!master) return;
    uint32 botId =  bot->GetGUIDLow();
    uint32 masterId =  master->GetGUIDLow();
    std::ostringstream name; name << "trade_discount_" << masterId;
    SetEventValue(botId, name.str(), value, sPlayerbotAIConfig.maxRandomBotInWorldTime);
}

uint32 RandomPlayerbotMgr::GetTradeDiscount(Player* bot, Player* master)
{
    if (!master) return 0;
    uint32 botId =  bot->GetGUIDLow();
    uint32 masterId = master->GetGUIDLow();
    std::ostringstream name; name << "trade_discount_" << masterId;
    return GetEventValue(botId, name.str());
}

std::string RandomPlayerbotMgr::HandleRemoteCommand(std::string request)
{
    std::string::iterator pos = find(request.begin(), request.end(), ',');
    if (pos == request.end())
    {
        std::ostringstream out; out << "invalid request: " << request;
        return out.str();
    }

    std::string command = std::string(request.begin(), pos);
    uint32 guid = std::atoi(std::string(pos + 1, request.end()).c_str());
    Player* bot = GetPlayerBot(guid);
    if (!bot)
        return "invalid guid";

    PlayerbotAI *ai = bot->GetPlayerbotAI();
    if (!ai)
        return "invalid guid";

    return ai->HandleRemoteCommand(command);
}

void RandomPlayerbotMgr::ChangeStrategy(Player* player)
{
    uint32 bot = player->GetGUIDLow();

    if (urand(0, 100) > 100 * sPlayerbotAIConfig.randomBotRpgChance) // select grind / pvp
    {
        sLog.outDetail("Bot #%d %s:%d <%s>: sent to grind spot", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
        // teleport in different places only if players are online
        RandomTeleportForLevel(player, players.size());
        ScheduleTeleport(bot);
    }
    else
    {
        sLog.outDetail("Bot #%d %s:%d <%s>: sent to inn", bot, player->GetTeam() == ALLIANCE ? "A" : "H", player->GetLevel(), player->GetName());
        RandomTeleportForRpg(player, players.size());
        ScheduleTeleport(bot);
    }
}

void RandomPlayerbotMgr::RandomTeleportForRpg(Player* bot, bool activeOnly)
{
    uint32 race = bot->getRace();
    uint32 level = bot->GetLevel();
    sLog.outDetail("Random teleporting bot %s for RPG (%zu locations available)", bot->GetName(), rpgLocsCacheLevel[race][level].size());
    RandomTeleport(bot, rpgLocsCacheLevel[race][level], true, activeOnly);
    Refresh(bot);

    //Travel cooldown for 10 minutes.
    if (bot->GetPlayerbotAI())
    {
        AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        sTravelMgr.SetNullTravelTarget(travelTarget);
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
        travelTarget->SetExpireIn(10 * MINUTE * IN_MILLISECONDS);
    }
}

void RandomPlayerbotMgr::Remove(Player* bot)
{
    uint32 owner = bot->GetGUIDLow();
    CharacterDatabase.PExecute("DELETE FROM ai_playerbot_random_bots WHERE owner = 0 AND bot = '%d'", owner);
    eventCache[owner].clear();

    LogoutPlayerBot(owner);
}

const CreatureDataPair* RandomPlayerbotMgr::GetCreatureDataByEntry(uint32 entry)
{
    if (entry != 0 && ObjectMgr::GetCreatureTemplate(entry))
    {
        FindCreatureData worker(entry, NULL);
        sObjectMgr.DoCreatureData(worker);
        CreatureDataPair const* dataPair = worker.GetResult();
        return dataPair;
    }
    return NULL;
}

uint32 RandomPlayerbotMgr::GetCreatureGuidByEntry(uint32 entry)
{
    uint32 guid = 0;

    CreatureDataPair const* dataPair = sRandomPlayerbotMgr.GetCreatureDataByEntry(entry);
    guid = dataPair->first;

    return guid;
}

uint32 RandomPlayerbotMgr::GetBattleMasterEntry(Player* bot, BattleGroundTypeId bgTypeId, bool fake)
{
    Team team = bot->GetTeam();
    uint32 entry = 0;
    std::vector<uint32> Bms;

    for (auto i = begin(BattleMastersCache[team][bgTypeId]); i != end(BattleMastersCache[team][bgTypeId]); ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    for (auto i = begin(BattleMastersCache[TEAM_BOTH_ALLOWED][bgTypeId]); i != end(BattleMastersCache[TEAM_BOTH_ALLOWED][bgTypeId]); ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    if (Bms.empty())
        return entry;

    float dist1 = FLT_MAX;

    for (auto i = begin(Bms); i != end(Bms); ++i)
    {
        CreatureDataPair const* dataPair = sRandomPlayerbotMgr.GetCreatureDataByEntry(*i);
        if (!dataPair)
            continue;

        CreatureData const* data = &dataPair->second;

        Unit* Bm = sMapMgr.FindMap((uint32)data->mapid)->GetUnit(ObjectGuid(HIGHGUID_UNIT, *i, dataPair->first));
        if (!Bm)
            continue;

        if (bot->GetMapId() != Bm->GetMapId())
            continue;

        // return first available guid on map if queue from anywhere
        if (fake)
        {
            entry = *i;
            break;
        }

        AreaTableEntry const* area = GetAreaEntryByAreaID(sServerFacade.GetAreaId(Bm));
        if (!area)
            continue;

        if (area->team == 4 && bot->GetTeam() == ALLIANCE)
            continue;
        if (area->team == 2 && bot->GetTeam() == HORDE)
            continue;

        if (Bm->GetDeathState() == DEAD)
            continue;

        float dist2 = sServerFacade.GetDistance2d(bot, data->posX, data->posY);
        if (dist2 < dist1)
        {
            dist1 = dist2;
            entry = *i;
        }
    }

    return entry;
}

void RandomPlayerbotMgr::Hotfix(Player* bot, uint32 version)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    uint32 exp = bot->GetUInt32Value(PLAYER_XP);
    uint32 level = bot->GetLevel();
    uint32 id = bot->GetGUIDLow();

    for (int fix = version; fix <= MANGOSBOT_VERSION; fix++)
    {
        int count = 0;
        switch (fix)
        {
            case 1: // Apply class quests to previously made random bots

                if (level < 10)
                {
                    break;
                }

                for (std::list<uint32>::iterator i = factory.classQuestIds.begin(); i != factory.classQuestIds.end(); ++i)
                {
                    uint32 questId = *i;
                    Quest const *quest = sObjectMgr.GetQuestTemplate(questId);

                    if (!bot->SatisfyQuestClass(quest, false) ||
                        quest->GetMinLevel() > bot->GetLevel() ||
                        !bot->SatisfyQuestRace(quest, false) || bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                        continue;

                    bot->SetQuestStatus(questId, QUEST_STATUS_COMPLETE);
                    bot->RewardQuest(quest, 0, bot, false);
                    bot->SetLevel(level);
                    bot->SetUInt32Value(PLAYER_XP, exp);
                    sLog.outDetail("Bot %d rewarded quest %d",
                        bot->GetGUIDLow(), questId);
                    count++;
                }

                if (count > 0)
                {
                    sLog.outDetail("Bot %d hotfix (Class Quests), %d quests rewarded",
                        bot->GetGUIDLow(), count);
                    count = 0;
                }
                break;
            case 2: // Init Riding skill fix

                if (level < 20)
                {
                    break;
                }
                factory.InitSkills();
                sLog.outDetail("Bot %d hotfix (Riding Skill) applied",
                    bot->GetGUIDLow());
                break;

            default:
                break;
        }
    }
    SetValue(bot, "version", MANGOSBOT_VERSION);
    sLog.outDetail("Bot %d hotfix v%d applied",
        bot->GetGUIDLow(), MANGOSBOT_VERSION);
}

void RandomPlayerbotMgr::MirrorAh()
{
    sRandomPlayerbotMgr.m_ahActionMutex.lock();

    ahMirror.clear();

    std::vector<AuctionHouseType> houses = { (AuctionHouseType)0,(AuctionHouseType)1,(AuctionHouseType)2 };

    //Now loops over all houses. Can probably be faction specific later.
    for (auto house : houses)
    {
        AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(house);

        AuctionHouseObject::AuctionEntryMap const& map = auctionHouse->GetAuctions();

        for (auto& auction : map)
        {
            if (!auction.second)
                continue;

            AuctionEntry auctionEntry = *auction.second;

            if (!auctionEntry.buyout)
                continue;

            if (!auctionEntry.itemCount)
                continue;

            ahMirror[auctionEntry.itemTemplate].push_back(auctionEntry);
        }
    }
    sRandomPlayerbotMgr.m_ahActionMutex.unlock();
}

typedef std::unordered_map <uint32, std::list<float>> botPerformanceMetric;
std::unordered_map<std::string, botPerformanceMetric> botPerformanceMetrics;

void RandomPlayerbotMgr::PushMetric(botPerformanceMetric& metric, const uint32 bot, const float value, uint32 maxNum) const
{
    metric[bot].push_back(value);

    if (metric[bot].size() > maxNum)
        metric[bot].pop_front();
}

float RandomPlayerbotMgr::GetMetricDelta(botPerformanceMetric& metric) const
{
    float deltaMetric = 0;
    for (auto& botMetric : metric)
    {
        std::list<float> values = botMetric.second;
        if (values.size() > 1)
            deltaMetric += (values.back() - values.front()) / values.size();
    }

    if (metric.empty())
        return 0;

    return deltaMetric / metric.size();
}

std::string RandomPlayerbotMgr::GetCommandTexts(const std::string& command)
{
    auto texts = GetCommandTexts();
    auto it = texts.find(command);
    if (it != texts.end())
        return it->second;
    return "";
}

std::unordered_map<std::string, std::string> RandomPlayerbotMgr::GetCommandTexts()
{
    return std::unordered_map<std::string, std::string>
    {
        {"init", "Randomize the first available bot.\nUsage: init"},
        {"upgrade", "Update gear and spells for all random bots.\nUsage: upgrade"},
        {"refresh", "Log out and log in all random bots to refresh their status.\nUsage: refresh"},
        {"teleport", "Teleport all random bots to a location suitable for their level.\nUsage: teleport"},
        {"rpg", "Teleport all random bots to a location for RPG activities.\nUsage: rpg"},
        {"revive", "Revive all dead random bots.\nUsage: revive"},
        {"grind", "Teleport all random bots to a grinding location.\nUsage: grind"},
        {"change_strategy", "Change the AI strategy for random bots.\nUsage: change_strategy <botname> <strategy>"},
        {"remove", "Remove a random bot from the server.\nUsage: remove <botname>"},
        {"reset", "Reset all random bots and clear event cache.\nUsage: reset"},
        {"diff", "Show server performance metrics.\nUsage: diff [player_diff] [empty_diff]"},
        {"stats", "Print bot statistics.\nUsage: stats"},
        {"sample", "Show a compact row for the first N bots.\nUsage: sample [N] [filter]"},
        {"find", "Show a compact row for every bot matching a filter.\nUsage: find [filter] [N]"},
        {"history", "Set per-bot action history size (0 = off).\nUsage: history <off|on|N> | history <botname> <off|on|N>"},
        {"update", "Trigger immediate bot AI update.\nUsage: update"},
        {"pid", "Adjust PID controller values.\nUsage: pid p i d"},
        {"clean map", "Unload and reload map files.\nUsage: clean map"},
        {"login debug", "Toggle login debug mode.\nUsage: login debug"},
        {"cmd", "Send command to a bot.\nUsage: cmd <botname> <command>"},
        {"help", "Show help for commands.\nUsage: help [command]"}
    };
}

std::list<std::string> RandomPlayerbotMgr::HandleHelp(std::string param)
{
    std::list<std::string> messages;
        
    if (param.empty())
    {
        messages.push_back("Type 'help commands for all available commands.");
        messages.push_back("Type 'help <command>' for more information on a specific command.");
        return messages;
    }

    if (param == "commands")
    {
        std::string commands = "Commands: ";
        for (auto& [command, help] : GetCommandTexts())
        {
            commands += command + ", ";
        }

        commands = commands.substr(0, commands.size() - 2);
        messages.push_back(commands);
        return messages;
    }
    
    
    std::string helpText = GetCommandTexts(param);
    if (!helpText.empty())
    {
        messages.push_back(helpText);
    }  
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomizeFirst(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomizeFirst(bot);
    messages.push_back("init applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleUpdateGearSpells(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    UpdateGearSpells(bot);
    messages.push_back("upgrade applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRefresh(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    Refresh(bot);
    messages.push_back("refresh applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomTeleportForLevel(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomTeleportForLevel(bot);
    messages.push_back("teleport applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomTeleportForRpg(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomTeleportForRpg(bot);
    messages.push_back("rpg applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRevive(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    Revive(bot);
    messages.push_back("revive applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRandomTeleport(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    RandomTeleport(bot);
    messages.push_back("grind applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleChangeStrategy(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    ChangeStrategy(bot);
    messages.push_back("change_strategy applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleRemove(Player* bot)
{
    std::list<std::string> messages;
    if (!bot)
    {
        messages.push_back("Bot not found");
        return messages;
    }
    Remove(bot);
    messages.push_back("remove applied to " + std::string(bot->GetName()));
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleReset(std::string param)
{
    std::list<std::string> messages;
    CharacterDatabase.PExecute("delete from ai_playerbot_random_bots where event not in ('temporary')");
    sRandomPlayerbotMgr.eventCache.clear();
    std::string msg = "Random bots were reset for all players. Please restart the Server.";
    messages.push_back(msg);
    return messages;
}

uint32 RandomPlayerbotMgr::ResolveActionHistorySize(Player* bot)
{
    // Stored as size + 1, so 0 means "inherit the global setting" and >=1 is explicit.
    uint32 stored = GetEventValue(bot->GetGUIDLow(), "action_history");
    if (!stored)
        return sPlayerbotAIConfig.actionHistorySize;

    return stored - 1;
}

void RandomPlayerbotMgr::ApplyActionHistorySize(Player* bot)
{
    if (PlayerbotAI* ai = bot->GetPlayerbotAI())
        ai->SetActionHistorySize(ResolveActionHistorySize(bot));
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleHistory(std::string param)
{
    std::list<std::string> messages;

    std::vector<std::string> tokens;
    for (auto& token : Qualified::getMultiQualifiers(param, " "))
        if (!token.empty())
            tokens.push_back(token);

    if (tokens.empty() || tokens.size() > 2)
    {
        uint32 on = 0, total = 0;
        ForEachPlayerbot([&](Player* bot)
        {
            total++;
            if (bot->GetPlayerbotAI() && bot->GetPlayerbotAI()->GetActionHistorySize())
                on++;
        });

        messages.push_back("Usage: history <off|on|N> | history <botname> <off|on|N>");
        messages.push_back("Action history enabled on " + std::to_string(on) + " / " + std::to_string(total) + " bots (global default " + std::to_string(sPlayerbotAIConfig.actionHistorySize) + ").");
        return messages;
    }

    std::string name = "%";
    std::string setting = tokens[0];
    if (tokens.size() == 2)
    {
        name = tokens[0];
        setting = tokens[1];
    }

    int size = -1;
    if (setting == "off")
        size = 0;
    else if (setting == "on")
        size = sPlayerbotAIConfig.actionHistorySize ? (int)sPlayerbotAIConfig.actionHistorySize : 64;
    else if (Qualified::isValidNumberString(setting))
        size = std::stoi(setting);
    else
    {
        messages.push_back("Unknown setting: " + setting);
        messages.push_back("Usage: history <off|on|N> | history <botname> <off|on|N>");
        return messages;
    }

    if (size < 0)
        size = 0;

    uint32 stored = (uint32)size + 1;
    uint32 applied = 0;

    ForEachPlayerbot([&](Player* bot)
    {
        const char* botName = bot->GetName();
        if (name != "%" && (!botName || std::string(botName).find(name) != 0))
            return;

        // validIn must be huge: GetEventValue() expires any event where (now - lastChange) >= validIn,
        // and validIn == 0 would therefore read back as 0 immediately.
        SetEventValue(bot->GetGUIDLow(), "action_history", stored, 0xFFFFFFFF);
        if (PlayerbotAI* ai = bot->GetPlayerbotAI())
            ai->SetActionHistorySize((uint32)size);

        applied++;
    });

    messages.push_back("Action history = " + std::to_string(size) + " for " + std::to_string(applied) + " bot(s).");
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleStats(std::string param)
{
    // In-game callers pass their player guid (set by HandlePlayerbotConsoleCommand).
    // The RA console has no session, so param is empty -> build the histogram
    // synchronously and return it. RA commands run on the world thread
    // (World::ProcessCliCommands), so this pass is safe.
    if (!Qualified::isValidNumberString(param))
    {
        return FormatBotStats(GatherBotStats());
    }

    std::list<std::string> messages;
    std::string msg = "Stats requested.";
    messages.push_back(msg);

    ObjectGuid guid = ObjectGuid(uint64(std::stoull(param)));
    activatePrintStatsThread(guid);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleReload(std::string param)
{
    std::list<std::string> messages;
    sPlayerbotAIConfig.Initialize();
    std::string msg = "Playerbot config reloaded.";
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleUpdate(std::string param)
{
    std::list<std::string> messages;
    sRandomPlayerbotMgr.UpdateAIInternal(0);
    std::string msg = "Playerbot update triggered.";
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsolePid(std::string param)
{
    std::list<std::string> messages;
    std::string pids = param.substr(4);
    std::vector<std::string> pid = Qualified::getMultiQualifiers(pids, " ");

    if (pid.size() == 0)
        pid.push_back("0");
    if (pid.size() == 1)
        pid.push_back("0");
    if (pid.size() == 2)
        pid.push_back("0");
    sRandomPlayerbotMgr.pid.adjust(stof(pid[0]), stof(pid[1]), stof(pid[2]));

    std::string msg = "Pid set to p:" + std::to_string(stof(pid[0])) + " i:" + std::to_string(stof(pid[1])) + " d:" + std::to_string(stof(pid[2]));
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleDiff(std::string param)
{
    std::list<std::string> messages;
    if (param.empty())
    {
        std::stringstream ss;
        ss << "Avg diff: " << sWorld.GetAverageDiff() << "\n";
        ss << "Max diff: " << sWorld.GetMaxDiff() << "\n";
        ss << "char db ping: " << sRandomPlayerbotMgr.GetDatabaseDelay("CharacterDatabase") << "\n";
        ss << "Sessions online: " << sWorld.GetActiveSessionCount() << "\n";
        ss << "Bots online: " << sRandomPlayerbotMgr.botCount << " (active: " << sRandomPlayerbotMgr.activeBots << ")";

        messages.push_back(ss.str());
        return messages;
    }
    else if (param.find(" ") != std::string::npos)
    {
        std::vector<std::string> diff = Qualified::getMultiQualifiers(param, " ");
        if (diff.size() == 0)
            diff.push_back("100");
        if (diff.size() == 1)
            diff.push_back(diff[0]);
        sPlayerbotAIConfig.diffWithPlayer = stoi(diff[0]);
        sPlayerbotAIConfig.diffEmpty = stoi(diff[1]);

        std::string msg = "Diff set to " + std::to_string(stoi(diff[0])) + " (player), " + std::to_string(stoi(diff[1])) + " (empty)";
        messages.push_back(msg);
        return messages;
    }
    return messages;
}

static std::string GridWatchCounters(char const* label)
{
    std::ostringstream out;
    out << label << ": creature=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_GENERIC)
        << " pet=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_PET)
        << " totem=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_TOTEM)
        << " temp=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_TEMP_SUMMON)
        << " gameobject=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_GAMEOBJECT)
        << " gridmap=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_GRIDMAP)
        << " private=" << (sMemoryMonitor.GetPrivateUsage() / 1048576) << "MB";
    return out.str();
}

static float GridWatchWorld(uint32 grid)
{
    return ((float)grid - CENTER_GRID_ID) * SIZE_OF_GRIDS + CENTER_GRID_OFFSET;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleGrid(std::string param)
{
    std::list<std::string> messages;

    std::vector<std::string> args;
    for (auto& arg : Qualified::getMultiQualifiers(param, " "))
        if (!arg.empty())
            args.push_back(arg);

    if (args.empty() || args[0] == "status")
    {
        messages.push_back(GridWatchCounters("grid status"));
        uint32 mapId = args.size() > 1 ? (uint32)atoi(args[1].c_str()) : 0;
        if (Map* map = sMapMgr.FindMap(mapId, 0))
        {
            uint32 loaded = 0;
            uint32 minX = MAX_NUMBER_OF_GRIDS, maxX = 0, minY = MAX_NUMBER_OF_GRIDS, maxY = 0;
            for (uint32 gx = 0; gx < MAX_NUMBER_OF_GRIDS; ++gx)
                for (uint32 gy = 0; gy < MAX_NUMBER_OF_GRIDS; ++gy)
                    if (map->IsLoaded(GridWatchWorld(gx), GridWatchWorld(gy)))
                    {
                        ++loaded;
                        minX = std::min(minX, gx);
                        maxX = std::max(maxX, gx);
                        minY = std::min(minY, gy);
                        maxY = std::max(maxY, gy);
                    }
            messages.push_back("grid status: map " + std::to_string(mapId) + " loaded grids=" + std::to_string(loaded) +
                (loaded ? " X " + std::to_string(minX) + "-" + std::to_string(maxX) + " Y " + std::to_string(minY) + "-" + std::to_string(maxY) : ""));
        }
        else
            messages.push_back("grid status: map " + std::to_string(mapId) + " not found");
        return messages;
    }

    if (args.size() < 4 || (args[0] != "load" && args[0] != "unload" && args[0] != "cycle"))
    {
        messages.push_back("Usage: grid status [map] | grid load <map> <gx> <gy> [n] | grid unload <map> <gx> <gy> [n] | grid cycle <map> <gx> <gy> [n]");
        return messages;
    }

    uint32 mapId = (uint32)atoi(args[1].c_str());
    uint32 startX = (uint32)atoi(args[2].c_str());
    uint32 startY = (uint32)atoi(args[3].c_str());
    uint32 count = args.size() > 4 ? (uint32)atoi(args[4].c_str()) : 1;
    if (!count)
        count = 1;
    if (count > MAX_NUMBER_OF_GRIDS)
        count = MAX_NUMBER_OF_GRIDS;

    Map* map = sMapMgr.FindMap(mapId, 0);
    if (!map)
    {
        messages.push_back("grid: map " + std::to_string(mapId) + " not found");
        return messages;
    }

    messages.push_back("grid " + args[0] + ": map " + std::to_string(mapId) + " from " + std::to_string(startX) + "," + std::to_string(startY) + " n=" + std::to_string(count));
    messages.push_back(GridWatchCounters("grid start"));

    if (args[0] == "load" || args[0] == "cycle")
    {
        uint32 loaded = 0;
        for (uint32 i = 0; i < count; ++i)
        {
            uint32 gx = startX + (i % 8);
            uint32 gy = startY + (i / 8);
            if (gx >= MAX_NUMBER_OF_GRIDS || gy >= MAX_NUMBER_OF_GRIDS)
                break;
            if (map->IsLoaded(GridWatchWorld(gx), GridWatchWorld(gy)))
                continue;
            map->ForceLoadGrid(GridWatchWorld(gx), GridWatchWorld(gy));
            ++loaded;
        }
        messages.push_back("grid load: loaded " + std::to_string(loaded));
        messages.push_back(GridWatchCounters("grid after load"));
    }

    if (args[0] == "unload" || args[0] == "cycle")
    {
        uint32 unloaded = 0;
        for (uint32 i = 0; i < count; ++i)
        {
            uint32 gx = startX + (i % 8);
            uint32 gy = startY + (i / 8);
            if (gx >= MAX_NUMBER_OF_GRIDS || gy >= MAX_NUMBER_OF_GRIDS)
                break;
            if (!map->IsLoaded(GridWatchWorld(gx), GridWatchWorld(gy)))
                continue;
            if (map->UnloadGrid(gx, gy, false))
                ++unloaded;
        }
        messages.push_back("grid unload: unloaded " + std::to_string(unloaded));
        messages.push_back(GridWatchCounters("grid after unload"));
    }

    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleMem(std::string param)
{
    std::list<std::string> messages;
    std::ostringstream out;

    out << "mem counters: creature=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_GENERIC)
        << " pet=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_PET)
        << " totem=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_TOTEM)
        << " temp=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_CREATURE_TEMP_SUMMON)
        << " gameobject=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_GAMEOBJECT)
        << " gridmap=" << sMemoryMonitor.GetCount(OBJECT_COUNTER_GRIDMAP);
    messages.push_back(out.str());

    uint64 privateBytes = sMemoryMonitor.GetPrivateUsage();
    uint32 regions = 0;
    uint64 regionBytes = 0;
#if PLATFORM == PLATFORM_WINDOWS
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    uintptr_t address = (uintptr_t)systemInfo.lpMinimumApplicationAddress;
    uintptr_t maxAddress = (uintptr_t)systemInfo.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION memoryInfo;
    while (address < maxAddress)
    {
        if (!VirtualQuery((LPCVOID)address, &memoryInfo, sizeof(memoryInfo)))
            break;

        if (memoryInfo.State == MEM_COMMIT && memoryInfo.Type == MEM_PRIVATE)
        {
            ++regions;
            regionBytes += memoryInfo.RegionSize;
        }

        address += (uintptr_t)memoryInfo.RegionSize;
    }
#endif

    out.str("");
    out << "mem heap: private=" << (privateBytes / 1048576) << "MB regions=" << regions
        << " regionMB=" << (regionBytes / 1048576)
        << " avgRegionKB=" << (regions ? (regionBytes / regions / 1024) : 0);
    messages.push_back(out.str());

    MMAP::MMapManager* mmapManager = MMAP::MMapFactory::createOrGetMMapManager();
    VMAP::VMapManager2* vmapManager = dynamic_cast<VMAP::VMapManager2*>(VMAP::VMapFactory::createOrGetVMapManager());

    uint32 vmapTiles = 0;
    if (vmapManager)
    {
        for (uint32 mapId : { 0, 1, 530, 571, 609 })
            for (uint32 gx = 0; gx < MAX_NUMBER_OF_GRIDS; ++gx)
                for (uint32 gy = 0; gy < MAX_NUMBER_OF_GRIDS; ++gy)
                    if (vmapManager->IsTileLoaded(mapId, gx, gy))
                        ++vmapTiles;
    }

    // Loaded model/map counters need core-side accessors that only exist on wotlk;
    // vanilla/tbc report 0 so the output format stays identical across cores.
    uint32 mmapModels = 0;
    uint32 vmapMaps = 0;
    uint32 vmapModels = 0;

    out.str("");
    out << "mem engine: mmapMaps=" << (mmapManager ? mmapManager->getLoadedMapsCount() : 0)
        << " mmapTiles=" << (mmapManager ? mmapManager->getLoadedTilesCount() : 0)
        << " mmapModels=" << mmapModels
        << " vmapMaps=" << vmapMaps
        << " vmapModels=" << vmapModels
        << " vmapTiles=" << vmapTiles;
    messages.push_back(out.str());

    messages.push_back(sTravelMgr.GetCacheCensus());

    uint32 eventBots = 0;
    uint32 events = 0;
    for (auto& botEvents : eventCache)
    {
        ++eventBots;
        events += botEvents.second.size();
    }

    out.str("");
    out << "mem module: eventBots=" << eventBots << " events=" << events
        << " ahMirror=" << ahMirror.size()
        << " locsPerLevel=" << locsPerLevelCache.size()
        << " rpgLocs=" << rpgLocsCache.size()
        << " rpgLocsLevel=" << rpgLocsCacheLevel.size()
        << " battleMasters=" << BattleMastersCache.size()
        << " lfg=" << LfgDungeons.size()
        << " dbDelay=" << databaseDelay.size()
        << " needBots=" << NeedBots.size()
        << " bgBots=" << BgBots.size()
        << " visualBots=" << VisualBots.size()
        << " bgPlayers=" << BgPlayers.size()
        << " arenaBots=" << ArenaBots.size()
        << " rating=" << Rating.size()
        << " supporters=" << Supporters.size();
    messages.push_back(out.str());

    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleCleanMap(std::string param)
{
    std::list<std::string> messages;
    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        if (!sMapStore.LookupEntry(i))
            continue;

        uint32 mapId = sMapStore.LookupEntry(i)->MapID;
        boost::thread t([mapId]() {WorldPosition::unloadMapAndVMaps(mapId); });
        t.detach();
    }

    std::string msg = "Map cleaning initiated.";
    messages.push_back(msg);
    return messages;
}

std::list<std::string> RandomPlayerbotMgr::HandleConsoleLoginDebug(std::string param)
{
    std::list<std::string> messages;
    sPlayerBotLoginMgr.ToggleDebug();
    std::string msg = "Login debug toggled.";
    messages.push_back(msg);
    return messages;
}

uint32 RandomPlayerbotMgr::GetOrCreateAccount(Player* master, std::string& error)
{
    uint32 maxCharsPerAccount = 9;
#ifdef MANGOSBOT_TWO
    maxCharsPerAccount = 10;
#endif

    auto accountNrQr = LoginDatabase.PQuery("SELECT max(replace(lower(username), lower('%s'), '') + 1 - 1) maxAccountNr FROM account WHERE replace(lower(username), lower('%s'), '') != 0", sPlayerbotAIConfig.randomBotAccountPrefix.c_str(), sPlayerbotAIConfig.randomBotAccountPrefix.c_str());

    if (!accountNrQr)
    {
        error = "Failed to find last " + sPlayerbotAIConfig.randomBotAccountPrefix + " account nr.";
        return 0;
    }

    Field* fields = accountNrQr->Fetch();
    uint32 accountNumber = 0;
    uint32 maxAccountNum = fields[0].GetUInt32();

    for (uint32 i = 0; i < 10000; i++)
    {
        std::ostringstream accountNameStr;
        accountNameStr << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
        std::string accountName = accountNameStr.str();

        uint32 accountId = sAccountMgr.GetId(accountName);

        if (!accountId)
        {
            std::string password;
            if (sPlayerbotAIConfig.randomBotRandomPassword)
            {
                for (int i = 0; i < 10; i++)
                    password += (char)urand('!', 'z');
            }
            else
                password = accountName;

            LoginDatabase.BeginTransaction();
#ifndef MANGOSBOT_ZERO
            uint8 max_expansion = MAX_EXPANSION;
            AccountOpResult result = sAccountMgr.CreateAccount(accountName, password, max_expansion);
#else
            AccountOpResult result = sAccountMgr.CreateAccount(accountName, password);
#endif
            LoginDatabase.CommitTransactionDirect();

            if (result == AOR_OK)
            {
                uint32 accountId = sAccountMgr.GetId(accountName);
                if (accountId)
                {
                    sPlayerbotAIConfig.randomBotAccounts.push_back(accountId);
                    return accountId;
                }
            }

            error = "Failed to create account";
            return 0;        
        }

        uint32 charCount = sAccountMgr.GetCharactersCount(accountId);

        if (charCount < maxCharsPerAccount)
        {
            if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
            {
                sPlayerbotAIConfig.randomBotAccounts.push_back(accountId);
            }
            return accountId;
        }

        accountNumber++;
    }

    error = "Failed to find a suitable account.";
    return 0;
}

void RandomPlayerbotMgr::OnBotDeleted(uint32 botGuid, uint32 accountId)
{
    if (accountId > 0 && sPlayerbotAIConfig.IsInRandomAccountList(accountId))
    {
        uint32 maxCharsPerAccount = 9;
    #ifdef MANGOSBOT_TWO
        maxCharsPerAccount = 10;
    #endif
    
        if (sAccountMgr.GetCharactersCount(accountId) == 0)
        {
            std::ostringstream prefix;
            prefix << sPlayerbotAIConfig.randomBotAccountPrefix;
            size_t prefixLen = prefix.str().length();
            
            auto result = LoginDatabase.PQuery("SELECT username FROM account WHERE id = '%u'", accountId);
            if (result)
            {
                std::string username = result->Fetch()[0].GetString();
                if (username.substr(0, prefixLen) == prefix.str())
                {
                    uint32 accountNum = std::stoul(username.substr(prefixLen));
                    if (accountNum >= sPlayerbotAIConfig.randomBotAccountCount)
                    {
                        sAccountMgr.DeleteAccount(accountId);
                        sLog.outString("Deleted empty random bot account: %s (id: %u)", username.c_str(), accountId);
                    }
                }
            }
        }
    }
}