#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <unordered_map>

// Silvermoon brain: how much CPU time the brain costs. Every brain action (request, choose,
// re-check goal) adds its wall time on the world/bot-update threads to BrainMicros(); the
// background destination fetches add theirs to BrainAsyncMicros(). The perf row takes and
// resets both every 5 minutes (silvermoon_perf.brain_ms / brain_async_ms).
namespace silvermoon
{
    inline std::atomic<uint64_t>& BrainMicros() { static std::atomic<uint64_t> v{ 0 }; return v; }
    inline std::atomic<uint64_t>& BrainAsyncMicros() { static std::atomic<uint64_t> v{ 0 }; return v; }

    // lift rides skipped dock to dock (Thunder Bluff, Undercity), for silvermoon_perf.tp_lift
    inline std::atomic<uint32_t>& LiftSkips() { static std::atomic<uint32_t> v{ 0 }; return v; }

    // corrective teleports per 5 minutes, for silvermoon_perf.tp_* (Phase 2a)
    enum TpKind { TP_LEVEL_ZONE, TP_TOO_DANGEROUS, TP_ENEMY_ZONE, TP_CITY, TP_GOLDSHIRE, TP_FALLBACK, TP_STUCK, TP_COUNT };
    inline std::atomic<uint32_t>& Tp(TpKind kind) { static std::atomic<uint32_t> v[TP_COUNT]{}; return v[kind]; }

    // money ledger (random bots, copper): where bot money comes from and goes, written hourly to
    // characters.silvermoon_ledger. GAIN/LOSS are every bot's money changes between 10 s checks;
    // the rest are the parts Silvermoon code can see.
    enum LedgerKind { LG_GAIN, LG_LOSS, LG_REPAIR, LG_TRAIN, LG_SELL, LG_GEAR, LG_DEATHS, LG_BUY, LG_COUNT };
    inline std::atomic<uint64_t>& Ledger(LedgerKind kind) { static std::atomic<uint64_t> v[LG_COUNT]{}; return v[kind]; }

    // repair visits (S: no vendor-hopping): after a visit that left gear damaged (not enough
    // money), no new repair trip for 30 minutes unless the bot has 5 silver more than it left with
    struct RepairVisit { time_t at; uint32_t money; };
    inline std::mutex& RepairVisitLock() { static std::mutex m; return m; }
    inline std::unordered_map<uint32_t, RepairVisit>& RepairVisits() { static std::unordered_map<uint32_t, RepairVisit> v; return v; }
    inline void NoteRepairVisit(uint32_t guid, uint32_t moneyAfter)
    {
        std::lock_guard<std::mutex> guard(RepairVisitLock());
        RepairVisits()[guid] = { time(nullptr), moneyAfter };
    }
    inline bool RepairOnCooldown(uint32_t guid, uint32_t money)
    {
        std::lock_guard<std::mutex> guard(RepairVisitLock());
        auto it = RepairVisits().find(guid);
        return it != RepairVisits().end() && time(nullptr) - it->second.at < 30 * 60 && money < it->second.money + 500;
    }

    class BrainTimer
    {
    public:
        explicit BrainTimer(std::atomic<uint64_t>& sink) : sink(sink), start(std::chrono::steady_clock::now()) {}
        ~BrainTimer()
        {
            sink += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        }
    private:
        std::atomic<uint64_t>& sink;
        std::chrono::steady_clock::time_point start;
    };
}
