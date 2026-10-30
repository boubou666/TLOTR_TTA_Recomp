#pragma once

#include <cstdint>

#if __has_include("runtime/native/native_vu1_stream.h")
#include "runtime/native/native_vu1_stream.h"

namespace tta
{
    // Whether the program at startPc in this VU1 code image is one TTA draws in C++ (tta_vu1_port.cpp).
    bool vu1PortProgram(const uint8_t *code, uint32_t startPc);
    // The adapter's Config::draw for those programs: at the MSCAL (resumed = false) and at each MSCNT.
    void vu1PortDraw(ps2x::native::Vu1StreamAdapter &adapter, uint32_t startPc, bool resumed);
}
#endif
