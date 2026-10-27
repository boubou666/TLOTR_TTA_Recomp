// Game-specific runtime setup for The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    // TTA_TRACE_CALLS=1 (bring-up aid): the first calls of a few functions with their caller, arguments and result.
    // The loading screen: 0x1C39E8 shows it (0x1C0FA8 sets the byte at 0x3E9C68), loads through 0x1D66D8 and
    // 0x1DA420, and only hides it (0x1C0FF0) when both succeed.
    template <uint32_t Address, void (*Function)(uint8_t *, R5900Context *, PS2Runtime *)>
    void callTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0;
        const uint32_t n = ++s_calls, ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
        Function(rdram, ctx, runtime);
        if (n <= 16u)
            std::fprintf(stderr, "[tta:call] 0x%x #%u ra=0x%x a0=0x%x a1=0x%x -> 0x%x\n", Address, n, ra, a0, a1,
                         getRegU32(ctx, 2));
    }

    // TTA_TRACE_STRIPS=1 (bring-up aid): 0x218FB8 turns vertices in the scratchpad into GIF strips (STQ, RGBA,
    // XYZF2). After the loading screen every strip it draws lands off screen; log its arguments, the render
    // record at [0x36F870] and the scratchpad it reads, every 600th call.
    constexpr uint32_t kRenderRecord = 0x0036F870u;

    void dumpFloats(const char *label, const uint8_t *base, uint32_t bytes)
    {
        std::fprintf(stderr, "[tta:strip] %s:", label);
        for (uint32_t offset = 0; offset < bytes; offset += 4u)
        {
            float value;
            std::memcpy(&value, base + offset, sizeof(value));
            std::fprintf(stderr, "%s%g", (offset % 16u) ? " " : (offset ? " | " : " "), value);
        }
        std::fprintf(stderr, "\n");
    }

    // 0x2C9A08 (mesh renderer, the strips' caller) multiplies the matrices at 0x418A40 and 0x418A80 with the VU0
    // microprogram at 0xA80 (VCALLMS) and transforms its vertices with the product.
    void meshTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0;
        if ((s_calls++ % 600u) == 0u && s_calls < 600u * 6u)
        {
            const uint8_t *code = runtime->memory().getVU0Code();
            std::fprintf(stderr, "[tta:mesh] #%u vu0 code generation %llu, microprogram 0xA80:", s_calls,
                         static_cast<unsigned long long>(runtime->memory().getVU0CodeGeneration()));
            for (uint32_t offset = 0; code && offset < 0x40u; offset += 4u)
            {
                uint32_t word;
                std::memcpy(&word, code + 0xA80u + offset, sizeof(word));
                std::fprintf(stderr, " %08x", word);
            }
            std::fprintf(stderr, "\n");
            dumpFloats("matrix 0x418A40", rdram + 0x418A40u, 0x40u);
            dumpFloats("matrix 0x418A80", rdram + 0x418A80u, 0x40u);
            // Run the same VCALLMS on a copy of the context: vf9-vf12 should be the rows of 0x418A40 times
            // the matrix at 0x418A80.
            R5900Context probe = *ctx;
            for (int row = 0; row < 4; ++row)
            {
                std::memcpy(&probe.vu0_vf[1 + row], rdram + 0x418A40u + row * 16u, 16u);
                std::memcpy(&probe.vu0_vf[5 + row], rdram + 0x418A80u + row * 16u, 16u);
            }
            runtime->vu0StartMicroProgram(rdram, &probe, 0xA80u);
            float product[16];
            for (int row = 0; row < 4; ++row)
                std::memcpy(product + row * 4, &probe.vu0_vf[9 + row], 16u);
            dumpFloats("vu0 product", reinterpret_cast<const uint8_t *>(product), 0x40u);
            float a[16], b[16], expected[16] = {};
            std::memcpy(a, rdram + 0x418A40u, sizeof(a));
            std::memcpy(b, rdram + 0x418A80u, sizeof(b));
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    for (int k = 0; k < 4; ++k)
                        expected[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col];
            dumpFloats("expected", reinterpret_cast<const uint8_t *>(expected), 0x40u);
        }
        FUN_002c9a08_0x2c9a08(rdram, ctx, runtime);
    }

    void stripTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0;
        if ((s_calls++ % 600u) == 0u && s_calls < 600u * 12u)
        {
            const uint32_t record = rd32(rdram, kRenderRecord);
            std::fprintf(stderr, "[tta:strip] #%u ra=0x%x a0=0x%x a1=0x%x a2=0x%x a3=0x%x t0=0x%x t1=0x%x record=0x%x mode=%u\n",
                         s_calls, getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                         getRegU32(ctx, 7), getRegU32(ctx, 8), getRegU32(ctx, 9), record,
                         rdram[(record + 0x90u) & 0x1FFFFFFu]);
            dumpFloats("record+0x40", rdram + ((record + 0x40u) & 0x1FFFFF0u), 0x40u);
            if (const uint8_t *scratchpad = ps2GetScratchpadHostPtr())
                dumpFloats("spr", scratchpad, 0x80u);
            // a1: the transformed vertices (x, y, z, w per qword), t0: the second array (colours/UVs).
            dumpFloats("a1 vertices", rdram + (getRegU32(ctx, 5) & 0x1FFFFF0u), 0x80u);
            dumpFloats("t0 array", rdram + (getRegU32(ctx, 8) & 0x1FFFFF0u), 0x40u);
        }
        FUN_00218fb8_0x218fb8(rdram, ctx, runtime);
    }

    // TTA's newlib sbrk (0x328C30) owns guest RAM from the end of its BSS (0x448E10) up to EndOfHeap, and EA carves
    // its whole dlmalloc heap out of it. The runtime's guest heap (HLE GIF packets of sceGsResetGraph and
    // sceGsExecLoadImage/StoreImage, MPEG callback data, thread stacks) starts at the same base by default:
    // sceGsResetGraph's packet wiped the heap's top chunk header and every allocation failed. The runtime's heap goes
    // to the 1 MB under its reserved area instead, and the game's sbrk stops there.
    constexpr uint32_t kRuntimeHeapBytes = 0x00100000u;

    // TTA's VU1 strips come out as garbage when the AOT-compiled microprograms skip the MAC/status flags their
    // observer analysis believes unread (raw floats in STQ, Y at 0x7FFF): one of TTA's flag readers is missed.
    // Until the analysis sees it, the game computes every flag (PS2X_VU1_EXACT_FLAGS, read once on first use).
    void requestExactVu1Flags()
    {
        if (std::getenv("PS2X_VU1_EXACT_FLAGS"))
            return;
#ifdef _WIN32
        _putenv("PS2X_VU1_EXACT_FLAGS=1");
#else
        setenv("PS2X_VU1_EXACT_FLAGS", "1", 0);
#endif
    }

    void applyTta(PS2Runtime &runtime)
    {
        requestExactVu1Flags();
        // paraLLEl-GS by default (PS2X_GS_RENDERER still overrides it): the CPU rasterizer draws TTA's movie
        // frames as solid magenta, its fallback colour for a texture it cannot sample.
        if (runtime.gsForPresentation().requestParallelRenderer())
            std::cout << "[tta] GS renderer: paraLLEl-GS" << std::endl;
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
            runtime.registerFunction(0x001C39E8u, &callTrace<0x1C39E8u, FUN_001c39e8_0x1c39e8>);
            runtime.registerFunction(0x001C0FA8u, &callTrace<0x1C0FA8u, FUN_001c0fa8_0x1c0fa8>);
            runtime.registerFunction(0x001C0FF0u, &callTrace<0x1C0FF0u, FUN_001c0ff0_0x1c0ff0>);
            runtime.registerFunction(0x001D66D8u, &callTrace<0x1D66D8u, FUN_001d66d8_0x1d66d8>);
            runtime.registerFunction(0x001D6880u, &callTrace<0x1D6880u, FUN_001d6880_0x1d6880>);
            runtime.registerFunction(0x001DA420u, &callTrace<0x1DA420u, FUN_001da420_0x1da420>);
            // GameDataStorage (save manager, object 0x3A2300): SetState (a1 = state) and the vtable 0xA4 check
            // 0x1C39E8 makes before hiding the loading screen.
            runtime.registerFunction(0x0019A6C0u, &callTrace<0x19A6C0u, FUN_0019a6c0_0x19a6c0>);
            runtime.registerFunction(0x00333F60u, &callTrace<0x333F60u, entry_333f60_0x333fb8>);
        }
        if (std::getenv("TTA_TRACE_STRIPS"))
        {
            runtime.registerFunction(0x00218FB8u, &stripTrace);
            runtime.registerFunction(0x002C9A08u, &meshTrace);
        }
        std::cout << "[tta] The Third Age (NTSC-U SLUS-21027) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("The Lord of the Rings - The Third Age (USA)", "SLUS_210.27", kTtaNtscEntry, 0u, &applyTta)
