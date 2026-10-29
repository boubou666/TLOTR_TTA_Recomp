// Native PC rendering for The Third Age (TTA_NATIVE=1): no VU1 program of the game runs on the emulated VU1 or
// reaches the GS. The fork's generic adapter (ps2x::native::Vu1StreamAdapter) reads every VIF1 stream TTA sends,
// runs each microprogram on its own VU1 copy (the sandbox) and draws what it sends the GS with the native renderer;
// DIRECT packets that only draw go the same way. TTA's own programs move to translated code one by one (Choice::Port).
//
// The frame ends at each sceGsSwapDBuffDc (0x277448): the game's frame swap (0x212478) calls it only in some states,
// and the movie player flips on its own.
// TTA_NATIVE_LOG=1 turns on the adapter's statistics (programs drawn natively per start address, per 250 frames).

#include "tta_native.h"

#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"

#include <cstdlib>
#include <iostream>

#if __has_include("runtime/native/native_vu1_stream.h")
#include "runtime/native/native_vu1_stream.h"
#define TTA_HAVE_VU1_STREAM 1
#endif

#ifdef TTA_HAVE_VU1_STREAM
namespace
{
    constexpr uint32_t kSwapDBuffDc = 0x00277448u;
    PS2Runtime::RecompiledFunction g_swapDBuffDc = nullptr; // the HLE stub it replaces

    void swapDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_swapDBuffDc(rdram, ctx, runtime);
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

namespace tta
{
    bool nativeEnabled()
    {
        const char *value = std::getenv("TTA_NATIVE");
        return value && *value == '1';
    }

    void installNative(PS2Runtime &runtime)
    {
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
        ps2x::native::Vu1StreamAdapter::instance().install(config);
        g_swapDBuffDc = runtime.lookupFunction(kSwapDBuffDc);
        if (g_swapDBuffDc)
            runtime.registerFunction(kSwapDBuffDc, &swapDBuffDc);
        else
            std::cout << "[tta] TTA_NATIVE: no function at sceGsSwapDBuffDc 0x277448, the native frame never ends" << std::endl;
        std::cout << "[tta] native renderer: every VU1 program on the sandbox, drawn natively" << std::endl;
#endif
    }
}
