// Game-specific runtime setup for The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

#include <iostream>

namespace
{
    constexpr uint32_t kTtaNtscEntry = 0x00110008u;

    void applyTta(PS2Runtime &runtime)
    {
        (void)runtime;
        std::cout << "[tta] The Third Age (NTSC-U SLUS-21027) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("The Lord of the Rings - The Third Age (USA)", "SLUS_210.27", kTtaNtscEntry, 0u, &applyTta)
