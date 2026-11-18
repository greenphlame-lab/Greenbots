#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Silvermoon brain: buffered writes to characters.silvermoon_decisions. Bots add rows from any
// thread; the world thread flushes them as one multi-row INSERT every 30 seconds.
namespace silvermoon
{
    struct DecisionRow
    {
        uint32_t ts = 0;
        uint32_t guid = 0;
        std::string goal;          // purpose, e.g. "quest objective", "grind", "repair"
        std::string target;        // destination title
        float score = 0.0f;
        std::string runnerUp1;
        float runnerUp1Score = 0.0f;
        std::string runnerUp2;
        float runnerUp2Score = 0.0f;
        std::string reason;        // biggest score term, e.g. "quest xp", "close by", "needs repair"
        uint32_t map = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    class DecisionLog
    {
    public:
        static void Add(DecisionRow row);
        static void FlushIfDue();  // world thread
        static void Flush();
    };
}
