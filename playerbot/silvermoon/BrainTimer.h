#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

// Silvermoon brain: how much CPU time the brain costs. Every brain action (request, choose,
// re-check goal) adds its wall time on the world/bot-update threads to BrainMicros(); the
// background destination fetches add theirs to BrainAsyncMicros(). The perf row takes and
// resets both every 5 minutes (silvermoon_perf.brain_ms / brain_async_ms).
namespace silvermoon
{
    inline std::atomic<uint64_t>& BrainMicros() { static std::atomic<uint64_t> v{ 0 }; return v; }
    inline std::atomic<uint64_t>& BrainAsyncMicros() { static std::atomic<uint64_t> v{ 0 }; return v; }

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
