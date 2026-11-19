#include "playerbot/playerbot.h"
#include "Hubs.h"
#include "Database/DatabaseEnv.h"
#include <atomic>
#include <cstring>
#include <sstream>
#include <mutex>
#include <set>
#include <unordered_map>

namespace silvermoon
{
    namespace
    {
        std::vector<Hub> hubs;
        std::atomic<bool> loaded{ false };
        std::mutex memoryLock;
        std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> visited;   // guid -> bitset (ids 1-128)
        std::set<uint32_t> dirty;
        time_t lastSave = 0;

        bool Bit(std::pair<uint64_t, uint64_t> const& bits, uint16_t id)
        {
            if (id == 0 || id > 128)
                return false;
            uint16_t i = id - 1;
            return i < 64 ? (bits.first >> i) & 1 : (bits.second >> (i - 64)) & 1;
        }

        void SetBit(std::pair<uint64_t, uint64_t>& bits, uint16_t id)
        {
            if (id == 0 || id > 128)
                return;
            uint16_t i = id - 1;
            if (i < 64)
                bits.first |= uint64_t(1) << i;
            else
                bits.second |= uint64_t(1) << (i - 64);
        }
    }

    void Hubs::Load()
    {
        if (loaded)
            return;
        if (auto result = WorldDatabase.Query("SELECT id, side, name, map, x, y, z, min_level, max_level, quests, innkeeper, inn_x, inn_y, inn_z, flightmaster FROM silvermoon_hubs"))
        {
            do
            {
                Field* f = result->Fetch();
                Hub h;
                h.id = uint16_t(f[0].GetUInt32());
                std::string side = f[1].GetCppString();
                h.side = side == "A" ? 1 : side == "H" ? 2 : 3;
                std::strncpy(h.name, f[2].GetCppString().c_str(), sizeof(h.name) - 1);
                h.map = uint16_t(f[3].GetUInt32());
                h.x = f[4].GetFloat(); h.y = f[5].GetFloat(); h.z = f[6].GetFloat();
                h.minLevel = uint8_t(f[7].GetUInt32()); h.maxLevel = uint8_t(f[8].GetUInt32());
                h.quests = uint16_t(f[9].GetUInt32());
                h.innkeeper = f[10].GetUInt32();
                h.innX = f[11].GetFloat(); h.innY = f[12].GetFloat(); h.innZ = f[13].GetFloat();
                h.flightMaster = f[14].GetUInt32();
                hubs.push_back(h);
            } while (result->NextRow());
        }
        if (auto result = CharacterDatabase.Query("SELECT guid, visited_lo, visited_hi FROM silvermoon_bot_memory"))
        {
            std::lock_guard<std::mutex> guard(memoryLock);
            do
            {
                Field* f = result->Fetch();
                visited[f[0].GetUInt32()] = { f[1].GetUInt64(), f[2].GetUInt64() };
            } while (result->NextRow());
        }
        lastSave = time(nullptr);
        loaded = true;
        sLog.outBasic("Silvermoon: %u quest hubs loaded, %u bots remember visits", uint32_t(hubs.size()), uint32_t(visited.size()));
    }

    std::vector<Hub> const& Hubs::All() { return hubs; }

    Hub const* Hubs::Get(uint16_t id)
    {
        for (Hub const& h : hubs)
            if (h.id == id)
                return &h;
        return nullptr;
    }

    bool Hubs::Knows(uint32_t guid, Hub const& hub, bool alliance)
    {
        if (hub.side != 3 && hub.side != (alliance ? 1 : 2))
            return false;
        uint32_t share = 70 + guid % 21;
        uint32_t h = (guid * 2654435761u) ^ (uint32_t(hub.id) * 40503u);
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return h % 100 < share;
    }

    bool Hubs::Visited(uint32_t guid, uint16_t hubId)
    {
        std::lock_guard<std::mutex> guard(memoryLock);
        auto it = visited.find(guid);
        return it != visited.end() && Bit(it->second, hubId);
    }

    void Hubs::NoteVisits(Player* bot)
    {
        if (!loaded || !bot)
            return;
        uint32_t guid = bot->GetGUIDLow();
        for (Hub const& h : hubs)
        {
            if (h.map != bot->GetMapId())
                continue;
            float dx = h.x - bot->GetPositionX(), dy = h.y - bot->GetPositionY();
            if (dx * dx + dy * dy > 150.0f * 150.0f)
                continue;
            std::lock_guard<std::mutex> guard(memoryLock);
            auto& bits = visited[guid];
            if (!Bit(bits, h.id))
            {
                SetBit(bits, h.id);
                dirty.insert(guid);
            }
        }
    }

    void Hubs::SaveIfDue()
    {
        time_t now = time(nullptr);
        if (!loaded || now - lastSave < 10 * MINUTE)
            return;
        lastSave = now;
        std::lock_guard<std::mutex> guard(memoryLock);
        if (dirty.empty())
            return;
        std::ostringstream sql;
        sql << "REPLACE INTO silvermoon_bot_memory (guid, visited_lo, visited_hi) VALUES ";
        bool first = true;
        for (uint32_t guid : dirty)
        {
            auto const& bits = visited[guid];
            sql << (first ? "" : ",") << "(" << guid << "," << bits.first << "," << bits.second << ")";
            first = false;
        }
        dirty.clear();
        CharacterDatabase.Execute(sql.str().c_str());
    }

    Hub const* Hubs::Nearest(uint32_t map, float x, float y, float maxDistance)
    {
        Hub const* best = nullptr;
        float bestD = maxDistance * maxDistance;
        for (Hub const& h : hubs)
        {
            if (h.map != map)
                continue;
            float d = (h.x - x) * (h.x - x) + (h.y - y) * (h.y - y);
            if (d <= bestD)
            {
                best = &h;
                bestD = d;
            }
        }
        return best;
    }
}
