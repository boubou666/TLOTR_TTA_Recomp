// Game-specific runtime setup for The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

#include <cstdlib>
#include <iostream>

namespace
{
    constexpr uint32_t kTtaNtscEntry = 0x00110008u;

    uint32_t rd32(const uint8_t *rdram, uint32_t addr)
    {
        return *reinterpret_cast<const uint32_t *>(rdram + (addr & 0x1FFFFFCu));
    }

    // TTA_TRACE_ALLOC=1 (bring-up aid). The global allocator (object pointer at 0x378888, set once by 0x26BA50 to
    // operator new's result) is called through vtable slot 0x1C by 0x26CB80; operator new takes from EA's heap
    // (0x3A9790, allocator 0x2500F8), which 0x1B8478 carves out of newlib malloc (0x31F5D0 -> sbrk 0x328C30).
    constexpr uint32_t kAllocCall = 0x0026CB80u;
    constexpr uint32_t kAllocGlobal = 0x00378888u;
    constexpr uint32_t kEaHeapSetup = 0x001B8478u;
    constexpr uint32_t kEaHeapState = 0x003A9780u; // [0] malloc'd base, [0xC] size
    constexpr uint32_t kEaHeapAlloc = 0x002500F8u;
    constexpr uint32_t kMalloc = 0x0031F5D0u;
    constexpr uint32_t kSbrk = 0x00328C30u;
    constexpr uint32_t kSbrkBreak = 0x0037D52Cu;

    // Log the object, its vtable and the slot whenever one of them changes. A NULL object reads guest address 0,
    // like the game does.
    void allocCallTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0, s_object = ~0u, s_vtable = ~0u, s_slot = ~0u;
        const uint32_t object = rd32(rdram, kAllocGlobal);
        const uint32_t vtable = rd32(rdram, object);
        const uint32_t slot = rd32(rdram, vtable + 0x1Cu);
        const uint32_t n = s_calls++;
        if (object != s_object || vtable != s_vtable || slot != s_slot)
        {
            std::cerr << std::hex << "[tta:alloc] #" << std::dec << n << std::hex << " object=0x" << object
                      << " [0x" << vtable << " 0x" << rd32(rdram, object + 4u) << "] slot1c=0x" << slot
                      << " size=0x" << getRegU32(ctx, 5) << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
            s_object = object;
            s_vtable = vtable;
            s_slot = slot;
        }
        FUN_0026cb80_0x26cb80(rdram, ctx, runtime);
    }

    void dumpWords(const uint8_t *rdram, const char *label, uint32_t addr, uint32_t bytes)
    {
        std::cerr << std::hex << "[tta:heap] " << label << " @0x" << addr << ":";
        for (uint32_t offset = 0; offset < bytes; offset += 4u)
            std::cerr << ((offset % 32u) ? " " : "\n    ") << "0x" << rd32(rdram, addr + offset);
        std::cerr << std::dec << std::endl;
    }

    void eaHeapSetupTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::cerr << std::hex << "[tta:heap] setup: break=0x" << rd32(rdram, kSbrkBreak) << " EndOfHeap=0x"
                  << runtime->guestHeapLimit() << std::dec << std::endl;
        FUN_001b8478_0x1b8478(rdram, ctx, runtime);
        const uint32_t base = rd32(rdram, kEaHeapState);
        std::cerr << std::hex << "[tta:heap] setup done: base=0x" << base << " size=0x"
                  << rd32(rdram, kEaHeapState + 0xCu) << " break=0x" << rd32(rdram, kSbrkBreak) << std::dec << std::endl;
        // dlmalloc 2.6 arena: flags and fastbins, the first bins (bin 0's fd/bk is the top chunk), the tail fields.
        dumpWords(rdram, "arena head", kEaHeapState + 0x10u, 0x50u);
        dumpWords(rdram, "arena tail", kEaHeapState + 0x10u + 0x430u, 0x78u);
        dumpWords(rdram, "region start", base, 0x20u);
        const uint32_t top = rd32(rdram, kEaHeapState + 0x10u + 0x440u);
        dumpWords(rdram, "top chunk", top, 0x20u);
    }

    // TTA_MEMTOP=0x...: the memory top EA's heap sizing reads (0x1BD698 fills [0x20] = top - end of BSS and
    // [0x24] = top, from 0x377DD0 = 0x2000000). The runtime keeps RAM above EndOfHeap (0x1F00000) for itself, so
    // the game's first malloc of the whole span fails and the heap ends up 1 MB short. A/B switch for that.
    constexpr uint32_t kMemoryInfo = 0x001BD698u;
    constexpr uint32_t kEndOfBss = 0x00448E10u;
    uint32_t g_memoryTop = 0u;

    void memoryInfoOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t info = getRegU32(ctx, 4);
        FUN_001bd698_0x1bd698(rdram, ctx, runtime);
        auto *words = reinterpret_cast<uint32_t *>(rdram + (info & 0x1FFFFFCu));
        words[0x20 / 4] = g_memoryTop - kEndOfBss;
        words[0x24 / 4] = g_memoryTop;
    }

    void eaHeapAllocTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0, s_failures = 0;
        const uint32_t heap = getRegU32(ctx, 4), size = getRegU32(ctx, 5), align = getRegU32(ctx, 6);
        const uint32_t ra = getRegU32(ctx, 31), n = s_calls++;
        FUN_002500f8_0x2500f8(rdram, ctx, runtime);
        const uint32_t result = getRegU32(ctx, 2);
        if (n < 8u || (result == 0u && s_failures++ < 16u))
            std::cerr << std::hex << "[tta:heap] alloc #" << std::dec << n << std::hex << " heap=0x" << heap
                      << " size=0x" << size << " align=0x" << align << " ra=0x" << ra << " -> 0x" << result
                      << std::dec << std::endl;
    }

    void mallocTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logged = 0;
        const uint32_t size = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        FUN_0031f5d0_0x31f5d0(rdram, ctx, runtime);
        const uint32_t result = getRegU32(ctx, 2);
        if ((size >= 0x100000u || result == 0u) && s_logged++ < 16u)
            std::cerr << std::hex << "[tta:heap] malloc size=0x" << size << " ra=0x" << ra << " -> 0x" << result
                      << std::dec << std::endl;
    }

    void sbrkTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logged = 0;
        const uint32_t increment = getRegU32(ctx, 4), before = rd32(rdram, kSbrkBreak);
        FUN_00328c30_0x328c30(rdram, ctx, runtime);
        if (s_logged++ < 16u)
            std::cerr << std::hex << "[tta:heap] sbrk incr=0x" << increment << " break=0x" << before << " -> 0x"
                      << getRegU32(ctx, 2) << " limit=0x" << runtime->guestHeapLimit() << std::dec << std::endl;
    }

    void applyTta(PS2Runtime &runtime)
    {
        if (std::getenv("TTA_TRACE_ALLOC"))
        {
            runtime.registerFunction(kAllocCall, &allocCallTrace);
            runtime.registerFunction(kEaHeapSetup, &eaHeapSetupTrace);
            runtime.registerFunction(kEaHeapAlloc, &eaHeapAllocTrace);
            runtime.registerFunction(kMalloc, &mallocTrace);
            runtime.registerFunction(kSbrk, &sbrkTrace);
        }
        if (const char *top = std::getenv("TTA_MEMTOP"); top && *top)
        {
            g_memoryTop = static_cast<uint32_t>(std::strtoul(top, nullptr, 0));
            runtime.registerFunction(kMemoryInfo, &memoryInfoOverride);
        }
        std::cout << "[tta] The Third Age (NTSC-U SLUS-21027) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("The Lord of the Rings - The Third Age (USA)", "SLUS_210.27", kTtaNtscEntry, 0u, &applyTta)
