#pragma once
#include <cstdint>

// Silvermoon brain Phase 2a: how many bots are heading for or working at each spot. The world
// thread recounts every 60 seconds from the bots' travel targets (cells of 100 x 100 yards); the
// scorer on the bot threads reads the counts.
namespace silvermoon
{
    class Crowd
    {
    public:
        static void Recount();                              // world thread, rate-limited inside
        static uint32_t At(uint32_t map, float x, float y);  // bots counted in that cell
    };
}
