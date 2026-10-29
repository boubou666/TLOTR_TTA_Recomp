// Native PC rendering for The Third Age (on by default, TTA_NATIVE=0 turns it off): no VU1 program of the game runs
// on the emulated VU1 or reaches the GS. The fork's generic adapter (ps2x::native::Vu1StreamAdapter) reads every VIF1 stream TTA sends,
// runs each microprogram on its own VU1 copy (the sandbox) and draws what it sends the GS with the native renderer;
// DIRECT packets that only draw go the same way. TTA's own programs move to translated code one by one (Choice::Port).
//
// The frame ends at the game's frame swap (0x212478) and at the movie player's sceGsSwapDBuffDc (0x277448): the frame
// swap calls sceGsSwapDBuffDc only outside the game.
// TTA_NATIVE_LOG=1 turns on the adapter's statistics (programs drawn natively per start address, per 250 frames).

#include "tta_native.h"

#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#if __has_include("runtime/native/native_vu1_stream.h")
#include "runtime/native/native_vu1_stream.h"
#define TTA_HAVE_VU1_STREAM 1
#endif

#ifdef TTA_HAVE_VU1_STREAM
namespace
{
    constexpr uint32_t kSwapDBuffDc = 0x00277448u;
    PS2Runtime::RecompiledFunction g_swapDBuffDc = nullptr; // the HLE stub it replaces

    // The game's own frame swap: it calls sceGsSwapDBuffDc only with a0 = 0 (not in game), so the frame ends here.
    constexpr uint32_t kFrameSwap = 0x00212478u;
    PS2Runtime::RecompiledFunction g_frameSwap = nullptr;
    bool g_inFrameSwap = false;

    void swapDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_swapDBuffDc(rdram, ctx, runtime);
        if (!g_inFrameSwap) // the movie player's flips
            ps2x::native::Vu1StreamAdapter::instance().frameEnd();
    }

    void frameSwap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_inFrameSwap = true;
        g_frameSwap(rdram, ctx, runtime);
        g_inFrameSwap = false;
        ps2x::native::Vu1StreamAdapter::instance().frameEnd();
    }

    // TTA draws a 512 x 448 picture (sceGsSetDefDBuffDc; the GS frame shows it 512 pixels wide), centred on the
    // GS's 2048, 2048 (XYOFFSET).
    ps2x::native::Screen screen()
    {
        ps2x::native::Screen screen;
        screen.width = 512.0f;
        screen.height = 448.0f;
        screen.originX = 2048.0f - screen.width / 2.0f;
        screen.originY = 2048.0f - screen.height / 2.0f;
        return screen;
    }
}
#endif

#if __has_include("runtime/native/native_vif1.h")
#include "runtime/native/native_vif1.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

namespace
{
    // TTA_VU1_STATS=1: which VU1 programs the game starts, counted from the VIF1 stream without changing what runs.
    // Every 5 s: the MSCAL start addresses with the microcode they ran in (a checksum of VU1 code memory as the
    // stream's MPGs left it), to choose the programs worth translating first.
    struct Vu1Stats
    {
        uint32_t code[4096] = {}; // VU1 micro memory as the MPGs fill it, in words
        uint32_t codeSum = 0;
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> calls; // (code checksum, start pc) -> MSCALs
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    } g_vu1Stats;

    void vu1StatsObserve(const uint8_t *stream, size_t bytes)
    {
        Vu1Stats &st = g_vu1Stats;
        // TTA_VIF1_DUMP=<dir>: some whole VIF1 transfers that start program 0x460 (MSCAL 0x8c), as vif1_<n>.bin.
        static const char *s_vifDump = std::getenv("TTA_VIF1_DUMP");
        static int s_vifDumped = 0, s_vifSeen = 0;
        if (s_vifDump && s_vifDumped < 12)
        {
            const uint32_t *w = reinterpret_cast<const uint32_t *>(stream);
            bool has = false;
            for (size_t k = 0; k < bytes / 4u && !has; ++k)
                has = (w[k] & 0x7fffffffu) == 0x1400008cu;
            if (has && ++s_vifSeen % 50 == 0)
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/vif1_%02d.bin", s_vifDump, s_vifDumped++);
                if (FILE *file = std::fopen(path, "wb"))
                {
                    std::fwrite(stream, 1, bytes, file);
                    std::fclose(file);
                }
            }
        }
        const uint32_t *words = reinterpret_cast<const uint32_t *>(stream);
        const size_t count = bytes / 4u;
        size_t i = 0;
        while (i < count)
        {
            const uint32_t vifcode = words[i++];
            const uint32_t cmd = (vifcode >> 24) & 0x7Fu, num = (vifcode >> 16) & 0xFFu, imm = vifcode & 0xFFFFu;
            if ((cmd & 0x60u) == 0x60u)
            {
                // UNPACK: num vectors of vn+1 elements of 32 >> vl bits (V4-5: 16 bits each), padded to a word.
                const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u;
                const uint32_t n = num ? num : 256u;
                const uint32_t bits = (vn == 3u && vl == 3u ? 16u : (vn + 1u) * (32u >> vl)) * n; // V4-5: 16 bits
                i += (bits + 31u) / 32u;
                continue;
            }
            switch (cmd)
            {
            case 0x20: i += 1; break;            // STMASK
            case 0x30: case 0x31: i += 4; break; // STROW, STCOL
            case 0x4A:                           // MPG
            {
                const size_t size = static_cast<size_t>(num ? num : 256u) * 2u;
                for (size_t w = 0; w < size && i + w < count; ++w)
                    st.code[(imm * 2u + w) & 4095u] = words[i + w];
                uint32_t sum = 0;
                for (uint32_t w = 0; w < 4096u; ++w)
                    sum = sum * 31u + st.code[w];
                st.codeSum = sum;
                // TTA_VU1_DUMP=<dir>: each code image once, as <dir>/vu1_<checksum>.bin (for tools/vu_disasm.py).
                static const char *s_dumpDir = std::getenv("TTA_VU1_DUMP");
                static std::map<uint32_t, bool> s_dumped;
                if (s_dumpDir && !s_dumped[sum])
                {
                    s_dumped[sum] = true;
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/vu1_%08x.bin", s_dumpDir, sum);
                    if (FILE *file = std::fopen(path, "wb"))
                    {
                        std::fwrite(st.code, sizeof(st.code), 1, file);
                        std::fclose(file);
                    }
                }
                i += size;
                break;
            }
            case 0x50: case 0x51: ++st.calls[{0xd1ec7u, 0u}]; i += static_cast<size_t>(imm ? imm : 65536u) * 4u; break; // DIRECT(HL), counted as d1ec7:0
            case 0x14: case 0x15: ++st.calls[{st.codeSum, imm * 8u}]; break;                  // MSCAL(F)
            default: break;
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - st.start).count();
        if (seconds >= 5.0)
        {
            std::string line;
            for (const auto &[key, calls] : st.calls)
            {
                char item[48];
                std::snprintf(item, sizeof(item), " %08x:%#x x%llu", key.first, key.second, static_cast<unsigned long long>(calls));
                line += item;
            }
            std::fprintf(stderr, "[tta:vu1] 5 s:%s%c", line.c_str(), 10);
            st.calls.clear();
            st.start = std::chrono::steady_clock::now();
        }
    }
}
#define TTA_HAVE_VIF1_OBSERVER 1
#endif

namespace tta
{
    // On by default (the first battle and what follows it match the VU1 picture at the same speed); TTA_NATIVE=0 runs
    // the microprograms on the emulated VU1 (AOT-compiled) instead.
    bool nativeEnabled()
    {
        const char *value = std::getenv("TTA_NATIVE");
        return !value || *value != '0';
    }

    void installNative(PS2Runtime &runtime)
    {
#ifdef TTA_HAVE_VIF1_OBSERVER
        if (std::getenv("TTA_VU1_STATS") && !nativeEnabled())
            ps2x::native::setVif1Observer(&vu1StatsObserve);
#endif
        if (!nativeEnabled())
            return;
#ifndef TTA_HAVE_VU1_STREAM
        (void)runtime;
        std::cout << "[tta] TTA_NATIVE: this fork has no ps2x::native::Vu1StreamAdapter (branch native/vu1-stream)" << std::endl;
#else
        if (const char *log = std::getenv("TTA_NATIVE_LOG"); log && *log == '1' && !std::getenv("PS2X_NATIVE_STREAM_LOG"))
        {
#ifdef _WIN32
            _putenv("PS2X_NATIVE_STREAM_LOG=1");
#else
            setenv("PS2X_NATIVE_STREAM_LOG", "1", 0);
#endif
        }
        ps2x::native::Vu1StreamAdapter::Config config;
        config.screen = screen();
        // TTA draws into its own targets and textures from them: the GL GS renderer draws the sandbox's packets.
        config.gsWrites = true;
        // TTA_VU1_SNAP=<dir>: VU1 data memory at some MSCALs of 0x460 and 0x268 (the hot programs), as
        // <dir>/snap_<pc>_<n>_top<TOP>.bin, to read their input layout; the programs still run on the sandbox.
        if (const char *snapDir = std::getenv("TTA_VU1_SNAP"))
        {
            const std::string dir = snapDir;
            config.choose = [dir](ps2x::native::Vu1StreamAdapter &adapter, uint32_t startPc) {
                static uint64_t s_calls = 0;
                static int s_written = 0;
                if ((startPc == 0x460u || startPc == 0x268u) && ++s_calls % 4000u == 0u && s_written < 24)
                {
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/snap_%x_%02d_top%x.bin", dir.c_str(), startPc, s_written++, adapter.top());
                    if (FILE *file = std::fopen(path, "wb"))
                    {
                        std::fwrite(adapter.data(), 16, 1024, file);
                        std::fwrite(adapter.code(), 1, 16 * 1024, file);
                        std::fclose(file);
                    }
                }
                return ps2x::native::Vu1StreamAdapter::Choice::Sandbox;
            };
        }
        ps2x::native::Vu1StreamAdapter::instance().install(config);
#ifdef TTA_HAVE_VIF1_OBSERVER
        // TTA_VU1_STATS=1 with the adapter: both read each VIF1 transfer (the statistics first).
        if (std::getenv("TTA_VU1_STATS"))
            ps2x::native::setVif1Observer([](const uint8_t *stream, size_t bytes) {
                vu1StatsObserve(stream, bytes);
                ps2x::native::Vu1StreamAdapter::instance().observe(stream, bytes);
            });
#endif
        g_swapDBuffDc = runtime.lookupFunction(kSwapDBuffDc);
        if (g_swapDBuffDc)
            runtime.registerFunction(kSwapDBuffDc, &swapDBuffDc);
        else
            std::cout << "[tta] TTA_NATIVE: no function at sceGsSwapDBuffDc 0x277448, the native frame never ends" << std::endl;
        g_frameSwap = runtime.lookupFunction(kFrameSwap);
        if (g_frameSwap)
            runtime.registerFunction(kFrameSwap, &frameSwap);
        std::cout << "[tta] native renderer: every VU1 program on the sandbox, drawn natively" << std::endl;
#endif
    }
}
