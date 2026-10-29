#pragma once

// QA kit: the pixels of the editor's QA
// screenshots (editor/qa_shots.h). A back buffer copy (RGBA8, BGRA8,
// RGB10A2 or RGBA16F, read back from the GPU) becomes 24-bit BGR rows, the
// layout the JPEG encoder takes, at its own size or box-reduced to a
// smaller one. Runs on a worker thread, never on the render thread.
//
// Pure: unit tested in ReplayCoreTests.

#include <cstdint>
#include <cstring>
#include <vector>

namespace replay::qa::image
{
    enum class Pixel : std::uint8_t { Rgba8, Bgra8, Rgb10, Half };

    inline std::size_t BytesPerPixel(Pixel kind) { return kind == Pixel::Half ? 8 : 4; }

    // The small size of a ring picture: `height` rows, the width that keeps
    // the aspect (even). 3840x2160 -> 854x480.
    inline void RingSize(std::uint32_t width, std::uint32_t height, std::uint32_t targetHeight, std::uint32_t& outWidth,
        std::uint32_t& outHeight)
    {
        if (width == 0 || height == 0)
        {
            outWidth = outHeight = 0;
            return;
        }
        if (height <= targetHeight)
        {
            outWidth = width & ~1u;
            outHeight = height & ~1u;
            return;
        }
        outHeight = targetHeight;
        outWidth = static_cast<std::uint32_t>((static_cast<std::uint64_t>(width) * targetHeight + height / 2) / height) & ~1u;
    }

    inline float HalfToFloat(std::uint16_t h)
    {
        const std::uint32_t sign = (h & 0x8000u) << 16;
        const std::uint32_t exponent = (h >> 10) & 0x1F;
        const std::uint32_t mantissa = h & 0x3FF;
        std::uint32_t bits = 0;
        if (exponent == 0)
            bits = sign;
        else if (exponent == 31)
            bits = sign | 0x7F800000u | (mantissa << 13);
        else
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // One pixel to 8-bit B, G, R (a half float picture is scRGB, linear:
    // clipped and taken as is; good enough to see what is on screen).
    inline void ReadBgr(const std::uint8_t* p, Pixel kind, std::uint32_t bgr[3])
    {
        switch (kind)
        {
        case Pixel::Rgba8:
            bgr[0] = p[2];
            bgr[1] = p[1];
            bgr[2] = p[0];
            break;
        case Pixel::Bgra8:
            bgr[0] = p[0];
            bgr[1] = p[1];
            bgr[2] = p[2];
            break;
        case Pixel::Rgb10:
        {
            std::uint32_t v = 0;
            std::memcpy(&v, p, 4);
            bgr[2] = ((v & 0x3FF) * 255 + 511) / 1023;
            bgr[1] = (((v >> 10) & 0x3FF) * 255 + 511) / 1023;
            bgr[0] = (((v >> 20) & 0x3FF) * 255 + 511) / 1023;
            break;
        }
        case Pixel::Half:
        {
            std::uint16_t h[3];
            std::memcpy(h, p, sizeof(h));
            for (int c = 0; c < 3; ++c)
            {
                float f = HalfToFloat(h[c]);
                f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
                bgr[2 - c] = static_cast<std::uint32_t>(f * 255.0f + 0.5f);
            }
            break;
        }
        }
    }

    // `raw` (rows of `rowPitch` bytes) reduced to outWidth x outHeight BGR
    // (rows of outWidth * 3 bytes, top first), each output pixel the mean of
    // a 2x2 grid of source samples inside its box. Same size: a plain
    // conversion.
    inline void ToBgr(const std::uint8_t* raw, std::size_t rowPitch, std::uint32_t width, std::uint32_t height, Pixel kind,
        std::uint32_t outWidth, std::uint32_t outHeight, std::vector<std::uint8_t>& out)
    {
        out.assign(static_cast<std::size_t>(outWidth) * outHeight * 3, 0);
        if (!raw || !width || !height || !outWidth || !outHeight)
            return;
        const std::size_t bpp = BytesPerPixel(kind);
        if (outWidth == width && outHeight == height)
        {
            for (std::uint32_t y = 0; y < height; ++y)
            {
                const std::uint8_t* row = raw + y * rowPitch;
                std::uint8_t* d = out.data() + static_cast<std::size_t>(y) * outWidth * 3;
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    std::uint32_t bgr[3];
                    ReadBgr(row + x * bpp, kind, bgr);
                    d[x * 3 + 0] = static_cast<std::uint8_t>(bgr[0]);
                    d[x * 3 + 1] = static_cast<std::uint8_t>(bgr[1]);
                    d[x * 3 + 2] = static_cast<std::uint8_t>(bgr[2]);
                }
            }
            return;
        }
        // Source columns of each output column's two taps (quarter and three
        // quarters of its box).
        std::vector<std::uint32_t> taps(static_cast<std::size_t>(outWidth) * 2);
        for (std::uint32_t x = 0; x < outWidth; ++x)
        {
            for (int t = 0; t < 2; ++t)
            {
                const std::uint64_t sx = (static_cast<std::uint64_t>(x) * 4 + 1 + 2 * t) * width / (static_cast<std::uint64_t>(outWidth) * 4);
                taps[x * 2 + t] = static_cast<std::uint32_t>(sx < width ? sx : width - 1);
            }
        }
        for (std::uint32_t y = 0; y < outHeight; ++y)
        {
            const std::uint8_t* rows[2];
            for (int t = 0; t < 2; ++t)
            {
                std::uint64_t sy = (static_cast<std::uint64_t>(y) * 4 + 1 + 2 * t) * height / (static_cast<std::uint64_t>(outHeight) * 4);
                sy = sy < height ? sy : height - 1;
                rows[t] = raw + sy * rowPitch;
            }
            std::uint8_t* d = out.data() + static_cast<std::size_t>(y) * outWidth * 3;
            for (std::uint32_t x = 0; x < outWidth; ++x)
            {
                std::uint32_t sum[3] = {};
                for (int ty = 0; ty < 2; ++ty)
                {
                    for (int tx = 0; tx < 2; ++tx)
                    {
                        std::uint32_t bgr[3];
                        ReadBgr(rows[ty] + taps[x * 2 + tx] * bpp, kind, bgr);
                        sum[0] += bgr[0];
                        sum[1] += bgr[1];
                        sum[2] += bgr[2];
                    }
                }
                d[x * 3 + 0] = static_cast<std::uint8_t>((sum[0] + 2) / 4);
                d[x * 3 + 1] = static_cast<std::uint8_t>((sum[1] + 2) / 4);
                d[x * 3 + 2] = static_cast<std::uint8_t>((sum[2] + 2) / 4);
            }
        }
    }
}
