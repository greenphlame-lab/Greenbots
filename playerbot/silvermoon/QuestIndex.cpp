#include "playerbot/playerbot.h"
#include "QuestIndex.h"
#include "Database/DatabaseEnv.h"
#include <atomic>
#include <chrono>

namespace silvermoon
{
    namespace
    {
        std::unordered_map<uint32_t, QuestIndexEntry> index;
        std::atomic<bool> loaded{ false };
    }

    void QuestIndex::Load()
    {
        if (loaded)
            return;
        auto start = std::chrono::steady_clock::now();
        uint32_t rows = 0;
        if (auto result = WorldDatabase.Query(
                "SELECT quest, objective, map, x, y, z, radius, min_mob_level, max_mob_level, elite_count, spawn_count, "
                "drop_chance, flags, chain_xp, source FROM silvermoon_quest_index"))
        {
            do
            {
                Field* f = result->Fetch();
                QuestIndexEntry& entry = index[f[0].GetUInt32()];
                entry.flags |= uint8_t(f[12].GetUInt32());
                entry.chainXp = f[13].GetUInt32();
                if (f[2].GetUInt32() == 0 && f[3].GetFloat() == 0.0f && f[4].GetFloat() == 0.0f)
                    continue;   // flag-only row (no spawn, or an exploration trigger)
                QuestArea area;
                area.objective = uint8_t(f[1].GetUInt32());
                area.map = uint16_t(f[2].GetUInt32());
                area.x = f[3].GetFloat();
                area.y = f[4].GetFloat();
                area.z = f[5].GetFloat();
                area.radius = f[6].GetFloat();
                area.minMobLevel = uint8_t(f[7].GetUInt32());
                area.maxMobLevel = uint8_t(f[8].GetUInt32());
                area.elites = uint16_t(f[9].GetUInt32());
                area.spawns = uint16_t(f[10].GetUInt32());
                area.dropChance = f[11].GetFloat();
                entry.areas.push_back(area);
                ++rows;
            } while (result->NextRow());
        }
        loaded = true;
        uint32_t ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        sLog.outBasic("Silvermoon: quest index loaded, %u quests, %u areas, %u ms", uint32_t(index.size()), rows, ms);
    }

    bool QuestIndex::Loaded() { return loaded; }

    QuestIndexEntry const* QuestIndex::Get(uint32_t quest)
    {
        if (!loaded)
            return nullptr;
        auto it = index.find(quest);
        return it == index.end() ? nullptr : &it->second;
    }

    QuestArea const* QuestIndex::NearestArea(uint32_t quest, uint8_t objective, uint32_t map, float x, float y)
    {
        QuestIndexEntry const* entry = Get(quest);
        if (!entry)
            return nullptr;
        QuestArea const* best = nullptr;
        float bestDist = 0;
        for (QuestArea const& area : entry->areas)
        {
            if (area.objective != objective || area.map != map)
                continue;
            float d = (area.x - x) * (area.x - x) + (area.y - y) * (area.y - y);
            if (!best || d < bestDist)
            {
                best = &area;
                bestDist = d;
            }
        }
        return best;
    }
}
