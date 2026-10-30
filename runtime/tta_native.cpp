// Native PC rendering for The Third Age (on by default, TTA_NATIVE=0 turns it off): no VU1 program of the game runs
// on the emulated VU1 or reaches the GS. The fork's generic adapter (ps2x::native::Vu1StreamAdapter) reads every VIF1 stream TTA sends,
// runs each microprogram on its own VU1 copy (the sandbox) and draws what it sends the GS with the native renderer;
// DIRECT packets that only draw go the same way. TTA's own programs move to translated code one by one (Choice::Port).
//
// The frame ends at the game's frame swap (0x212478) and at the movie player's sceGsSwapDBuffDc (0x277448): the frame
// swap calls sceGsSwapDBuffDc only outside the game.
// TTA_NATIVE_LOG=1 turns on the adapter's statistics (programs drawn natively per start address, per 250 frames).

#include "tta_native.h"
#include "tta_vu1_port.h"

#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
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
    constexpr uint32_t kFrameSwapFlipReturn = 0x002124D0u; // after the frame swap's own sceGsSwapDBuffDc
    PS2Runtime::RecompiledFunction g_frameSwap = nullptr;

    void swapDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool movie = getRegU32(ctx, 31) != kFrameSwapFlipReturn; // the movie player flips on its own
        g_swapDBuffDc(rdram, ctx, runtime);
        if (movie)
            ps2x::native::Vu1StreamAdapter::instance().frameEnd();
    }

    // Native frame loop (on by default, TTA_NATIVE_LOOP=0 keeps the vblank pacing): in game, frames are paced by the
    // host's clock, not by the emulated vblanks. The frame swap's vblank wait (its sceGsSyncV) waits for the next
    // frame deadline of the runtime's frame cap (PS2X_FRAME_CAP, default 60) on the host clock instead, with the
    // other guest threads running meanwhile (and the swap itself when it has no wait). At the deadline the EE is charged the real time the frame took (its
    // timers and the IOP keep up) and the frame is shown: vblanks no longer latch it (EeScheduler::setNativeFrames).
    // TTA's engine already steps on COP0 Count, which runs on the host clock. Movies keep the vblank wait
    // (sceGsSyncV, 0x2764F0) for now, see kMpegGetPicture.
    constexpr uint32_t kSyncV = 0x002764F0u;
    constexpr uint32_t kFrameSwapSyncVReturn = 0x002124B0u; // after the frame swap's own sceGsSyncV
    PS2Runtime::RecompiledFunction g_syncV = nullptr;       // the vblank wait it replaces
    constexpr double kEeClockHz = 294912000.0;
    bool g_nativeLoop = false;
    bool g_waitedThisFrame = false;
    // Movies keep the vblank pacing for now: their pictures come from the MPEG HLE (sceMpegGetPicture, 0x27ED48),
    // which presents on EE-time vsync ticks, and with native frames the logo films ran at 2.8 fps instead of 15
    // (boots 134, 136, 137). A frame is a movie's while a picture was asked for in the last half second.
    constexpr uint32_t kMpegGetPicture = 0x0027ED48u;
    PS2Runtime::RecompiledFunction g_getPicture = nullptr;
    std::chrono::steady_clock::time_point g_lastPicture{};
    bool g_nativeFrame = false; // this frame is paced by the host

    void getPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_lastPicture = std::chrono::steady_clock::now(); // before: the call may suspend the thread
        g_getPicture(rdram, ctx, runtime);
    }

    std::chrono::steady_clock::time_point nextDeadline(PS2Runtime *runtime)
    {
        static auto s_next = std::chrono::steady_clock::now();
        const uint32_t cap = runtime->framePacer().cap();
        const auto now = std::chrono::steady_clock::now();
        s_next = cap == 0u ? now
                           : std::max(now, s_next + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                        std::chrono::duration<double>(1.0 / cap)));
        return s_next;
    }

    void frameWake(PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        static uint64_t s_lastCycle = scheduler.currentCycle();
        static auto s_last = std::chrono::steady_clock::now();
        const auto woke = std::chrono::steady_clock::now();
        const double seconds = std::min(0.1, std::chrono::duration<double>(woke - s_last).count());
        s_last = woke;
        const uint64_t accounted = scheduler.currentCycle() - s_lastCycle;
        const uint64_t real = static_cast<uint64_t>(seconds * kEeClockHz);
        if (real > accounted)
            scheduler.accountCycles(static_cast<uint32_t>(std::min<uint64_t>(real - accounted, 0xFFFFFFFFu)));
        s_lastCycle = scheduler.currentCycle();
        scheduler.presentGuestFrame();
    }

    void nativeSyncV(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        // The movie player's and the loading screens' waits poll for work the IOP finishes (a CD read): on the host
        // clock the IOP only catches up at each wake-up, and those loops ran 1.5x (movies) to 20x (loads) slower
        // (boots 134, 136). They keep the vblank wait for now; only the frame swap's is the frame deadline.
        if (ra != kFrameSwapSyncVReturn || !g_nativeFrame)
        {
            g_syncV(rdram, ctx, runtime);
            return;
        }
        g_waitedThisFrame = true;
        runtime->eeScheduler().waitHostDeadline(
            nextDeadline(runtime), [runtime] { frameWake(runtime); },
            [runtime, ra](R5900Context &resume)
            {
                // The field (CSR.FIELD) as sceGsSyncV returns it: the vblanks still run on EE time.
                setReturnU32(&resume, static_cast<uint32_t>((runtime->memory().gs().csr.load() >> 13) & 1u));
                resume.pc = ra;
            });
    }

    void frameSwap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The frame's draws are all sent: its native frame ends here. (The swap's own sceGsSyncV, with the native
        // loop, suspends the thread and resumes it inside the swap: nothing after the call below runs then.)
        ps2x::native::Vu1StreamAdapter::instance().frameEnd();
        g_waitedThisFrame = false;
        if (g_nativeLoop)
        {
            const bool inGame = std::chrono::steady_clock::now() - g_lastPicture > std::chrono::milliseconds(500);
            if (inGame != g_nativeFrame)
                runtime->eeScheduler().setNativeFrames(inGame);
            g_nativeFrame = inGame;
        }
        g_frameSwap(rdram, ctx, runtime);
        if (!g_nativeFrame || g_waitedThisFrame)
            return;
        const uint32_t ret = ctx->pc;
        runtime->eeScheduler().waitHostDeadline(nextDeadline(runtime), [runtime] { frameWake(runtime); },
                                                [ret](R5900Context &resume) { resume.pc = ret; });
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
        // TTA_VU1_THREAD=1: the adapter, its sandbox and the C++ programs run on the VIF1 thread beside the EE, as VU1
        // did (SC3's Config::onVif1Thread); the ports only read VU memory, never EE memory the stream points to.
        if (const char *thread = std::getenv("TTA_VU1_THREAD"); thread && *thread == '1')
            config.onVif1Thread = true;
        // The programs TTA draws in C++ (tta_vu1_port.cpp: 0x460 and 0x268 of its 3D image); the others on the sandbox.
        // TTA_VU1_PORT=0 leaves them all on the sandbox.
        const char *port = std::getenv("TTA_VU1_PORT");
        if (!port || *port != '0')
        {
            config.choose = [](ps2x::native::Vu1StreamAdapter &adapter, uint32_t startPc) {
                return tta::vu1PortProgram(adapter.code(), startPc) ? ps2x::native::Vu1StreamAdapter::Choice::Port
                                                                    : ps2x::native::Vu1StreamAdapter::Choice::Sandbox;
            };
            config.draw = &tta::vu1PortDraw;
        }
        // TTA_VU1_SNAP=<dir>: VU1 data memory at some MSCALs of 0x460 and 0x268 (the hot programs), as
        // <dir>/snap_<pc>_<n>_top<TOP>.bin, to read their input layout; the programs run on the sandbox.
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
        // Every native draw of TTA goes to the GS front end (gsWrites, the C++ programs' fromGsContext batches), whose GL
        // renderer keeps its own textures: the native renderer's copy of GS memory (TextureCache, filled at each upload
        // on the GS front end's thread) is never read, and kept it was about a third of that thread's time in battle
        // (TTA_HOST_PROFILE, boot 144). TTA_TEXTURE_CACHE=1 keeps it.
        if (const char *cache = std::getenv("TTA_TEXTURE_CACHE"); !cache || *cache != '1')
            ps2x::native::setUploadsWanted(false);
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
        if (const char *loop = std::getenv("TTA_NATIVE_LOOP"); (!loop || *loop != '0') && g_frameSwap)
        {
            g_nativeLoop = true;
            if ((g_syncV = runtime.lookupFunction(kSyncV)))
                runtime.registerFunction(kSyncV, &nativeSyncV);
            if ((g_getPicture = runtime.lookupFunction(kMpegGetPicture)))
                runtime.registerFunction(kMpegGetPicture, &getPicture);
            std::cout << "[tta] native loop: frames paced by the host clock (cap " << runtime.framePacer().cap()
                      << " fps, 0 = unlimited), no vblank pacing" << std::endl;
        }
        std::cout << "[tta] native renderer: VU1 programs 0x460 / 0x268 in C++" << (config.draw ? "" : " (off)")
                  << ", the others on the sandbox, all drawn natively" << std::endl;
#endif
    }
}
