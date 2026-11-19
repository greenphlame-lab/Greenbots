#pragma once
#include <cstdint>
#include <vector>

class Player;

// Silvermoon brain Phase 2b: quest hubs (mangos.silvermoon_hubs, built by tools/silvermoon/build_hubs.py
// from docs/phase2-hubs-draft.md). Each bot knows a seeded 70-90% of its faction's hubs and
// remembers the ones it has visited (characters.silvermoon_bot_memory).
namespace silvermoon
{
    struct Hub
    {
        uint16_t id = 0;
        uint8_t side = 0;           // 1 Alliance, 2 Horde, 3 both
        char name[64] = {};
        uint16_t map = 0;
        float x = 0, y = 0, z = 0;
        uint8_t minLevel = 0, maxLevel = 0;
        uint16_t quests = 0;
        uint32_t innkeeper = 0;     // creature guid, 0 = none
        float innX = 0, innY = 0, innZ = 0;
        uint32_t flightMaster = 0;
    };

    class Hubs
    {
    public:
        static void Load();                                    // world thread, at startup
        static std::vector<Hub> const& All();
        static Hub const* Get(uint16_t id);
        // a seeded 70-90% share of the bot's faction hubs (stable, no storage)
        static bool Knows(uint32_t guid, Hub const& hub, bool alliance);
        static bool Visited(uint32_t guid, uint16_t hubId);
        // marks hubs within 150 yards of the bot visited (world thread, from the fast loop)
        static void NoteVisits(Player* bot);
        static void SaveIfDue();                               // world thread, batched every 10 minutes
        static Hub const* Nearest(uint32_t map, float x, float y, float maxDistance);
    };
}
