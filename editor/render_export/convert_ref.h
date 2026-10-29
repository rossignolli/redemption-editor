#pragma once

// Render export: CPU reference of the GPU conversion in
// shaders/export_convert.hlsl. Same maths, same packing:
//   - ReplayCoreTests checks the packing and the colour maths on tiny
//     pictures (tests/export_schedule_tests.cpp);
//   - the offline GPU bench (tools/render_bench) runs the real SPIR-V on the
//     GPU and compares every byte with this reference (tolerance 1 code
//     value: float rounding differs between the GPU and the CPU).
// It is also the CPU fallback when the compute path cannot be created.
// Pure (no Windows, no ReShade).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace render_export
{
    // Encoder pixel layouts of the resolve (ExportConstants::format).
    enum class OutputLayout : std::uint32_t
    {
        Nv12 = 0,   // 8 bit 4:2:0, Y plane then interleaved CbCr (NVENC native)
        P010 = 1,   // 10 bit 4:2:0 in 16 bit words, value in the high bits
        Rgba8 = 2,  // packed R8G8B8A8 (FFV1, PNG, ProRes input)
        X2Bgr10 = 3 // packed 10:10:10:2, R in the low bits (ffmpeg x2bgr10le)
    };

    constexpr std::uint32_t kFlagFirst = 1;
    constexpr std::uint32_t kFlagFromSum = 2;
    constexpr std::uint32_t kFlagDither = 4;
    constexpr std::uint32_t kFlagBt2020 = 8;

    // Push constants of both compute shaders (48 bytes, std430 / cbuffer
    // compatible: 12 scalars).
    struct ExportConstants
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t format = 0;
        std::uint32_t flags = 0;
        float weight = 1.0f;
        std::uint32_t transfer = 0; // 0 sRGB, 1 PQ, 2 linear
        std::uint32_t barTop = 0;
        std::uint32_t barBottom = 0;
        float grain = 0.0f;
        std::uint32_t seed = 0;
        std::uint32_t pad0 = 0;
        std::uint32_t pad1 = 0;
    };
    static_assert(sizeof(ExportConstants) == 48, "push constant block is 48 bytes");

    // Bytes of one output frame.
    constexpr std::uint64_t OutputBytes(OutputLayout layout, std::uint32_t width, std::uint32_t height) noexcept
    {
        const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
        switch (layout)
        {
        case OutputLayout::Nv12: return pixels * 3 / 2;
        case OutputLayout::P010: return pixels * 3;
        case OutputLayout::Rgba8:
        case OutputLayout::X2Bgr10: return pixels * 4;
        }
        return pixels * 4;
    }

    // The GPU writes the output bytes into an R32_UINT image this many texels
    // wide (export_convert.hlsl kOutputRowShift); the readback copies the
    // whole image, so the staging buffers hold whole rows.
    inline constexpr std::uint32_t kOutputRowWords = 4096;

    constexpr std::uint32_t OutputImageRows(OutputLayout layout, std::uint32_t width, std::uint32_t height) noexcept
    {
        const std::uint64_t words = (OutputBytes(layout, width, height) + 3) / 4;
        return static_cast<std::uint32_t>((words + kOutputRowWords - 1) / kOutputRowWords);
    }

    constexpr std::uint64_t OutputImageBytes(OutputLayout layout, std::uint32_t width, std::uint32_t height) noexcept
    {
        return static_cast<std::uint64_t>(OutputImageRows(layout, width, height)) * kOutputRowWords * 4;
    }

    // The resolve works on 4 x 2 blocks: width a multiple of 4, height of 2.
    constexpr bool SizeSupported(std::uint32_t width, std::uint32_t height) noexcept
    {
        return width >= 4 && height >= 2 && width % 4 == 0 && height % 2 == 0 && width <= 16384 && height <= 16384;
    }

    // Letterbox bars (rows) for a target aspect ratio (2.39 for scope);
    // 0 when the picture is already wider. Even, so 4:2:0 rows stay whole.
    inline void LetterboxRows(std::uint32_t width, std::uint32_t height, float aspect, std::uint32_t& top, std::uint32_t& bottom) noexcept
    {
        top = bottom = 0;
        if (aspect <= 0.0f || width == 0 || height == 0)
            return;
        const double visible = static_cast<double>(width) / aspect;
        if (visible >= height)
            return;
        std::uint32_t bars = static_cast<std::uint32_t>(height - visible + 0.5);
        bars &= ~3u; // two even bars
        top = bars / 2;
        bottom = bars - top;
    }

    namespace ref
    {
        inline float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow(std::max((c + 0.055f) / 1.055f, 0.0f), 2.4f); }
        inline float LinearToSrgb(float c)
        {
            c = std::clamp(c, 0.0f, 1.0f);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }
        inline float PqToLinear(float e)
        {
            const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
            const float p = std::pow(std::clamp(e, 0.0f, 1.0f), 1.0f / m2);
            return std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
        }
        inline float LinearToPq(float l)
        {
            const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
            const float p = std::pow(std::max(l, 0.0f), m1);
            return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
        }
        inline float Decode(float c, std::uint32_t transfer) { return transfer == 1 ? PqToLinear(c) : transfer == 2 ? c : SrgbToLinear(c); }
        inline float Encode(float c, std::uint32_t transfer)
        {
            return transfer == 1 ? LinearToPq(c) : transfer == 2 ? std::clamp(c, 0.0f, 1.0f) : LinearToSrgb(c);
        }
        inline std::uint32_t Hash(std::uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            x ^= x >> 16;
            return x;
        }
        inline float Dither(const ExportConstants& pc, std::uint32_t x, std::uint32_t y)
        {
            if ((pc.flags & kFlagDither) == 0)
                return 0.0f;
            static const std::uint32_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            return (static_cast<float>(bayer[(y & 3u) * 4u + (x & 3u)]) + 0.5f) / 16.0f - 0.5f;
        }
        struct Ycc { float y, cb, cr; };
        inline Ycc ToYcc(const ExportConstants& pc, const float* c)
        {
            const bool bt2020 = (pc.flags & kFlagBt2020) != 0;
            const float kr = bt2020 ? 0.2627f : 0.2126f;
            const float kb = bt2020 ? 0.0593f : 0.0722f;
            const float y = kr * c[0] + (1.0f - kr - kb) * c[1] + kb * c[2];
            return {y, (c[2] - y) / (2.0f * (1.0f - kb)), (c[0] - y) / (2.0f * (1.0f - kr))};
        }
        inline std::uint32_t Clamp8(float v) { return static_cast<std::uint32_t>(std::clamp(std::floor(v), 0.0f, 255.0f)); }
        inline std::uint32_t Clamp10(float v) { return static_cast<std::uint32_t>(std::clamp(std::floor(v), 0.0f, 1023.0f)); }
    }

    // A picture as the GPU sees it: RGBA float, encoded (what Load returns
    // from a UNORM view of the back buffer).
    struct RefPicture
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<float> rgba; // width * height * 4
        const float* At(std::uint32_t x, std::uint32_t y) const { return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4; }
    };

    // CSAccumulate.
    inline void RefAccumulate(const ExportConstants& pc, const RefPicture& picture, std::vector<float>& sum)
    {
        const std::size_t pixels = static_cast<std::size_t>(pc.width) * pc.height;
        if (sum.size() != pixels * 4)
            sum.assign(pixels * 4, 0.0f);
        for (std::size_t i = 0; i < pixels; ++i)
        {
            const float* c = picture.rgba.data() + i * 4;
            float* s = sum.data() + i * 4;
            const bool first = (pc.flags & kFlagFirst) != 0;
            for (int k = 0; k < 3; ++k)
                s[k] = (first ? 0.0f : s[k]) + ref::Decode(c[k], pc.transfer) * pc.weight;
            s[3] = (first ? 0.0f : s[3]) + pc.weight;
        }
    }

    // CSResolve: packs the whole frame into `out` (OutputBytes long).
    inline void RefResolve(const ExportConstants& pc, const RefPicture& picture, const std::vector<float>& sum, std::vector<std::uint8_t>& out)
    {
        const std::uint32_t w = pc.width;
        const std::uint32_t h = pc.height;
        const auto layout = static_cast<OutputLayout>(pc.format);
        out.assign(static_cast<std::size_t>(OutputBytes(layout, w, h)), 0);
        auto pixel = [&](std::uint32_t x, std::uint32_t y, float* c) {
            if (y < pc.barTop || y >= h - pc.barBottom)
            {
                c[0] = c[1] = c[2] = 0.0f;
                return;
            }
            if ((pc.flags & kFlagFromSum) != 0)
            {
                const float* s = sum.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                const float a = std::max(s[3], 1e-6f);
                for (int k = 0; k < 3; ++k)
                    c[k] = ref::Encode(s[k] / a, pc.transfer);
            }
            else
            {
                const float* p = picture.At(x, y);
                c[0] = p[0];
                c[1] = p[1];
                c[2] = p[2];
            }
            if (pc.grain > 0.0f)
            {
                const std::uint32_t hv = ref::Hash(x * 1973u + y * 9277u + pc.seed * 26699u);
                const float n = (static_cast<float>(hv & 0xFFFFu) + static_cast<float>(hv >> 16)) / 65535.0f - 1.0f;
                for (int k = 0; k < 3; ++k)
                    c[k] = std::clamp(c[k] + n * pc.grain, 0.0f, 1.0f);
            }
        };
        auto store = [&](std::size_t offset, std::uint32_t word) { std::memcpy(out.data() + offset, &word, 4); };
        for (std::uint32_t y0 = 0; y0 < h; y0 += 2)
            for (std::uint32_t x0 = 0; x0 < w; x0 += 4)
            {
                float c[2][4][3];
                for (std::uint32_t r = 0; r < 2; ++r)
                    for (std::uint32_t k = 0; k < 4; ++k)
                        pixel(x0 + k, y0 + r, c[r][k]);
                if (layout == OutputLayout::Rgba8 || layout == OutputLayout::X2Bgr10)
                {
                    for (std::uint32_t r = 0; r < 2; ++r)
                        for (std::uint32_t k = 0; k < 4; ++k)
                        {
                            const float* v = c[r][k];
                            std::uint32_t word;
                            if (layout == OutputLayout::Rgba8)
                            {
                                const float d = ref::Dither(pc, x0 + k, y0 + r);
                                word = ref::Clamp8(v[0] * 255.0f + 0.5f + d) | (ref::Clamp8(v[1] * 255.0f + 0.5f + d) << 8) |
                                    (ref::Clamp8(v[2] * 255.0f + 0.5f + d) << 16) | (0xFFu << 24);
                            }
                            else
                            {
                                word = ref::Clamp10(v[0] * 1023.0f + 0.5f) | (ref::Clamp10(v[1] * 1023.0f + 0.5f) << 10) |
                                    (ref::Clamp10(v[2] * 1023.0f + 0.5f) << 20) | (3u << 30);
                            }
                            store((static_cast<std::size_t>(y0 + r) * w + x0 + k) * 4, word);
                        }
                    continue;
                }
                ref::Ycc ycc[2][4];
                for (std::uint32_t r = 0; r < 2; ++r)
                    for (std::uint32_t k = 0; k < 4; ++k)
                        ycc[r][k] = ref::ToYcc(pc, c[r][k]);
                float avg0[3], avg1[3];
                for (int k = 0; k < 3; ++k)
                {
                    avg0[k] = (c[0][0][k] + c[0][1][k] + c[1][0][k] + c[1][1][k]) * 0.25f;
                    avg1[k] = (c[0][2][k] + c[0][3][k] + c[1][2][k] + c[1][3][k]) * 0.25f;
                }
                const ref::Ycc a0 = ref::ToYcc(pc, avg0);
                const ref::Ycc a1 = ref::ToYcc(pc, avg1);
                if (layout == OutputLayout::Nv12)
                {
                    for (std::uint32_t r = 0; r < 2; ++r)
                    {
                        std::uint32_t word = 0;
                        for (std::uint32_t k = 0; k < 4; ++k)
                            word |= ref::Clamp8(16.0f + 219.0f * ycc[r][k].y + 0.5f + ref::Dither(pc, x0 + k, y0 + r)) << (8u * k);
                        store(static_cast<std::size_t>(y0 + r) * w + x0, word);
                    }
                    const float dc = ref::Dither(pc, x0 >> 1, y0 >> 1);
                    auto c8 = [&](float v) { return ref::Clamp8(128.0f + 224.0f * v + 0.5f + dc); };
                    const std::uint32_t uv = c8(a0.cb) | (c8(a0.cr) << 8) | (c8(a1.cb) << 16) | (c8(a1.cr) << 24);
                    store(static_cast<std::size_t>(w) * h + static_cast<std::size_t>(y0 >> 1) * w + x0, uv);
                    continue;
                }
                auto y10 = [](float v) { return ref::Clamp10(64.0f + 876.0f * v + 0.5f) << 6; };
                auto c10 = [](float v) { return ref::Clamp10(512.0f + 896.0f * v + 0.5f) << 6; };
                for (std::uint32_t r = 0; r < 2; ++r)
                {
                    const std::size_t base = (static_cast<std::size_t>(y0 + r) * w + x0) * 2;
                    store(base, y10(ycc[r][0].y) | (y10(ycc[r][1].y) << 16));
                    store(base + 4, y10(ycc[r][2].y) | (y10(ycc[r][3].y) << 16));
                }
                const std::size_t cbase = static_cast<std::size_t>(w) * h * 2 + (static_cast<std::size_t>(y0 >> 1) * w + x0) * 2;
                store(cbase, c10(a0.cb) | (c10(a0.cr) << 16));
                store(cbase + 4, c10(a1.cb) | (c10(a1.cr) << 16));
            }
    }

    // Largest per byte difference between two frames (the GPU check).
    inline int MaxByteDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b)
    {
        if (a.size() != b.size())
            return 256;
        int worst = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
        return worst;
    }
}
