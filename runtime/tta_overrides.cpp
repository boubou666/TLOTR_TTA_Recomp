// Game-specific runtime setup for The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

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

    // dlmalloc's malloc (0x24F780) and the helpers it calls on its slow paths: for the first calls, log the arguments,
    // the result and which helpers ran in between, to see where a failing request leaves.
    constexpr uint32_t kDlMalloc = 0x0024F780u;
    uint32_t g_dlMallocDepth = 0u, g_dlMallocCalls = 0u;
    std::string g_dlMallocPath;

    template <uint32_t Address, void (*Function)(uint8_t *, R5900Context *, PS2Runtime *)>
    void dlHelperTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
        Function(rdram, ctx, runtime);
        if (g_dlMallocDepth != 0u && g_dlMallocCalls <= 4u)
        {
            char step[96];
            std::snprintf(step, sizeof(step), " %x(%x,%x)->%x", Address, a0, a1, getRegU32(ctx, 2));
            g_dlMallocPath += step;
        }
    }

    void dlMallocTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t arena = getRegU32(ctx, 4), size = getRegU32(ctx, 5), flags = getRegU32(ctx, 6);
        const uint32_t ra = getRegU32(ctx, 31), n = ++g_dlMallocCalls;
        if (g_dlMallocDepth++ == 0u)
            g_dlMallocPath.clear();
        FUN_0024f780_0x24f780(rdram, ctx, runtime);
        --g_dlMallocDepth;
        if (n <= 4u)
            std::cerr << std::hex << "[tta:heap] dlmalloc #" << std::dec << n << std::hex << " arena=0x" << arena
                      << " size=0x" << size << " flags=0x" << flags << " ra=0x" << ra << " -> 0x" << getRegU32(ctx, 2)
                      << " max_fast=0x" << rd32(rdram, arena + 4u) << " split=0x" << rd32(rdram, arena + 0x468u)
                      << " byte46c=0x" << (rd32(rdram, arena + 0x46Cu) & 0xFFFFu) << " runtimeHeapEnd=0x"
                      << runtime->guestHeapEnd() << " path:" << g_dlMallocPath
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

    // TTA_TRACE_CALLS=1 (bring-up aid): the first calls of a few functions with their caller, stack and arguments.
    // 0x125FA8 (reached through 0x125C90's vtable slot) calls a method of the object at 0x3A1728, which 0x1552C0
    // only sets later in the boot; 0x212638 is the game's sceGsSyncVCallback handler.
    constexpr uint32_t kTracedObject = 0x003A1728u;
    constexpr uint32_t kVSyncHook = 0x003F6E54u; // called by 0x212638 when set

    template <uint32_t Address, void (*Function)(uint8_t *, R5900Context *, PS2Runtime *)>
    void callTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0, s_hook = 0;
        const uint32_t hook = rd32(rdram, kVSyncHook);
        const bool hookChanged = hook != s_hook;
        s_hook = hook;
        if (s_calls++ < 4u || hookChanged)
            std::fprintf(stderr, "[tta:call] 0x%x #%u ra=0x%x sp=0x%x a0=0x%x a1=0x%x [0x3a1728]=0x%x [0x3f6e54]=0x%x\n", Address,
                         s_calls, getRegU32(ctx, 31), getRegU32(ctx, 29), getRegU32(ctx, 4), getRegU32(ctx, 5),
                         rd32(rdram, kTracedObject), rd32(rdram, kVSyncHook));
        Function(rdram, ctx, runtime);
    }

    // TTA's newlib sbrk (0x328C30) owns guest RAM from the end of its BSS (0x448E10) up to EndOfHeap, and EA carves
    // its whole dlmalloc heap out of it. The runtime's guest heap (HLE GIF packets of sceGsResetGraph and
    // sceGsExecLoadImage/StoreImage, MPEG callback data, thread stacks) starts at the same base by default:
    // sceGsResetGraph's packet wiped the heap's top chunk header and every allocation failed. The runtime's heap goes
    // to the 1 MB under its reserved area instead, and the game's sbrk stops there.
    constexpr uint32_t kRuntimeHeapBytes = 0x00100000u;

    void applyTta(PS2Runtime &runtime)
    {
        runtime.pinGuestHeap(PS2Runtime::reservedMemoryBase() - kRuntimeHeapBytes, PS2Runtime::reservedMemoryBase());
        if (std::getenv("TTA_TRACE_ALLOC"))
        {
            runtime.registerFunction(kAllocCall, &allocCallTrace);
            runtime.registerFunction(kEaHeapSetup, &eaHeapSetupTrace);
            runtime.registerFunction(kEaHeapAlloc, &eaHeapAllocTrace);
            runtime.registerFunction(kMalloc, &mallocTrace);
            runtime.registerFunction(kSbrk, &sbrkTrace);
            runtime.registerFunction(kDlMalloc, &dlMallocTrace);
            runtime.registerFunction(0x0024EAA0u, &dlHelperTrace<0x24EAA0u, FUN_0024eaa0_0x24eaa0>);
            runtime.registerFunction(0x002506C0u, &dlHelperTrace<0x2506C0u, FUN_002506c0_0x2506c0>);
            runtime.registerFunction(0x0024ECE8u, &dlHelperTrace<0x24ECE8u, FUN_0024ece8_0x24ece8>);
            runtime.registerFunction(0x00250A78u, &dlHelperTrace<0x250A78u, FUN_00250a78_0x250a78>);
            runtime.registerFunction(0x0024FF28u, &dlHelperTrace<0x24FF28u, FUN_0024ff28_0x24ff28>);
        }
        if (std::getenv("TTA_TRACE_CALLS"))
        {
            runtime.registerFunction(0x00125C90u, &callTrace<0x125C90u, FUN_00125c90_0x125c90>);
            runtime.registerFunction(0x00125FA8u, &callTrace<0x125FA8u, FUN_00125fa8_0x125fa8>);
            runtime.registerFunction(0x001552C0u, &callTrace<0x1552C0u, FUN_001552c0_0x1552c0>);
            runtime.registerFunction(0x00212638u, &callTrace<0x212638u, FUN_00212638_0x212638>);
        }
        std::cout << "[tta] The Third Age (NTSC-U SLUS-21027) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("The Lord of the Rings - The Third Age (USA)", "SLUS_210.27", kTtaNtscEntry, 0u, &applyTta)
