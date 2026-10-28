#pragma once

class PS2Runtime;

namespace tta
{
    // TTA_NATIVE=1: the game's VU1 programs drawn by the native renderer (runtime/tta_native.cpp).
    bool nativeEnabled();
    void installNative(PS2Runtime &runtime);
}
