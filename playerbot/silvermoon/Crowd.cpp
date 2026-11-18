#include "playerbot/playerbot.h"
#include "Crowd.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/TravelMgr.h"
#include <mutex>
#include <unordered_map>

namespace silvermoon
{
    namespace
    {
        const float CELL = 100.0f;
        std::mutex lock;
        std::unordered_map<uint64_t, uint32_t> counts;
        time_t lastCount = 0;

        uint64_t Key(uint32_t map, float x, float y)
        {
            uint64_t cx = uint64_t(int64_t(x / CELL) + 100000) & 0xFFFFF;
            uint64_t cy = uint64_t(int64_t(y / CELL) + 100000) & 0xFFFFF;
            return (uint64_t(map) << 40) | (cx << 20) | cy;
        }
    }

    void Crowd::Recount()
    {
        time_t now = time(nullptr);
        if (now - lastCount < 60)
            return;
        lastCount = now;
        std::unordered_map<uint64_t, uint32_t> fresh;
        sRandomPlayerbotMgr.ForEachPlayerbot([&](Player* p)
        {
            if (!p || !p->IsInWorld() || !p->GetPlayerbotAI())
                return;
            TravelTarget* t = p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (!t || !t->GetPosition() || !t->GetDestination())
                return;
            if (t->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL && t->GetStatus() != TravelStatus::TRAVEL_STATUS_WORK)
                return;
            WorldPosition* pos = t->GetPosition();
            ++fresh[Key(pos->getMapId(), pos->getX(), pos->getY())];
        });
        std::lock_guard<std::mutex> guard(lock);
        counts.swap(fresh);
    }

    uint32_t Crowd::At(uint32_t map, float x, float y)
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = counts.find(Key(map, x, y));
        return it == counts.end() ? 0 : it->second;
    }
}
