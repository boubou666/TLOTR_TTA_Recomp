// TTA's two hottest VU1 microprograms as C++ (the adapter's Choice::Port): what they would send the GS is drawn
// natively, without running them on any VU. Both live in the code image the game uploads for its 3D (checksum
// 8e45395d in TTA_VU1_STATS):
//
//   0x460 (~210 per battle frame): a textured, fogged triangle strip. Its MSCAL only loads its registers (the matrix
//         at VU 0-3, 129.w for fog, vf5 / vf6 at 125 / 126 for its clip test) and ends; each batch then comes with an
//         MSCNT after its UNPACKs. At TOP: a GIF tag, optionally an A+D tag (its w word non-zero) with NLOOP register
//         writes before the PRIM tag; then two quadwords per vertex, the position (x, y, z) and (s, t, colour index);
//         bit 15 of the index is the GS's ADC (no triangle ends there). Per vertex v' = M (x, y, z, 1) (the matrix
//         maps to GS screen coordinates), Q = 1 / v'.w; out ST * Q, Q, the colour at VU 136 + index (integers) and
//         XYZF = v' * Q with F = min(129.w * Q, 255). vf5 * v' + vf6 * v'.w is only its guard-band clip space:
//         triangles crossing it go through a second clipping pass (0x830); here the GPU clips, and the near plane in
//         C++.
//   0x268 (~80 per battle frame): the same without the clip test (objects the game knows are on screen).
//
// The draws go to the GS front end (fromGsContext with the header's register writes, Material::gsWrites), so the GL
// GS renderer textures them from its own memory and targets, as it would the programs' XGKICK output.

#include "tta_vu1_port.h"

#if __has_include("runtime/native/native_vu1_stream.h")
#include "runtime/native/native_renderer.h"
#include "runtime/native/native_vu1_stream.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    using ps2x::native::Vu1StreamAdapter;

    constexpr uint32_t kStrip = 0x460u, kDirect = 0x268u;
    // FNV-1a of each program's code (0x460-0x830, 0x268-0x460) in that image: other images reuse the addresses.
    constexpr uint32_t kStripHash = 0x647d8296u, kDirectHash = 0x95db193du;
    constexpr float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    uint32_t fnv(const uint8_t *data, size_t bytes)
    {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < bytes; ++i)
            hash = (hash ^ data[i]) * 16777619u;
        return hash;
    }

    float asFloat(uint32_t bits)
    {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // The registers a program's MSCAL loaded, used by the MSCNTs that follow it (the stream may rewrite VU memory in
    // between for the next program).
    struct Registers
    {
        float m[4][4] = {};    // columns: VU 0-3, to GS screen coordinates
        float fogScale = 0.0f; // 129.w
    };
    Registers g_regs;
    uint32_t g_running = 0; // the program the MSCNTs continue

    void loadRegisters(const uint32_t *data)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                g_regs.m[c][r] = asFloat(data[c * 4 + r]);
        g_regs.fogScale = asFloat(data[129 * 4 + 3]);
    }

    // A vertex before the divide: the screen point times w, and what goes across the triangle linearly with it.
    struct Clip
    {
        float x, y, z, w;  // screen * v'.w (the divide by w gives GS coordinates), and v'.w
        float s, t;        // texture coordinates (S / Q, T / Q on the GS: they go across with Q)
        float r, g, b, a;  // colour
        bool adc;
    };

    void emit(std::vector<ps2x::native::Vertex> &out, const Clip &c, float zScale)
    {
        const float q = 1.0f / c.w;
        ps2x::native::Vertex v;
        v.x = c.x * q;
        v.y = c.y * q;
        v.z = c.z * q / zScale;
        v.u = c.s;
        v.v = c.t;
        v.q = q;
        v.fog = std::min(g_regs.fogScale * q, 255.0f) / 255.0f;
        const auto byte = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f) & 0xffu; };
        v.rgba = byte(c.r) | (byte(c.g) << 8) | (byte(c.b) << 16) | (byte(c.a) << 24);
        out.push_back(v);
    }

    Clip lerp(const Clip &a, const Clip &b, float f)
    {
        Clip c;
        c.x = a.x + (b.x - a.x) * f, c.y = a.y + (b.y - a.y) * f, c.z = a.z + (b.z - a.z) * f, c.w = a.w + (b.w - a.w) * f;
        c.s = a.s + (b.s - a.s) * f, c.t = a.t + (b.t - a.t) * f;
        c.r = a.r + (b.r - a.r) * f, c.g = a.g + (b.g - a.g) * f, c.b = a.b + (b.b - a.b) * f, c.a = a.a + (b.a - a.a) * f;
        c.adc = false;
        return c;
    }

    // One triangle, clipped to w >= kNear (what the VU's clipping pass does for the near plane; the GPU clips the rest).
    void triangle(std::vector<ps2x::native::Vertex> &out, const Clip &a, const Clip &b, const Clip &c, float zScale)
    {
        constexpr float kNear = 1.0e-3f;
        if (a.w >= kNear && b.w >= kNear && c.w >= kNear)
        {
            emit(out, a, zScale), emit(out, b, zScale), emit(out, c, zScale);
            return;
        }
        Clip in[3] = {a, b, c}, poly[4];
        int n = 0;
        for (int i = 0; i < 3; ++i)
        {
            const Clip &p = in[i], &q = in[(i + 1) % 3];
            if (p.w >= kNear)
                poly[n++] = p;
            if ((p.w >= kNear) != (q.w >= kNear))
                poly[n++] = lerp(p, q, (kNear - p.w) / (q.w - p.w));
        }
        for (int i = 1; i + 1 < n; ++i)
            emit(out, poly[0], zScale), emit(out, poly[i], zScale), emit(out, poly[i + 1], zScale);
    }

    void drawBatch(Vu1StreamAdapter &adapter)
    {
        const uint32_t *data = adapter.data();
        const auto word = [data](uint32_t qword, int i) { return data[((qword & 0x3ffu) * 4u) + i]; };
        uint32_t at = adapter.top();

        auto writes = std::make_shared<std::vector<std::pair<uint8_t, uint64_t>>>();
        uint32_t nloop = word(at, 0) & 0x7fffu;
        if (word(at, 3) & 0xffffu) // an A+D tag first (the program copies its NLOOP register writes before the PRIM tag)
        {
            for (uint32_t i = 0; i < nloop; ++i)
            {
                const uint32_t q = at + 1u + i;
                writes->emplace_back(static_cast<uint8_t>(word(q, 2) & 0xffu), word(q, 0) | (static_cast<uint64_t>(word(q, 1)) << 32));
            }
            at += 1u + nloop;
            nloop = word(at, 0) & 0x7fffu;
        }
        const uint64_t tag = word(at, 0) | (static_cast<uint64_t>(word(at, 1)) << 32);
        const uint32_t prim = ((tag >> 46) & 1u) ? static_cast<uint32_t>((tag >> 47) & 0x7ffu) : 0x5cu; // PRE, else a fogged textured strip
        ++at;

        ps2x::native::Material material;
        material.fromGsContext = true;
        material.textured = (prim >> 4) & 1u;
        material.fog = (prim >> 5) & 1u;
        material.alphaBlend = (prim >> 6) & 1u;
        material.context = static_cast<uint8_t>((prim >> 9) & 1u);
        if (!writes->empty())
            material.gsWrites = std::move(writes);

        static thread_local std::vector<Clip> s_verts;
        static thread_local std::vector<ps2x::native::Vertex> s_out;
        s_verts.clear();
        s_out.clear();
        const Registers &r = g_regs;
        for (uint32_t i = 0; i < nloop; ++i, at += 2u)
        {
            const float x = asFloat(word(at, 0)), y = asFloat(word(at, 1)), z = asFloat(word(at, 2));
            float v[4];
            for (int k = 0; k < 4; ++k)
                v[k] = r.m[0][k] * x + r.m[1][k] * y + r.m[2][k] * z + r.m[3][k];
            Clip c;
            c.x = v[0], c.y = v[1], c.z = v[2], c.w = v[3];
            c.s = asFloat(word(at + 1u, 0));
            c.t = asFloat(word(at + 1u, 1));
            const uint32_t index = word(at + 1u, 2) & 0xffffu;
            c.adc = (index & 0x8000u) != 0u;
            const uint32_t colour = (136u + index) & 0x3ffu;
            c.r = static_cast<float>(word(colour, 0) & 0xffu);
            c.g = static_cast<float>(word(colour, 1) & 0xffu);
            c.b = static_cast<float>(word(colour, 2) & 0xffu);
            c.a = static_cast<float>(word(colour, 3) & 0xffu);
            s_verts.push_back(c);
        }

        const float zScale = ps2x::native::Renderer::instance().gsZScale();
        const uint32_t type = prim & 7u;
        for (size_t i = 2; i < s_verts.size(); ++i)
        {
            if (type == 3u && i % 3u != 2u)
                continue; // a list: every third vertex ends a triangle
            if (s_verts[i].adc)
                continue;
            const Clip &a = type == 5u ? s_verts[0] : type == 3u ? s_verts[i - 2] : s_verts[i - 2];
            triangle(s_out, a, s_verts[i - 1], s_verts[i], zScale);
        }
        if (!s_out.empty())
            ps2x::native::Renderer::instance().draw(material, kScreenSpace, s_out.data(), static_cast<uint32_t>(s_out.size()));
    }
}

namespace tta
{
    bool vu1PortProgram(const uint8_t *code, uint32_t startPc)
    {
        if (startPc == kStrip)
            return fnv(code + kStrip, 0x830u - kStrip) == kStripHash;
        if (startPc == kDirect)
            return fnv(code + kDirect, 0x460u - kDirect) == kDirectHash;
        return false;
    }

    void vu1PortDraw(Vu1StreamAdapter &adapter, uint32_t startPc, bool resumed)
    {
        if (!resumed) // the MSCAL: its registers, then it ends (E bit) before any vertex
        {
            g_running = startPc;
            loadRegisters(adapter.data());
            return;
        }
        if (g_running == kStrip || g_running == kDirect)
            drawBatch(adapter);
    }
}
#endif
