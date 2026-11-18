#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

// Silvermoon brain Phase 2a: the quest index (mangos.silvermoon_quest_index, built by
// tools/silvermoon/build_quest_index.py). Loaded once at startup, read-only afterwards, so any
// thread can read it without locks once Load() has run.
namespace silvermoon
{
    enum QuestIndexFlags : uint8_t
    {
        QI_EVENT = 1,       // scripted event or escort
        QI_TIMED = 2,
        QI_GROUP = 4,       // group quest
        QI_NO_SOURCE = 8,   // an objective with no known spawn
        QI_DUNGEON = 16,    // an objective only found in a dungeon
        QI_AVOID = QI_EVENT | QI_TIMED | QI_GROUP,
    };

    struct QuestArea
    {
        uint8_t objective = 0;      // 1-4 (0 = exploration trigger)
        uint16_t map = 0;
        float x = 0, y = 0, z = 0, radius = 0;
        uint8_t minMobLevel = 0, maxMobLevel = 0;
        uint16_t elites = 0, spawns = 0;
        float dropChance = 0;
    };

    struct QuestIndexEntry
    {
        uint8_t flags = 0;
        uint32_t chainXp = 0;
        std::vector<QuestArea> areas;
    };

    class QuestIndex
    {
    public:
        static void Load();                                   // world thread, at startup
        static QuestIndexEntry const* Get(uint32_t quest);    // nullptr when the quest isn't indexed
        // the objective's area nearest to (map, x, y), or nullptr
        static QuestArea const* NearestArea(uint32_t quest, uint8_t objective, uint32_t map, float x, float y);
        static bool Loaded();
    };
}
