// Silvermoon: the realm's own bot management (dungeon runs, guilds, crafting, watchdogs,
// telemetry, GM commands). Moved out of RandomPlayerbotMgr.cpp unchanged (brain Phase 1 step 1);
// the blocks below are RandomPlayerbotMgr member functions called from the same places as before.
#include "Config/Config.h"

#include "playerbot/playerbot.h"
#include "playerbot/TravelNode.h"
#include <atomic>
#include <array>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/silvermoon/BrainTimer.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/RandomItemMgr.h"
#include "Tools/Formulas.h"
#include "playerbot/strategy/actions/GuildCreateActions.h"
#include "playerbot/RandomPlayerbotFactory.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/strategy/values/CraftValues.h"
#include "AuctionHouse/AuctionHouseMgr.h"
#include "playerbot/strategy/values/LastMovementValue.h"
#include "Accounts/AccountMgr.h"
#include "Globals/ObjectMgr.h"
#include "Database/DatabaseEnv.h"
#include "playerbot/PlayerbotAI.h"
#include "Entities/Player.h"
#include "Entities/Pet.h"
#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotCommandServer.h"
#include "playerbot/MemoryMonitor.h"

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
#include "playerbot/FleeManager.h"
#include "playerbot/ServerFacade.h"

#include "BattleGround/BattleGround.h"
#include "BattleGround/BattleGroundMgr.h"

#ifdef GenerateBotTests
#include "playerbot/strategy/tests/TestRegistry.h"
#include "playerbot/strategy/tests/TravelProbe.h"
#endif
#include "Chat/ChannelMgr.h"
#include "Guilds/GuildMgr.h"
#include "World/WorldState.h"
#include "playerbot/PlayerbotLoginMgr.h"
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
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/silvermoon/DecisionLog.h"
#include "playerbot/silvermoon/QuestIndex.h"
#include "playerbot/silvermoon/Hubs.h"
#include "Chat/Chat.h"
#include "playerbot/strategy/silvermoon/Policies.h"
#include "playerbot/strategy/silvermoon/Professions.h"
#include "playerbot/silvermoon/Crowd.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/strategy/silvermoon/ChooseGoalAction.h"
#include "playerbot/strategy/silvermoon/RelocateGoal.h"
#include "playerbot/strategy/values/LastMovementValue.h"

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
std::set<uint32> g_silvermoonPriorityBots;

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

// Silvermoon patch: gathering professions early. Bots reached level 10 with no profession at all
// (Fizzlewick, Grashnak), missing the easiest earned money. A random bot of level 5+ resting in
// town with a free profession slot learns a gathering one - Skinning first, then Mining or
// Herbalism - paying the 10 silver fee and the vendor price of the tool it needs.
// Silvermoon patch: dual wield (S: a level 10 rogue should train Dual Wield and get a second
// weapon first). In town, before professions and bags: a rogue from 10, or a warrior or hunter
// from 20, without Dual Wield pays the trainer fee and learns it; a bot with Dual Wield, a
// one-handed main hand and an empty off hand buys the best plain vendor one-hander it can use
// (dagger, sword, axe or mace by its weapon skills) for at most half its money.
// Silvermoon patch: the level-zone teleports (death-loop rescue, stuck spots, failed trips) went
// through the stock random teleport, which picks any zone with mobs of the bot's level - it sent
// Alliance 20s to Southshore (Hillsbrad: Horde-heavy, an Alliance 32-39 hub here) with Redridge
// quests still open. Now: the inn of the nearest own-faction hub the bot is 2+ levels into (same
// continent first). Returns false when none fits, and the caller falls back to the stock teleport.
static bool SilvermoonHubTeleport(Player* bot)
{
    bool alliance = bot->GetTeam() == ALLIANCE;
    int32 level = int32(bot->GetLevel());
    WorldPosition here(bot);
    silvermoon::Hub const* best = nullptr;
    float bestDist = 0.0f;
    for (silvermoon::Hub const& h : silvermoon::Hubs::All())
    {
        if (!(h.side == 3 || h.side == (alliance ? 1 : 2)))
            continue;
        if (level < int32(h.minLevel) + 2 || level > int32(h.maxLevel) - 2)
            continue;
        float dist = h.map == here.getMapId() ? here.fDist(WorldPosition(h.map, h.x, h.y, h.z)) : 1000000.0f;
        if (!best || dist < bestDist)
        {
            best = &h;
            bestDist = dist;
        }
    }
    if (!best)
        return false;
    float x = best->innkeeper ? best->innX : best->x, y = best->innkeeper ? best->innY : best->y, z = best->innkeeper ? best->innZ : best->z;
    sLog.outBasic("Silvermoon: bot %s (level %u) moved to hub %s", bot->GetName(), bot->GetLevel(), best->name);
    return bot->TeleportTo(best->map, x, y, z, bot->GetOrientation());
}

// plain vendor one-handers: { item, weapon skill, required level }, best first
static const uint32 silvermoonVendorWeapons[][3] = {
    { 2526, 173, 29 }, { 2209, 173, 19 }, { 927, 44, 19 }, { 2028, 54, 16 }, { 2029, 44, 15 },
    { 2208, 173, 14 }, { 2027, 43, 14 }, { 2207, 173, 11 }, { 853, 44, 11 }, { 851, 43, 10 },
    { 852, 54, 9 }, { 2490, 44, 4 }, { 2488, 43, 4 }, { 2494, 173, 3 }, { 2492, 54, 2 } };

static float SilvermoonWeaponDps(ItemPrototype const* proto)
{
    if (!proto || !proto->Delay)
        return 0.0f;
    return (proto->Damage[0].DamageMin + proto->Damage[0].DamageMax) / 2.0f / (proto->Delay / 1000.0f);
}

// Silvermoon patch: main-hand upgrades (S: Guile had nearly 7 silver and still swung his level-1
// Worn Dagger, 0.9 dps, with a 3.3 dps Stiletto at 4 silver on the vendor). In town, a melee or
// hunter bot whose main hand is a one-hander (or empty) buys the best plain vendor one-hander it can
// use when that is at least 1.5x the damage per second, for at most 70% of its money. The old weapon
// goes to the bags and is sold on the next vendor visit. Runs before Dual Wield.
static void SilvermoonWeaponUpgrade(Player* bot)
{
    uint8 cls = bot->getClass();
    if (cls != CLASS_ROGUE && cls != CLASS_WARRIOR && cls != CLASS_HUNTER && cls != CLASS_PALADIN && cls != CLASS_SHAMAN)
        return;
    Item* main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (main && main->GetProto()->InventoryType == INVTYPE_2HWEAPON)
        return;                                      // two-handers are left to loot and quests
    float current = main ? SilvermoonWeaponDps(main->GetProto()) : 0.0f;
    uint32 level = bot->GetLevel();
    for (auto const& w : silvermoonVendorWeapons)
    {
        if (w[2] > level || !bot->HasSkill(w[1]) || (SilvermoonIsOwn(bot) && SilvermoonWeaponAvoided(bot->GetGUIDLow(), w[1])))
            continue;
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(w[0]);
        if (!proto || proto->BuyPrice > SilvermoonUpgradeBudget(bot, 70))
            continue;
        if (SilvermoonWeaponDps(proto) < current * 1.5f)
            continue;
        if (main)
        {
            ItemPosCountVec space;
            uint8 bagSlot = 0;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, space, main, bagSlot, false) != EQUIP_ERR_OK)
                return;                              // no room for the old weapon
            bot->RemoveItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND, true);
            bot->StoreItem(space, main, true);
        }
        uint16 dest;
        if (bot->CanEquipNewItem(EQUIPMENT_SLOT_MAINHAND, dest, w[0], false) != EQUIP_ERR_OK)
            return;
        bot->EquipNewItem(dest, w[0], true);
        bot->ModifyMoney(-int32(proto->BuyPrice));
        sLog.outBasic("Silvermoon: bot %s (level %u) bought a main-hand %s for %u copper (%.1f -> %.1f dps)",
            bot->GetName(), level, proto->Name1, proto->BuyPrice, current, SilvermoonWeaponDps(proto));
        return;
    }
}

// Silvermoon patch (S): quests a bot hasn't finished within 5 levels are abandoned - they clog the
// log and pull the bot back to low zones. Unfinished (not ready to hand in), not class quests. The
// level a quest was first seen in the log is remembered; without a record (after a restart) the
// quest's own level stands in for it.
// Silvermoon (S): your character on Auto never swaps gear, but an EMPTY main hand is filled from the
// bags with the best weapon it can use (it switched to Auto unarmed and fought with its fists)
static void SilvermoonFillEmptyWeapon(Player* bot)
{
    if (bot->IsInCombat() || bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
        return;
    Item* best = nullptr;
    float bestDps = 0.0f;
    auto consider = [&](Item* item)
    {
        if (!item)
            return;
        ItemPrototype const* proto = item->GetProto();
        if (proto->Class != ITEM_CLASS_WEAPON || (proto->InventoryType != INVTYPE_WEAPON && proto->InventoryType != INVTYPE_WEAPONMAINHAND &&
            proto->InventoryType != INVTYPE_2HWEAPON) || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return;
        if (item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) && !item->GetUInt32Value(ITEM_FIELD_DURABILITY))
            return;
        float dps = SilvermoonWeaponDps(proto);
        if (!best || dps > bestDps)
        {
            best = item;
            bestDps = dps;
        }
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                consider(bag->GetItemByPos(j));
    if (!best)
        return;
    uint16 dest;
    if (bot->CanEquipItem(EQUIPMENT_SLOT_MAINHAND, dest, best, true) != EQUIP_ERR_OK)
        return;
    bot->SwapItem(uint16((best->GetBagSlot() << 8) | best->GetSlot()), uint16((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND));
    sLog.outBasic("Silvermoon: %s on Auto had no weapon, equipped %s from its bags", bot->GetName(), best->GetProto()->Name1);
}

static void SilvermoonDropStaleQuests(Player* bot)
{
    static std::mutex lock;
    static std::unordered_map<uint32, std::unordered_map<uint32, uint32>> seenAt;   // guid -> quest -> level
    uint32 level = bot->GetLevel();
    std::lock_guard<std::mutex> guard(lock);
    auto& seen = seenAt[bot->GetGUIDLow()];
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr.GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        auto it = seen.find(questId);
        if (it == seen.end())
            it = seen.emplace(questId, level).first;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE || quest->GetRequiredClasses())
            continue;
        int32 questLevel = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : int32(level);
        bool stale = int32(level) >= int32(it->second) + 5 || int32(level) >= questLevel + 5;
        if (!stale)
            continue;
        bot->SetQuestSlot(slot, 0);
        bot->TakeQuestSourceItem(questId, false);
        bot->SetQuestStatus(questId, QUEST_STATUS_NONE);
        bot->getQuestStatusMap()[questId].m_rewarded = false;
        seen.erase(it);
        sLog.outBasic("Silvermoon: bot %s (level %u) abandoned quest %u \"%s\" (level %d, unfinished for 5 levels)",
            bot->GetName(), level, questId, quest->GetTitle().c_str(), questLevel);
    }
}

static void SilvermoonDualWield(Player* bot)
{
    uint8 cls = bot->getClass();
    uint32 level = bot->GetLevel();
    uint32 fee = cls == CLASS_ROGUE ? 300 : 2200;
    if (!((cls == CLASS_ROGUE && level >= 10) || ((cls == CLASS_WARRIOR || cls == CLASS_HUNTER) && level >= 20)))
        return;
    if (!bot->HasSpell(674))
    {
        if (bot->GetMoney() < fee)
            return;
        bot->learnSpell(674, false);
        bot->ModifyMoney(-int32(fee));
        sLog.outBasic("Silvermoon: bot %s (level %u) learned Dual Wield for %u copper", bot->GetName(), level, fee);
    }
    if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
        return;
    Item* main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!main || main->GetProto()->InventoryType == INVTYPE_2HWEAPON)
        return;
    for (auto const& w : silvermoonVendorWeapons)
    {
        if (w[2] > level || !bot->HasSkill(w[1]) || (SilvermoonIsOwn(bot) && SilvermoonWeaponAvoided(bot->GetGUIDLow(), w[1])))
            continue;
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(w[0]);
        if (!proto || proto->BuyPrice > SilvermoonUpgradeBudget(bot, 50))
            continue;
        uint16 dest;
        if (bot->CanEquipNewItem(EQUIPMENT_SLOT_OFFHAND, dest, w[0], false) != EQUIP_ERR_OK)
            continue;
        bot->EquipNewItem(dest, w[0], true);
        bot->ModifyMoney(-int32(proto->BuyPrice));
        sLog.outBasic("Silvermoon: bot %s bought an off-hand %s for %u copper", bot->GetName(), proto->Name1, proto->BuyPrice);
        return;
    }
}

static void SilvermoonLearnGathering(Player* bot)
{
    enum { MIN = 186, HERB = 182, SKIN = 393, BS = 164, LW = 165, ALCH = 171, TAIL = 197, ENG = 202, ENCH = 333 };
    // a gatherer without its tool gathers nothing: 155 of 215 online miners had no Mining Pick and
    // 164 of 218 skinners no Skinning Knife (professions given by older code came without them)
    for (auto const& need : { std::make_pair(uint32(MIN), 2901u), std::make_pair(uint32(SKIN), 7005u) })
        if (bot->HasSkill(need.first) && !bot->HasItemCount(need.second, 1))
            if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(need.second))
                if (bot->GetMoney() >= proto->BuyPrice * 2)
                {
                    ItemPosCountVec dest;
                    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, need.second, 1) == EQUIP_ERR_OK)
                    {
                        bot->StoreNewItem(dest, need.second, true);
                        bot->ModifyMoney(-int32(proto->BuyPrice));
                        sLog.outBasic("Silvermoon: bot %s bought a %s for %u copper", bot->GetName(), proto->Name1, proto->BuyPrice);
                    }
                }
    if (bot->GetLevel() < 5)
        return;
    uint32 primaries = 0;
    for (uint32 skill : { MIN, HERB, SKIN, BS, LW, ALCH, TAIL, ENG, ENCH })
        if (bot->HasSkill(skill))
            ++primaries;
    // a profession learned this session may not show as a skill yet (Achris learned Skinning,
    // Herbalism and Mining within a minute): count the apprentice spells too
    for (auto const& known : { std::make_pair(uint32(SKIN), 8613u), std::make_pair(uint32(MIN), 2575u), std::make_pair(uint32(HERB), 2366u) })
        if (!bot->HasSkill(known.first) && bot->HasSpell(known.second))
            ++primaries;
    if (primaries >= 2)
        return;
    struct Gathering { uint32 skill, spell, tool; };
    std::vector<Gathering> options;
    if (!bot->HasSkill(SKIN) && !bot->HasSpell(8613))
        options.push_back({ SKIN, 8613, 7005 });     // Skinning + Skinning Knife
    if (!bot->HasSkill(MIN) && !bot->HasSkill(HERB) && !bot->HasSpell(2575) && !bot->HasSpell(2366))
    {
        if (urand(0, 1))
            options.push_back({ MIN, 2575, 2901 });  // Mining + Mining Pick
        else
            options.push_back({ HERB, 2366, 0 });    // Herbalism
    }
    if (options.empty())
        return;
    Gathering const& g = options.front();
    uint32 toolPrice = 0;
    if (g.tool && !bot->HasItemCount(g.tool, 1))
        if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(g.tool))
            toolPrice = proto->BuyPrice;
    // the trainers' real apprentice fee is 10 copper (was charged as 10 silver, which kept most
    // broke bots from ever learning a way to earn without fighting)
    uint32 cost = 10 + toolPrice;
    if (bot->GetMoney() < cost + 50)
        return;
    if (toolPrice)
    {
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, g.tool, 1) != EQUIP_ERR_OK)
            return;
        bot->StoreNewItem(dest, g.tool, true);
    }
    bot->learnSpell(g.spell, false);
    bot->ModifyMoney(-int32(cost));
    sLog.outBasic("Silvermoon: bot %s (level %u) learned gathering skill %u for %u copper (fee and tool)", bot->GetName(), bot->GetLevel(), g.skill, cost);
}

// Silvermoon patch: bags. Full bags leave loot on corpses and force extra vendor trips. A random
// bot resting in town with an empty bag slot buys the biggest plain vendor bag it can afford with
// at most a quarter of its money (6, 8 or 10 slots), paying the vendor price.
static void SilvermoonBuyBag(Player* bot)
{
    // policies (S): your characters can be told not to buy bags, or to replace small bags with
    // bigger ones
    uint8 rules = SilvermoonIsOwn(bot) ? SilvermoonPolicyFor(bot->GetGUIDLow()).bags : 0;
    if (rules & 1)
        return;
    uint8 freeSlot = 0;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            freeSlot = slot;
            break;
        }
    if (!freeSlot && (rules & 2))
    {
        // the smallest equipped bag, if it is empty and a bigger vendor bag fits the budget: sold
        // (vendor price) and replaced in its slot
        Bag* smallest = nullptr;
        uint8 smallSlot = 0;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (bag->GetProto()->Class == ITEM_CLASS_CONTAINER && bag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER &&
                    (!smallest || bag->GetBagSize() < smallest->GetBagSize()))
                {
                    smallest = bag;
                    smallSlot = slot;
                }
        if (!smallest || !smallest->IsEmpty())
            return;
        for (uint32 entry : { 4497u, 4498u })    // Heavy Brown Bag 10, Brown Leather Satchel 8
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
            if (!proto || proto->ContainerSlots <= smallest->GetBagSize() || proto->BuyPrice > SilvermoonUpgradeBudget(bot, 50))
                continue;
            uint32 refund = smallest->GetProto()->SellPrice;
            std::string old = smallest->GetProto()->Name1;
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, smallSlot, true);
            uint16 dest;
            if (bot->CanEquipNewItem(smallSlot, dest, entry, false) != EQUIP_ERR_OK)
                return;
            bot->EquipNewItem(dest, entry, true);
            bot->ModifyMoney(int32(refund) - int32(proto->BuyPrice));
            sLog.outBasic("Silvermoon: %s replaced a %s with a %s (policy)", bot->GetName(), old.c_str(), proto->Name1);
            return;
        }
        return;
    }
    if (!freeSlot)
        return;
    for (uint32 entry : { 4497u, 4498u, 4496u })    // Heavy Brown Bag 10, Brown Leather Satchel 8, Small Brown Pouch 6
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
        // S: bags are a priority - up to half the bot's money (was a quarter)
        if (!proto || proto->BuyPrice > SilvermoonUpgradeBudget(bot, 50))
            continue;
        uint16 dest;
        if (bot->CanEquipNewItem(freeSlot, dest, entry, false) != EQUIP_ERR_OK)
            continue;
        bot->EquipNewItem(dest, entry, true);
        bot->ModifyMoney(-int32(proto->BuyPrice));
        sLog.outBasic("Silvermoon: bot %s bought a %s for %u copper", bot->GetName(), proto->Name1, proto->BuyPrice);
        return;
    }
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
    // Silvermoon (S): profession ranks (Journeyman, Expert, ...) are trained as soon as the skill
    // reaches the rank's requirement, wherever the bot rests, for up to half its money - the craft
    // pass then keeps skilling up. New recipes stay capital-only at the 5% rule below.
    {
        struct TradeSpell { uint32 skill, skillValue, level, cost, spell; bool rank; };
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
                    bool rank = false;
                    if (SpellEntry const* learnedInfo = sServerFacade.LookupSpellInfo(learned))
                        for (int j = 0; j < 3; ++j)
                            if (learnedInfo->Effect[j] == SPELL_EFFECT_SKILL_STEP)
                                rank = true;
                    if (found == bySpell.end() || tSpell.spellCost < found->second.cost)
                        bySpell[learned] = { tSpell.reqSkill, tSpell.reqSkillValue, tSpell.reqLevel, tSpell.spellCost, learned, rank };
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
            if (t.rank ? uint64(t.cost) * 2 > bot->GetMoney() : (!inCapital || uint64(t.cost) * 20 >= bot->GetMoney()))
                continue; // rank: at most half its money; recipe: in a capital, under 5% of its money
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
        if (crafted >= 20)
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
            // the same recipe again while it still gives skill-ups and the materials last (S: stacks
            // of Linen Cloth become bandages until they stop skilling First Aid)
            while (skillUp && crafted < 20 && ShouldCraftSpellValue::SpellGivesSkillUp(spellId, bot))
            {
                bool enough = true;
                for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (spell->Reagent[i] > 0 && spell->ReagentCount[i] && bot->GetItemCount(uint32(spell->Reagent[i])) < spell->ReagentCount[i])
                        enough = false;
                ItemPosCountVec again;
                if (!enough || bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, again, itemId, count) != EQUIP_ERR_OK)
                    break;
                for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (spell->Reagent[i] > 0 && spell->ReagentCount[i])
                        bot->DestroyItemCount(uint32(spell->Reagent[i]), spell->ReagentCount[i], true);
                bot->StoreNewItem(again, itemId, true, Item::GenerateItemRandomPropertyId(itemId));
                bot->UpdateCraftSkill(spellId);
                ++crafted;
            }
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


// Called from RandomPlayerbotMgr::UpdateAIInternal, right after the autologin/party-bot checks.
void RandomPlayerbotMgr::SilvermoonUpdate(uint32 elapsed)
{
    silvermoon::DecisionLog::FlushIfDue(); // brain decisions, batched every 30 seconds
    if (sPlayerbotAIConfig.silvermoonSpread)
        silvermoon::Crowd::Recount();      // Phase 2a crowding, every 60 seconds
    silvermoon::QuestIndex::Load();
    // Silvermoon: characters S asked for (silvermoon_new_chars, custom SQL 55), created once on a
    // random bot account and kept in the world as ordinary random bots, so their whole life can be
    // followed ("tracked characters")
    {
        static bool silvermoonNewCharsDone = false;
        if (!silvermoonNewCharsDone)
        {
            silvermoonNewCharsDone = true;
            auto pending = CharacterDatabase.Query("SELECT name, race, class, gender FROM silvermoon_new_chars WHERE guid = 0");
            if (pending)
            {
                do
                {
                    Field* f = pending->Fetch();
                    std::string name = f[0].GetCppString();
                    uint8 race = f[1].GetUInt8(), cls = f[2].GetUInt8(), gender = f[3].GetUInt8();
                    if (sObjectMgr.GetPlayerGuidByName(name))
                    {
                        sLog.outError("Silvermoon: tracked character name %s is taken", name.c_str());
                        continue;
                    }
                    // character counts per account, read once and kept in memory: saves are
                    // asynchronous, so a COUNT right after creating one still missed it and both
                    // tracked characters landed on an account that already had 9
                    static std::map<uint32, uint32> accountChars;
                    if (accountChars.empty())
                        for (uint32 acc : sPlayerbotAIConfig.randomBotAccounts)
                        {
                            auto count = CharacterDatabase.PQuery("SELECT COUNT(*) FROM characters WHERE account = '%u'", acc);
                            accountChars[acc] = count ? count->Fetch()[0].GetUInt32() : 10;
                        }
                    uint32 accountId = 0;
                    for (uint32 acc : sPlayerbotAIConfig.randomBotAccounts)
                        if (accountChars[acc] < 10)
                        {
                            accountId = acc;
                            ++accountChars[acc];
                            break;
                        }
                    if (!accountId)
                    {
                        sLog.outError("Silvermoon: no random bot account has room for %s", name.c_str());
                        continue;
                    }
                    // same session setup as RandomPlayerbotFactory::CreateRandomBot; the session is
                    // left alive like there (deleting a socketless session logs a player out)
                    WorldSession* session = new WorldSession(accountId, NULL, SEC_PLAYER,
#ifdef MANGOSBOT_ZERO
                        0, LOCALE_enUS, "", 0);
#else
                        2, 0, LOCALE_enUS, "", 0, 0, false);
#endif
                    session->SetNoAnticheat();
                    Player* player = new Player(session);
                    if (!player->Create(sObjectMgr.GeneratePlayerLowGuid(), name, race, cls, gender, 0, 0, 0, 0, 0, 0))
                    {
                        sLog.outError("Silvermoon: could not create tracked character %s", name.c_str());
                        delete player;
                        continue;
                    }
                    player->setCinematic(2);
                    player->SetAtLoginFlag(AT_LOGIN_NONE);
                    player->SaveToDB();
                    uint32 guid = player->GetGUIDLow();
                    delete player;
                    CharacterDatabase.escape_string(name);
                    CharacterDatabase.PExecute("UPDATE silvermoon_new_chars SET guid = '%u', account = '%u', created = UNIX_TIMESTAMP() WHERE name = '%s'", guid, accountId, name.c_str());
                    // in the random bot set for a year, so it is never rotated out
                    SetEventValue(guid, "add", 1, 365 * DAY); currentBots.push_back(guid); // the bot list is read from the database only at startup
                    sLog.outBasic("Silvermoon: created tracked character %s (guid %u, account %u)", name.c_str(), guid, accountId);
                } while (pending->NextRow());
            }
        }
    }
    silvermoon::Hubs::Load();              // Phase 2b quest hubs and visited memory, once
    silvermoon::Hubs::SaveIfDue();         // visited hubs, batched every 10 minutes
    {
        static time_t lastFast = 0;
        time_t nowFast = time(nullptr);
        static time_t ledgerStart = nowFast;
        // always-online alts: every 5 minutes an ungrouped alt with travel but no brain gets it
        // (Guile kept stock travel only although AiFactory should have given it the brain)
        static time_t lastAutoLoad = 0;
        if (nowFast - lastAutoLoad >= 10)   // panel updates every 10 s (was 30)
        {
            lastAutoLoad = nowFast;
            SilvermoonLoadAuto();
            // Always on (S: "a connection that maintains even when I'm logged out"). Every character
            // with auto on (characters.silvermoon_auto) stays in the world: when you aren't playing it -
            // logged out, disconnected, after a server restart - it is logged in as a bot with the brain
            // and keeps playing. Switching your character to Auto turns this on for it, Manual turns it
            // off. When you log back in on a character that is on Auto, Auto resumes.
            // new characters on your own accounts (not the random-bot accounts) start always on (S,
            // 2026-11-24, "for now"): checked once a minute for characters created since the start
            {
                static uint32 checkedUpTo = 0;
                static time_t lastNewCharCheck = 0;
                if (!checkedUpTo)
                {
                    if (auto r = CharacterDatabase.Query("SELECT MAX(guid) FROM characters"))
                        checkedUpTo = r->Fetch()[0].GetUInt32();
                    lastNewCharCheck = nowFast;
                }
                else if (nowFast - lastNewCharCheck >= 60)
                {
                    lastNewCharCheck = nowFast;
                    std::set<uint32> botAccounts(sPlayerbotAIConfig.randomBotAccounts.begin(), sPlayerbotAIConfig.randomBotAccounts.end());
                    if (auto r = CharacterDatabase.PQuery("SELECT guid, account, name FROM characters WHERE guid > %u ORDER BY guid", checkedUpTo))
                    {
                        do
                        {
                            Field* f = r->Fetch();
                            uint32 g = f[0].GetUInt32();
                            checkedUpTo = std::max(checkedUpTo, g);
                            if (botAccounts.count(f[1].GetUInt32()))
                                continue;
                            SilvermoonSetAuto(g, true);
                            sLog.outBasic("Silvermoon: new character %s on a player account starts always on (auto)", f[2].GetString());
                        } while (r->NextRow());
                    }
                }
            }
            static std::unordered_map<uint32, char> lastSeen;        // guid -> 'A' you, on Auto / 'M' you, Manual
            std::set<uint32> realNow;
            for (auto& i : sRandomPlayerbotMgr.GetPlayers())
                if (Player* me = i.second)
                    if (me->IsInWorld() && me->GetSession())
                    {
                        uint32 g = me->GetGUIDLow();
                        realNow.insert(g);
                        char before = lastSeen.count(g) ? lastSeen[g] : 0;
                        if (me->GetPlayerbotAI())
                        {
                            if (!SilvermoonAutoOn(g))
                                SilvermoonSetAuto(g, true);              // you switched to Auto
                            lastSeen[g] = 'A';
                        }
                        else if (SilvermoonAutoOn(g) && before != 'A' && before != 'M' && me->GetPlayerbotMgr())
                        {
                            me->GetPlayerbotMgr()->HandlePlayerbotCommand("self", me);   // just logged in on an Auto character
                            sLog.outBasic("Silvermoon: %s logged in on Auto, Auto resumed", me->GetName());
                            lastSeen[g] = 'A';
                        }
                        else
                        {
                            if (before == 'A' && SilvermoonAutoOn(g))
                                SilvermoonSetAuto(g, false);             // you switched to Manual
                            lastSeen[g] = 'M';
                        }
                    }
            for (auto it = lastSeen.begin(); it != lastSeen.end();)
                if (!realNow.count(it->first))
                    it = lastSeen.erase(it);                             // you left that character
                else
                    ++it;
            for (uint32 g : SilvermoonAutoGuids())
            {
                if (realNow.count(g))
                    continue;
                ObjectGuid guid(HIGHGUID_PLAYER, g);
                if (sObjectMgr.GetPlayer(guid, false))
                    continue;                                            // in the world (bot, or logging out)
                uint32 account = sObjectMgr.GetPlayerAccountIdByGUID(guid);
                if (!account)
                    continue;
                if (!sPlayerbotAIConfig.IsFreeAltBot(g))
                    sPlayerbotAIConfig.freeAltBots.push_back(std::make_pair(account, g));
                sRandomPlayerbotMgr.AddPlayerBot(g, account);
                sLog.outBasic("Silvermoon: character %u is on Auto and nobody is playing it, logging it in as a bot", g);
            }
            // the in-game panel: every logged-in player gets its own character's state
            for (auto& i : sRandomPlayerbotMgr.GetPlayers())
                if (Player* me = i.second)
                    if (me->IsInWorld() && me->GetSession())
                    {
                        // your character on Auto always runs the brain (S: Mommy sat on "starting up" and
                        // ground mobs with the stock logic - the brain wasn't added when Auto came on)
                        if (me->GetPlayerbotAI() && !me->GetGroup() && !me->InBattleGround() &&
                            !me->GetPlayerbotAI()->HasStrategy("silvermoon brain", BotState::BOT_STATE_NON_COMBAT))
                        {
                            me->GetPlayerbotAI()->ChangeStrategy("+travel,+rpg,+grind,+silvermoon brain", BotState::BOT_STATE_NON_COMBAT);
                            sLog.outBasic("Silvermoon: %s on Auto had no brain, added it", me->GetName());
                        }
                        if (me->GetPlayerbotAI() && me->IsAlive())
                        {
                            SilvermoonFillEmptyWeapon(me);
                            SilvermoonApplyItemRules(me);
                            SilvermoonProfBuyTools(me);
                            SilvermoonWeaponTraining(me);
                            if (me->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) && !me->IsInCombat())
                                SilvermoonBuyBag(me);
                            // upgrades already sitting in the bags (S): checked every 30 s out of combat
                            if (!me->IsInCombat())
                                me->GetPlayerbotAI()->DoSpecificAction("equip upgrades", Event("silvermoon auto"), true);
                        }
                        SilvermoonPushBrainPanel(me, me);
                        std::string pol = SilvermoonPolicyPanel(me);   // the Policies window
                        WorldPacket polPacket;
                        ChatHandler::BuildChatPacket(polPacket, CHAT_MSG_PARTY, pol.c_str(), LANG_ADDON, CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName());
                        me->GetSession()->SendPacket(polPacket);
                    }
            for (auto const& alt : sPlayerbotAIConfig.freeAltBots)
            {
                Player* p = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, alt.second));
                if (!p || !p->IsInWorld() || !p->GetPlayerbotAI())
                    continue;
                if (SilvermoonAutoOn(alt.second) && p->IsAlive() && !p->IsInCombat() && p->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) &&
                    !GetEventValue(alt.second, "silvermoon_alt_shop"))
                {
                    SetEventValue(alt.second, "silvermoon_alt_shop", 1, 10 * MINUTE);
                    uint32 before = p->GetMoney();
                    SilvermoonDropStaleQuests(p);
                    SilvermoonApplyItemRules(p);
                    SilvermoonProfBuyTools(p);
                    SilvermoonWeaponTraining(p);
                    SilvermoonWeaponUpgrade(p);
                    SilvermoonBuyBag(p);
                    SilvermoonDualWield(p);
                    if (before > p->GetMoney())
                        silvermoon::Ledger(silvermoon::LG_GEAR) += before - p->GetMoney();
                }
                Player* owner = p->GetPlayerbotAI()->GetMaster();
                if (owner && owner->GetSession() && owner->GetSession()->GetAccountId() == p->GetSession()->GetAccountId() &&
                    owner->isRealPlayer())
                    SilvermoonPushBrainPanel(p, owner);
            }
            // auto off: an alt that still runs the brain drops it (and its current goal)
            for (auto const& alt : sPlayerbotAIConfig.freeAltBots)
            {
                Player* p = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, alt.second));
                if (!p || !p->IsInWorld() || !p->GetPlayerbotAI() || SilvermoonAutoOn(alt.second))
                    continue;
                // not while you are playing it yourself (S: Mommy, logged in on Auto, lost the brain
                // every 30 s here - its alt switch was off - and went back to stock questing,
                // ignoring the profession you queued)
                if (p->GetPlayerbotAI()->IsRealPlayer())
                    continue;
                PlayerbotAI* pai = p->GetPlayerbotAI();
                if (pai->HasStrategy("silvermoon brain", BotState::BOT_STATE_NON_COMBAT))
                {
                    pai->ChangeStrategy("-silvermoon brain", BotState::BOT_STATE_NON_COMBAT);
                    if (TravelTarget* target = pai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                    sLog.outBasic("Silvermoon: auto off for %s, brain removed", p->GetName());
                }
            }
        }
        static time_t lastAltCheck = 0;
        if (sPlayerbotAIConfig.silvermoonBrain && nowFast - lastAltCheck >= 30)
        {
            lastAltCheck = nowFast;
            for (auto const& alt : sPlayerbotAIConfig.freeAltBots)
            {
                Player* p = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, alt.second));
                if (!p || !p->IsInWorld() || p->GetGroup() || p->InBattleGround() || !p->GetPlayerbotAI() || !SilvermoonAutoOn(alt.second))
                    continue;
                PlayerbotAI* pai = p->GetPlayerbotAI();
                if (!pai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT))
                    pai->ChangeStrategy("+travel,+rpg,+grind", BotState::BOT_STATE_NON_COMBAT);
                if (!pai->HasStrategy("silvermoon brain", BotState::BOT_STATE_NON_COMBAT))
                {
                    pai->ChangeStrategy("+silvermoon brain", BotState::BOT_STATE_NON_COMBAT);
                    sLog.outBasic("Silvermoon: always-online alt %s had no brain, added it", p->GetName());
                }
            }
        }
        static time_t lastClusterCheck = nowFast;
        if (nowFast - lastClusterCheck >= 10 * MINUTE)
        {
            lastClusterCheck = nowFast;
            static std::map<uint64, std::set<uint32>> previous;   // cell -> bots seen there last check
            static std::set<uint32> nudged;                       // bots whose goal was ended last check
            std::map<uint64, std::vector<Player*>> cells;
            ForEachPlayerbot([&](Player* p)
            {
                if (!p || !p->IsInWorld() || !p->IsAlive() || p->IsInCombat() || p->IsTaxiFlying() || p->IsBeingTeleported() ||
                    !IsRandomBot(p) || !p->GetMap() || !p->GetMap()->IsContinent() || p->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) ||
                    p->GetGroup() || SilvermoonDungeonOf(p->GetGUIDLow()))
                    return;
                uint64 key = (uint64(p->GetMapId()) << 48) | (uint64(int64(p->GetPositionX() / 10.0f) + 100000) << 24) | uint64(int64(p->GetPositionY() / 10.0f) + 100000);
                cells[key].push_back(p);
            });
            std::map<uint64, std::set<uint32>> current;
            std::set<uint32> nudgedNow;
            for (auto const& [key, bots] : cells)
            {
                if (bots.size() < 3)
                    continue;
                std::set<uint32> ids;
                for (Player* p : bots)
                    ids.insert(p->GetGUIDLow());
                current[key] = ids;
                auto prev = previous.find(key);
                if (prev == previous.end())
                    continue;
                std::vector<Player*> stayed;
                for (Player* p : bots)
                    if (prev->second.count(p->GetGUIDLow()))
                        stayed.push_back(p);
                if (stayed.size() < 3)
                    continue;
                sLog.outBasic("Silvermoon: stuck spot: %u bots at map %u (%.0f, %.0f) for 10+ minutes", uint32(stayed.size()),
                    stayed.front()->GetMapId(), stayed.front()->GetPositionX(), stayed.front()->GetPositionY());
                for (Player* p : stayed)
                {
                    if (nudged.count(p->GetGUIDLow()))
                    {
                        ++silvermoon::Tp(silvermoon::TP_STUCK);
                        if (!SilvermoonHubTeleport(p)) RandomTeleportForLevel(p, false);
                        continue;
                    }
                    if (TravelTarget* target = p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                    nudgedNow.insert(p->GetGUIDLow());
                }
            }
            previous = std::move(current);
            nudged = std::move(nudgedNow);
        }
        if (nowFast - ledgerStart >= HOUR)
        {
            uint64 v[silvermoon::LG_COUNT];
            for (int i = 0; i < silvermoon::LG_COUNT; ++i)
                v[i] = silvermoon::Ledger(silvermoon::LedgerKind(i)).exchange(0);
            CharacterDatabase.PExecute("INSERT INTO silvermoon_ledger (ts, seconds, gain, loss, repair, train, sell, gear, deaths, buy) VALUES (%u, %u, %llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu)",
                uint32(nowFast), uint32(nowFast - ledgerStart), (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
                (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6], (unsigned long long)v[7]);
            sLog.outBasic("Silvermoon: ledger last hour (copper): gained %llu, lost %llu, repairs %llu, training %llu, vendor sales %llu, bags and tools %llu, deaths %llu",
                (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6]);
            ledgerStart = nowFast;
        }
        if (nowFast - lastFast >= 10)
        {
            lastFast = nowFast;
            ForEachPlayerbot([&](Player* p)
            {
                if (p && p->IsInWorld() && !p->IsBeingTeleported() && p->GetPlayerbotAI() && IsRandomBot(p))
                    SilvermoonFastProcessBot(p, p->GetPlayerbotAI(), p->GetGUIDLow());
            });
        }
    }        // Phase 2a quest index, once (always loaded, used only with SilvermoonSpread = 1)

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
        static std::map<uint32, std::deque<time_t>> silvermoonPartyDeaths; // party id -> recent deaths
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
                            // a party that keeps dying (members revived one by one while the others
                            // fight on, so it never counts as a wipe) calls the run off after 15
                            // deaths in 15 minutes: Arcele died every 20-30 seconds on the Deadmines ship
                            if (uint32 partyId = GetEventValue(guid, "silvermoon_dungeon_party"))
                            {
                                std::deque<time_t>& deaths = silvermoonPartyDeaths[partyId];
                                time_t nowDeath = time(0);
                                deaths.push_back(nowDeath);
                                while (!deaths.empty() && nowDeath - deaths.front() > 15 * MINUTE)
                                    deaths.pop_front();
                                if (deaths.size() >= 15 && !silvermoonAbandoned.count(partyId))
                                {
                                    sLog.outBasic("Silvermoon: dungeon party of %s gives up on map %u after %u deaths in 15 minutes", p->GetName(), dungeonMap, uint32(deaths.size()));
                                    silvermoonAbandoned.insert(partyId);
                                    silvermoonPartyDeaths.erase(partyId);
                                }
                            }
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
                            if (wipes > 3) // was 8: a party that wipes 3 times is not going to make it
                            {
                                sLog.outBasic("Silvermoon: bot %s gives up on dungeon map %u after %u wipes", p->GetName(), dungeonMap, wipes);
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
                    pai->ChangeStrategy("-travel,-rpg,-tfish,-gather,-explore,-grind,-emote,-rpg jump,-silvermoon brain,+dungeon", BotState::BOT_STATE_NON_COMBAT);
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
                    // brain CPU time over the last 5 minutes: on the bot-update threads, and in the
                    // background destination fetches (ms, summed over all bots)
                    uint32 brainMs = uint32(silvermoon::BrainMicros().exchange(0) / 1000);
                    uint32 brainAsyncMs = uint32(silvermoon::BrainAsyncMicros().exchange(0) / 1000);
                    uint32 liftSkips = silvermoon::LiftSkips().exchange(0);
                    uint32 tp[silvermoon::TP_COUNT];
                    for (int k = 0; k < silvermoon::TP_COUNT; ++k)
                        tp[k] = silvermoon::Tp(silvermoon::TpKind(k)).exchange(0);
                    CharacterDatabase.PExecute("INSERT INTO silvermoon_perf (ts, bots, players, avg_diff, max_diff, brain_ms, brain_async_ms, tp_lift, "
                        "tp_level_zone, tp_too_dangerous, tp_enemy_zone, tp_city, tp_goldshire, tp_fallback, tp_stuck) "
                        "VALUES (UNIX_TIMESTAMP(), %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u)",
                        online, uint32(GetPlayers().size()), uint32(sWorld.GetAverageDiff()), uint32(sWorld.GetMaxDiff()), brainMs, brainAsyncMs, liftSkips,
                        tp[silvermoon::TP_LEVEL_ZONE], tp[silvermoon::TP_TOO_DANGEROUS], tp[silvermoon::TP_ENEMY_ZONE], tp[silvermoon::TP_CITY],
                        tp[silvermoon::TP_GOLDSHIRE], tp[silvermoon::TP_FALLBACK], tp[silvermoon::TP_STUCK]);
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
                // Phase 2a crowding spreads bots out instead, so the sweep is off with SilvermoonSpread = 1
                if (!sPlayerbotAIConfig.silvermoonSpread && nowTime - lastBalance >= 600)
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
                        ++silvermoon::Tp(silvermoon::TP_GOLDSHIRE);
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
                uint32 m0 = p->GetMoney();
                SilvermoonRepairNearby(p);
                uint32 m1 = p->GetMoney();
                SilvermoonDropStaleQuests(p);
                SilvermoonWeaponUpgrade(p);
                SilvermoonBuyBag(p);            // S: bags before Dual Wield and professions
                SilvermoonDualWield(p);
                SilvermoonLearnGathering(p);
                uint32 m2 = p->GetMoney();
                SilvermoonTrainNearby(p);
                uint32 m3 = p->GetMoney();
                if (m0 > m1) silvermoon::Ledger(silvermoon::LG_REPAIR) += m0 - m1;
                if (m1 > m2) silvermoon::Ledger(silvermoon::LG_GEAR) += m1 - m2;
                if (m2 > m3) silvermoon::Ledger(silvermoon::LG_TRAIN) += m2 - m3;
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
}

// Called from RandomPlayerbotMgr::ProcessBot(uint32) for a bot that is due an update.
// Silvermoon: checks that need to run often. SilvermoonProcessBot only runs when the stock
// "update" event comes round, every 1 to 25 minutes per bot, far too slow for stuck recovery
// (3 checks 30 seconds apart) - it gave up on 1 stuck bot in an hour. SilvermoonUpdate calls this
// for every bot every 10 seconds; each check keeps its own timer.
void RandomPlayerbotMgr::SilvermoonFastProcessBot(Player* player, PlayerbotAI* ai, uint32 bot)
{
    // in-memory timers: event values write to the database on every set, too much at this rate
    static std::mutex fastLock;
    static std::unordered_map<uint32, std::pair<time_t, time_t>> nextCheck;   // stuck, progress
    time_t fastNow = time(nullptr);
    bool stuckDue = false, progressDue = false;
    // gold watch: a bot whose money jumps by 5 gold or more within one 10-second check is logged
    // with what it is doing (5 Horde casters gained 50-96 gold in one hour on 2026-11-22 and the
    // cause could not be traced afterwards)
    {
        static std::mutex moneyLock;
        static std::unordered_map<uint32, uint32> lastMoney;
        uint32 money = player->GetMoney();
        uint32 before = 0;
        {
            std::lock_guard<std::mutex> guard(moneyLock);
            auto it = lastMoney.find(bot);
            before = it != lastMoney.end() ? it->second : money;
            lastMoney[bot] = money;
        }
        if (money > before)
            silvermoon::Ledger(silvermoon::LG_GAIN) += money - before;
        else if (money < before)
            silvermoon::Ledger(silvermoon::LG_LOSS) += before - money;
        if (money >= before + 50000)
        {
            TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            sLog.outError("Silvermoon: gold watch: bot %s (level %u, zone %u) went from %u to %u copper in 10 s; goal: %s",
                player->GetName(), player->GetLevel(), player->GetZoneId(), before, money,
                target && target->GetDestination() ? target->GetDestination()->GetTitle().c_str() : "none");
        }
    }
    // Phase 2b step 4: deaths, so routes avoid places a bot died twice in the last 2 hours
    {
        static std::mutex aliveLock;
        static std::unordered_map<uint32, bool> wasAlive;
        bool alive = player->IsAlive();
        bool died = false;
        {
            std::lock_guard<std::mutex> guard(aliveLock);
            auto it = wasAlive.find(bot);
            died = it != wasAlive.end() && it->second && !alive;
            wasAlive[bot] = alive;
        }
        // world PvP option A (S): was it a player? The core writes the death row at the kill;
        // read it on a later tick so the async insert has landed
        {
            static std::mutex pvpLock;
            static std::unordered_map<uint32, std::pair<time_t, uint32>> pending;   // guid -> (death time, zone)
            std::pair<time_t, uint32> check(0, 0);
            {
                std::lock_guard<std::mutex> guard(pvpLock);
                if (died && IsRandomBot(player) && !ai->HasRealPlayerMaster() && !player->InBattleGround())
                    pending[bot] = { time(nullptr), player->GetZoneId() };
                else
                {
                    auto it = pending.find(bot);
                    if (it != pending.end() && time(nullptr) - it->second.first >= 5)
                    {
                        check = it->second;
                        pending.erase(it);
                    }
                }
            }
            if (check.first)
                if (auto result = CharacterDatabase.PQuery("SELECT killer_is_player FROM silvermoon_deaths WHERE guid = %u AND ts >= %u ORDER BY id DESC LIMIT 1",
                    bot, uint32(check.first) - 5))
                    if (result->Fetch()[0].GetUInt32() && SilvermoonNotePvpDeath(bot, check.second))
                    {
                        sLog.outBasic("Silvermoon: bot %s died to players twice in zone %u within 30 minutes, avoiding it for an hour",
                            player->GetName(), check.second);
                        SilvermoonWantRelocate(bot);
                        if (TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                    }
        }
        // still standing in the zone it should avoid (graveyard and corpse runs keep it there; one bot
        // died 14 times to players in Stonetalon after the rule fired): hearthstone out when ready,
        // if home is somewhere else
        if (alive && !player->IsInCombat() && !player->IsTaxiFlying())
            if (uint32 avoided = SilvermoonAvoidedZone(bot))
                if (player->GetZoneId() == avoided)
                {
                    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
                    uint32 hmap = 0, hzone = 0, harea = 0;
                    player->GetHomebindLocation(hx, hy, hz, hmap);
                    sTerrainMgr.GetZoneAndAreaId(hzone, harea, hmap, hx, hy, hz);
                    if (hzone != avoided && ai->GetAiObjectContext()->GetValue<bool>("action useful", "hearthstone")->Get() &&
                        ai->DoSpecificAction("hearthstone", Event("silvermoon pvp avoid"), true))
                    {
                        sLog.outBasic("Silvermoon: bot %s hearthed out of zone %u (players kept killing it there)", player->GetName(), avoided);
                        return;   // casting / teleporting: nothing more this tick
                    }
                }
        if (died)
        {
            ++silvermoon::Ledger(silvermoon::LG_DEATHS);
            SilvermoonNoteDeath(bot, player->GetMapId(), player->GetPositionX(), player->GetPositionY());
            // second death here within the hour: drop the goal, the brain then avoids this spot
            if (SilvermoonDeathsNearPoint(bot, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), 250.0f, HOUR) >= 2)
                if (TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                    target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        }

        // death-loop rescue: 5 deaths in an hour at level 6+ -> a zone that fits its level
        if (alive && player->GetLevel() >= 6 && !player->GetGroup() && !ai->HasRealPlayerMaster() && !player->IsInCombat() &&
            !player->InBattleGround() && !SilvermoonDungeonOf(bot) && SilvermoonRecentDeaths(bot, HOUR) >= 5)
        {
            sLog.outBasic("Silvermoon: death-loop rescue: bot %s (level %u) died 5 times in an hour, moving it to a zone for its level",
                player->GetName(), player->GetLevel());
            SilvermoonClearDeaths(bot);
            ++silvermoon::Tp(silvermoon::TP_TOO_DANGEROUS);
            if (!SilvermoonHubTeleport(player)) RandomTeleportForLevel(player, false);
            // crash 2026-11-23 (x3): after a teleport to another map the rest of this function
            // called GetMap() on a bot that had no map yet (assert in WorldObject::GetMap)
            return;
        }

        // starter rescue: a bot under level 6 that died twice in 30 minutes far from where it
        // started (wandered into level 5-6 mobs) goes back to its starting spot, hearthstone included,
        // once it is alive again
        if (alive && player->GetLevel() < 6 && !player->GetGroup() && !ai->HasRealPlayerMaster() && !player->IsInCombat() &&
            SilvermoonRecentDeaths(bot, 30 * MINUTE) >= 2)
            if (PlayerInfo const* start = sObjectMgr.GetPlayerInfo(player->getRace(), player->getClass()))
            {
                WorldPosition startPos(start->mapId, start->positionX, start->positionY, start->positionZ);
                if (player->GetMapId() != start->mapId || startPos.distance(WorldPosition(player)) > 400.0f)
                {
                    sLog.outBasic("Silvermoon: starter rescue: bot %s (level %u) died twice far from its starting area, sending it back",
                        player->GetName(), player->GetLevel());
                    WorldLocation startLoc(start->mapId, start->positionX, start->positionY, start->positionZ, start->orientation);
                    player->SetHomebindToLocation(startLoc, start->areaId);
                    if (TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                    player->TeleportTo(start->mapId, start->positionX, start->positionY, start->positionZ, start->orientation);
                    SilvermoonClearDeaths(bot);
                    return;   // the bot may have left its map: nothing below may touch GetMap() now
                }
            }
    }
    // Phase 2b: hubs within 150 yards count as visited (cheap: ~90 distance checks), and relocation
    // trips: arrival (home bind at the innkeeper) and failed trips
    if (sPlayerbotAIConfig.silvermoonTravel && player->GetMap() && player->GetMap()->IsContinent())
    {
        if (player->IsAlive())
            silvermoon::Hubs::NoteVisits(player);
        SilvermoonRelocateTick(player, ai);   // also while dead: deaths on the way count
    }
    {
        std::lock_guard<std::mutex> guard(fastLock);
        auto& next = nextCheck[bot];
        if (fastNow >= next.first) { stuckDue = true; next.first = fastNow + 60; }   // 3 checks = 3 minutes
        if (fastNow >= next.second) { progressDue = true; next.second = fastNow + 2 * MINUTE; }
    }
        // Silvermoon brain Phase 2a: stuck recovery. Every 30 seconds while a brain bot travels to a
        // goal: if it moved less than 5 yards, (1) recompute the path, then (2) skip to the next
        // waypoint, then (3) give up: the destination is left out of its goals for an hour and the
        // give-up is logged to silvermoon_decisions with where it happened. Waiting is not stuck:
        // fighting, dead, flying, on or waiting for a transport, sitting, casting, looting or
        // talking to an NPC resets the check.
        if (ai && sPlayerbotAIConfig.silvermoonSpread && sPlayerbotAIConfig.silvermoonBrain && sRandomPlayerbotMgr.IsRandomBot(player) &&
            player->IsInWorld() && !player->GetGroup() && !ai->HasRealPlayerMaster() && !SilvermoonDungeonOf(bot) &&
            player->GetMap() && player->GetMap()->IsContinent() && stuckDue)
        {
            static std::mutex stuckLock;
            struct StuckState { WorldPosition pos; uint8 stage = 0; TravelDestination* dest = nullptr; };
            static std::unordered_map<uint32, StuckState> stuck;
            AiObjectContext* context = ai->GetAiObjectContext();
            TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
            LastMovement& lastMove = context->GetValue<LastMovement&>("last movement")->Get();
            // waiting at a dock: the next path point is a transport and the bot is at it. (The old
            // "last transport entry" stays set after any ride, which hid every stuck bot that had
            // ever taken a boat or lift.)
            bool waitingForTransport = false;
            if (!lastMove.lastPath.empty() && lastMove.lastPath.getPath().size() > 1)
            {
                PathNodePoint const& next = *std::next(lastMove.lastPath.getPath().begin());
                waitingForTransport = next.type == PathNodeType::NODE_TRANSPORT && next.point.getMapId() == player->GetMapId() &&
                    next.point.distance(WorldPosition(player)) < 40.0f;
            }
            WorldPosition here(player);
            GuidPosition rpgTarget = context->GetValue<GuidPosition>("rpg target")->Get();
            // any rpg target counts as busy (town chores: selling, training, repairing, chatting),
            // and so does resting in a town or inn area: 270 bots an hour "gave up" in Sentinel
            // Hill, Thelsamar, the Crossroads and other hubs while doing chores there
            bool talking = bool(rpgTarget) || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING);
            bool looting = context->GetValue<LootObjectStack*>("available loot")->Get()->CanLoot(INTERACTION_DISTANCE * 2);
            // resurrection sickness: the bot waits at the spirit healer until it wears off (up to 10
            // minutes) - every bot in the hub "stuck" clusters (graveyards at Sentinel Hill, the
            // Crossroads, Southshore, Lakeshire...) had it
            bool sick = ai->HasAura(SPELL_ID_PASSIVE_RESURRECTION_SICKNESS, player);
            bool waiting = !player->IsAlive() || player->IsInCombat() || player->IsTaxiFlying() || player->GetTransport() ||
                waitingForTransport || player->IsSitState() || player->IsNonMeleeSpellCasted(false) || talking || looting || sick;
            // any state but working at a destination counts: a stuck bot's goal keeps dropping into
            // cooldown or expiring and being chosen again, so it is rarely in "travel" right then
            bool travelling = target && target->GetStatus() != TravelStatus::TRAVEL_STATUS_WORK;
            uint8 stage = 0;
            TravelDestination* dest = travelling ? target->GetDestination() : nullptr;
            if (dest && typeid(*dest) == typeid(NullTravelDestination))
                dest = nullptr;
            if (!dest)
                travelling = false;     // no goal at all: idle between goals is not stuck
            // standing close to the goal is not stuck: talking to a quest giver, trainer or vendor,
            // or waiting for it, kept 300 bots an hour "stuck" in Sentinel Hill, Auberdine and the
            // Crossroads, and the give-up then hid their quest turn-ins for an hour
            if (travelling && target->GetPosition() && target->GetPosition()->getMapId() == player->GetMapId() &&
                target->GetPosition()->distance(WorldPosition(player)) < 40.0f)
                travelling = false;
            {
                std::lock_guard<std::mutex> guard(stuckLock);
                StuckState& st = stuck[bot];
                // anchored on position only: a bot that re-picks a new destination every few seconds
                // without moving (Evievera, cycling vendors) is just as stuck
                if (!travelling || waiting || st.pos.getMapId() != here.getMapId() || st.pos.distance(here) >= 5.0f)
                {
                    st.pos = here;
                    st.stage = 0;
                    st.dest = dest;
                }
                else
                    stage = ++st.stage;
                if (stage >= 3)
                    st.stage = 0;
            }
            if (stage == 1)
            {
                ai->StopMoving();
                lastMove.clear();       // the next move asks for a fresh path
            }
            else if (stage == 2)
            {
                std::vector<PathNodePoint>& points = lastMove.lastPath.getPath();
                if (points.size() > 1)
                    points.erase(points.begin());
                else
                    lastMove.clear();
            }
            else if (stage >= 3)
            {
                if (dest)
                    SilvermoonSkipDestination(bot, dest, 20 * MINUTE);   // was an hour: a wrong give-up hid quest turn-ins too long
                silvermoon::DecisionRow row;
                row.ts = uint32(time(nullptr));
                row.guid = bot;
                row.goal = "stuck give-up";
                row.target = dest ? dest->GetTitle() : "no destination";
                row.reason = "no progress for 3 minutes";
                row.map = player->GetMapId();
                row.x = player->GetPositionX();
                row.y = player->GetPositionY();
                silvermoon::DecisionLog::Add(row);
                target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                lastMove.clear();
            }
        }

        // Silvermoon patch: no-progress watchdog (stopgap until Phase 2a's stuck recovery). A bot
        // that can't path out of where it stands (Mowor, in a ravine below the Burning Blade camp)
        // keeps choosing goals it never reaches, and the skip list then walks it through every
        // vendor in the world. Every 2 minutes: a solo bot that has moved less than 5 yards in
        // 15 minutes, while not working at a destination, fishing, sitting or fighting, is sent home.
        if (ai && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsInWorld() && player->IsAlive() &&
            !player->IsTaxiFlying() && !player->IsBeingTeleported() && !player->InBattleGround() && !player->GetTransport() &&
            !player->GetGroup() && !ai->HasRealPlayerMaster() && player->GetMap() && player->GetMap()->IsContinent() &&
            progressDue)
        {
            static std::mutex progressLock;
            static std::unordered_map<uint32, std::pair<WorldPosition, time_t>> anchors;
            time_t now = time(nullptr);
            TravelTarget* travelTarget = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            bool busy = player->IsInCombat() || player->IsSitState() ||
                (travelTarget && travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK) ||
                (travelTarget && travelTarget->GetDestination() && travelTarget->GetDestination()->GetPurpose() == TravelDestinationPurpose::GatherFishing);
            bool sendHome = false;
            uint32 minutes = 0;
            {
                std::lock_guard<std::mutex> guard(progressLock);
                WorldPosition here(player);
                auto it = anchors.find(bot);
                if (busy || it == anchors.end() || it->second.first.getMapId() != here.getMapId() || it->second.first.distance(here) > 5.0f)
                    anchors[bot] = { here, now };
                else if (now - it->second.second >= 15 * MINUTE)
                {
                    minutes = uint32((now - it->second.second) / MINUTE);
                    anchors.erase(it);
                    sendHome = true;
                }
            }
            if (sendHome)
            {
                sLog.outBasic("Silvermoon: bot %s (level %u) has not moved for %u minutes, sending it home", player->GetName(), player->GetLevel(), minutes);
                if (travelTarget)
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                ++silvermoon::Tp(silvermoon::TP_STUCK);
                player->TeleportToHomebind();
            }
        }

}

void RandomPlayerbotMgr::SilvermoonProcessBot(Player* player, PlayerbotAI* ai, uint32 bot)
{
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
                    ++silvermoon::Tp(silvermoon::TP_ENEMY_ZONE);
                    player->TeleportTo(start->mapId, start->positionX, start->positionY, start->positionZ, start->orientation);
                }
                else
                {
                    sLog.outBasic("Silvermoon: bot %s (level %u) was in enemy zone %u area %u, sending it home",
                        player->GetName(), player->GetLevel(), zoneId, areaId);
                    ++silvermoon::Tp(silvermoon::TP_ENEMY_ZONE);
                    player->TeleportToHomebind();
                }
            }
        }

        // Silvermoon patch: city watchdog. Bots that log in or hearth into a capital often
        // cannot path out (Thunder Bluff lifts, a goal on the other continent) and re-pick
        // goals in place for an hour or more; the level-zone check below only catches them
        // after two 45-90 minute checks. Every 2 minutes: a solo bot that has been in a
        // capital for 10+ minutes is sent to a level-appropriate zone.
        if (ai && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsInWorld() && player->IsAlive() &&
            !player->IsTaxiFlying() && !player->IsBeingTeleported() && !player->InBattleGround() &&
            !player->InBattleGroundQueue() && !player->IsInCombat() && !player->GetGroup() &&
            !ai->HasRealPlayerMaster() && player->GetLevel() >= 10 &&
            player->GetMap() && player->GetMap()->IsContinent() &&
            !GetEventValue(bot, "silvermoon_city_check"))
        {
            SetEventValue(bot, "silvermoon_city_check", 1, 2 * MINUTE);
            if (ai->IsInCapitalCity())
            {
                uint32 since = GetEventValue(bot, "silvermoon_city_since");
                uint32 now = uint32(time(nullptr));
                if (!since)
                    SetEventValue(bot, "silvermoon_city_since", now, 2 * HOUR);
                else if (now - since >= 10 * MINUTE)
                {
                    sLog.outBasic("Silvermoon: bot %s (level %u) stuck in a city for %u minutes, sending it to a level-appropriate zone",
                        player->GetName(), player->GetLevel(), (now - since) / MINUTE);
                    TravelTarget* travelTarget = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                    if (travelTarget)
                        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                    ++silvermoon::Tp(silvermoon::TP_CITY);
                    if (!SilvermoonHubTeleport(player)) RandomTeleportForLevel(player, false);
                    SetEventValue(bot, "silvermoon_city_since", 0, 0);
                    SetEventValue(bot, "silvermoon_bind_home", 1, 10 * MINUTE);
                }
            }
            else if (GetEventValue(bot, "silvermoon_city_since"))
                SetEventValue(bot, "silvermoon_city_since", 0, 0);
        }

        // (stuck recovery and the no-progress watchdog run from SilvermoonFastProcessBot every 10 s)

        // Silvermoon patch: a random bot keeps a real player as its master after leaving that
        // player's group, and with it the player-bot strategy set ("chat" only for Velyna), so it
        // stands idle for days. Out of the group: no master, default random-bot strategies.
        if (ai && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsInWorld() && ai->HasRealPlayerMaster() &&
            !SilvermoonDungeonOf(bot))
        {
            Player* master = ai->GetMaster();
            if (master && (!player->GetGroup() || !player->GetGroup()->IsMember(master->GetObjectGuid())))
            {
                sLog.outBasic("Silvermoon: bot %s left %s's group but kept them as master, freeing it", player->GetName(), master->GetName());
                ai->SetMaster(nullptr);
                ai->ResetStrategies(false);
            }
        }

        // Silvermoon patch: brain on saved strategies. A bot whose strategy lists were saved before
        // Phase 1 (ai_playerbot_db_store "nc", e.g. after a player gave it commands) loads that
        // list at login, which has no "silvermoon brain" (Reneliora, Nynitiz, Issanne, Zannor).
        // Every 5 minutes: a solo random bot that should have the brain gets it back.
        if (ai && sPlayerbotAIConfig.silvermoonBrain && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsInWorld() &&
            !ai->HasRealPlayerMaster() && !SilvermoonDungeonOf(bot) && !GetEventValue(bot, "silvermoon_brain_check"))
        {
            SetEventValue(bot, "silvermoon_brain_check", 1, 5 * MINUTE);
            if (ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) && !ai->HasStrategy("silvermoon brain", BotState::BOT_STATE_NON_COMBAT))
            {
                ai->ChangeStrategy("+silvermoon brain", BotState::BOT_STATE_NON_COMBAT);
                sLog.outBasic("Silvermoon: bot %s had no brain (saved strategies), added it", player->GetName());
            }
            // a solo bot left with almost no strategies (Velyna: non-combat "chat" only, idle for days,
            // likely after a group with a player ended): back to the defaults, brain included
            else if (!player->GetGroup() && !ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) &&
                     !ai->HasStrategy("grind", BotState::BOT_STATE_NON_COMBAT) && !ai->HasStrategy("dungeon", BotState::BOT_STATE_NON_COMBAT))
            {
                ai->ResetStrategies(false);
                sLog.outBasic("Silvermoon: bot %s had no travel or grind strategy, reset it to the defaults", player->GetName());
            }
        }

        // Silvermoon patch: ghost watchdog. Some bots never make it back to their corpse
        // (Nynitiz and Beardo were ghosts for 40-55 hours). Every 2 minutes: a solo bot that
        // has been dead for 15+ minutes is revived where it stands.
        if (ai && sRandomPlayerbotMgr.IsRandomBot(player) && player->IsInWorld() && !player->IsAlive() &&
            !player->IsBeingTeleported() && !player->InBattleGround() && !ai->HasRealPlayerMaster() &&
            !SilvermoonDungeonOf(bot) && !GetEventValue(bot, "silvermoon_dead_check"))
        {
            SetEventValue(bot, "silvermoon_dead_check", 1, 2 * MINUTE);
            uint32 since = GetEventValue(bot, "silvermoon_dead_since");
            uint32 now = uint32(time(nullptr));
            if (!since)
                SetEventValue(bot, "silvermoon_dead_since", now, 2 * HOUR);
            else if (now - since >= 15 * MINUTE)
            {
                sLog.outBasic("Silvermoon: bot %s (level %u) dead for %u minutes, reviving it",
                    player->GetName(), player->GetLevel(), (now - since) / MINUTE);
                player->ResurrectPlayer(0.5f);
                player->SpawnCorpseBones();
                TravelTarget* travelTarget = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                if (travelTarget)
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                SetEventValue(bot, "silvermoon_dead_since", 0, 0);
            }
        }
        else if (ai && player->IsAlive() && GetEventValue(bot, "silvermoon_dead_since"))
            SetEventValue(bot, "silvermoon_dead_since", 0, 0);

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
            // Phase 2b: an outleveled bot travels to a quest hub itself (Relocate goal); the teleport
            // is only the fallback after 3 failed trips
            bool fallback = false;
            if (outleveled && !lingering && sPlayerbotAIConfig.silvermoonTravel && !player->GetGroup())
            {
                if (SilvermoonRelocateFailures(bot) < 3)
                {
                    SilvermoonWantRelocate(bot);
                    outleveled = false;
                }
                else
                {
                    fallback = true;
                    SilvermoonResetRelocateFailures(bot);
                    ++silvermoon::Tp(silvermoon::TP_FALLBACK);
                    sLog.outBasic("Silvermoon: bot %s failed 3 trips to a quest hub, fallback teleport", player->GetName());
                }
            }
            if (lingering || outleveled)
            {
                sLog.outBasic("Silvermoon: bot %s (level %u%s) outleveled area %u (level %d), teleporting it to a level-appropriate zone",
                    player->GetName(), player->GetLevel(), frontRunner ? ", front-runner" : "", areaId, areaLevel);
                TravelTarget* travelTarget = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                if (travelTarget)
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                if (!fallback)
                    ++silvermoon::Tp(silvermoon::TP_LEVEL_ZONE);
                if (!SilvermoonHubTeleport(player)) RandomTeleportForLevel(player, false);
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
                ++silvermoon::Tp(silvermoon::TP_TOO_DANGEROUS);
                if (!SilvermoonHubTeleport(player)) RandomTeleportForLevel(player, false);
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

}

// Called from RandomPlayerbotMgr::HandlePlayerbotConsoleCommand: true when it was a Silvermoon command.
bool RandomPlayerbotMgr::SilvermoonConsoleCommand(ChatHandler* handler, char const* args)
{
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
        // no priest, shaman or druid online at this level: a paladin heals instead (a party of two
        // paladins went in with neither one healing)
        auto isPaladin = [](Player* p) { return p->getClass() == CLASS_PALADIN; };
        if (party.size() == before)
            take([&](Player* p) { return isPaladin(p) && p->GetLevel() <= tankLevel; });
        if (party.size() == before)
            take(isPaladin);
        Player* healer = party.size() > before ? party.back() : nullptr;
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
            // roles: 1 = tank (the first pick, a warrior/paladin/druid), 2 = healer (the healer pick:
            // a priest/shaman/druid, or a paladin when none is online), 3 = damage
            uint32 role = 3;
            uint8 cls = p->getClass();
            if (p == party.front() && (cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID))
                role = 1;
            else if (p == healer)
                role = 2;
            sRandomPlayerbotMgr.SetEventValue(p->GetGUIDLow(), "silvermoon_dungeon_role", role, 150 * MINUTE);
            SilvermoonStockUp(p);
        }
        sLog.outBasic("Silvermoon: dungeon party for map %u: %s", d.map, names.str().c_str());
        handler->PSendSysMessage("Sent into the dungeon: %s", names.str().c_str());
        return true;
    }

    return false;
}
