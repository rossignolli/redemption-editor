// Render export: the GPU side of the export.
// Compute shaders run by the editor add-on through reshade::api (no ReShade
// FX effect, no per frame CPU work on pixels):
//
//   CSAccumulate  motion blur: sum += linear(picture) * weight, in a float4
//                 storage buffer (the first sample of a frame clears it).
//   CSResolve     the output frame: from the sum (motion blur) or straight
//                 from the picture (no blur), optional letterbox and grain,
//                 encoded and packed into a byte buffer in the encoder's
//                 layout (NV12, P010, RGBA8 or X2BGR10), so the readback is
//                 a plain buffer copy and ffmpeg takes the frame as is
//                 (no swscale on the CPU for the NVENC presets).
//
// Compiled offline to SPIR-V (Vulkan, the game's API) by
// tools/render_bench/compile_shaders.sh into export_convert_spirv.h; the
// offline bench (tools/render_bench) runs the same bytecode on the GPU and
// compares it with a CPU reference (render_convert_ref.h).
//
// Bindings (set 0): 0 the picture (sampled image, read with Load: no
// sampler), 1 the sum (RGBA32F storage image, one texel per pixel), 2 the
// output bytes as an R32_UINT storage image kOutputRowWords texels wide
// (word i at (i % kOutputRowWords, i / kOutputRowWords)), so a plain
// image-to-buffer copy gives the bytes in order. Push constants:
// ExportConstants below (48 bytes).
//
// Storage images, not storage buffers (found in game): in
// ReShade 6.6.2 on Vulkan a buffer with unordered_access usage also gets
// SHADER_DEVICE_ADDRESS usage, and its VMA allocator only has buffer device
// addresses when ray tracing is on; in RDR2 the buffer creation failed
// (RENDER_EXPORT,phase=fail,detail=gpu_buffers). Images have no such flag.

#ifdef VULKAN // predefined by glslang for SPIR-V
#define BINDING(b) [[vk::binding(b, 0)]]
#define IMAGE_FORMAT(f) f
#else
#define BINDING(b)
#define IMAGE_FORMAT(f)
#endif

struct ExportConstants
{
    uint width;
    uint height;
    uint format;     // 0 NV12, 1 P010, 2 RGBA8, 3 X2BGR10 (R in the low bits)
    uint flags;      // 1 first sample (clear), 2 resolve from the sum, 4 dither, 8 BT.2020 matrix
    float weight;    // accumulation weight of this sample
    uint transfer;   // 0 sRGB, 1 PQ (HDR10), 2 linear (scRGB)
    uint barTop;     // letterbox: black rows at the top
    uint barBottom;  // and at the bottom
    float grain;     // film grain amplitude in code values (0 = off)
    uint seed;       // grain seed (the frame index)
    uint pad0;
    uint pad1;
};

#ifdef VULKAN
[[vk::push_constant]] ConstantBuffer<ExportConstants> pc;
#else
cbuffer Constants : register(b0)
{
    ExportConstants pc;
};
#endif

BINDING(0) Texture2D<float4> Picture : register(t0);
BINDING(1) IMAGE_FORMAT([[spv::format_rgba32f]]) RWTexture2D<float4> Sum : register(u0);
BINDING(2) IMAGE_FORMAT([[spv::format_r32ui]]) RWTexture2D<uint> Output : register(u1);

// Output texels per row (convert_ref.h kOutputRowWords).
static const uint kOutputRowShift = 12;
static const uint kOutputRowMask = (1u << kOutputRowShift) - 1u;

// The output as a byte address: offsets are always multiples of 4.
void Store(uint byteOffset, uint word)
{
    const uint i = byteOffset >> 2;
    Output[uint2(i & kOutputRowMask, i >> kOutputRowShift)] = word;
}

void Store2(uint byteOffset, uint2 words)
{
    Store(byteOffset, words.x);
    Store(byteOffset + 4u, words.y);
}

static const uint kFlagFirst = 1;
static const uint kFlagFromSum = 2;
static const uint kFlagDither = 4;
static const uint kFlagBt2020 = 8;

float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r, c.g <= 0.04045 ? lo.g : hi.g, c.b <= 0.04045 ? lo.b : hi.b);
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r, c.g <= 0.0031308 ? lo.g : hi.g, c.b <= 0.0031308 ? lo.b : hi.b);
}

float3 PqToLinear(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float3 LinearToPq(float3 l)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(max(l, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float3 Decode(float3 c)
{
    if (pc.transfer == 1)
        return PqToLinear(c);
    if (pc.transfer == 2)
        return c;
    return SrgbToLinear(c);
}

float3 Encode(float3 c)
{
    if (pc.transfer == 1)
        return LinearToPq(c);
    if (pc.transfer == 2)
        return saturate(c);
    return LinearToSrgb(c);
}

// ---------------------------------------------------------------------
// Accumulation (one thread per pixel)
// ---------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSAccumulate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    const float3 c = Decode(Picture.Load(int3(id.xy, 0)).rgb);
    const float4 add = float4(c * pc.weight, pc.weight);
    Sum[id.xy] = (pc.flags & kFlagFirst) != 0 ? add : Sum[id.xy] + add;
}

// ---------------------------------------------------------------------
// Resolve and pack (one thread per 4 x 2 pixel block)
// ---------------------------------------------------------------------

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The encoded (display) colour of pixel p, letterbox and grain applied.
float3 Pixel(uint2 p)
{
    if (p.y < pc.barTop || p.y >= pc.height - pc.barBottom)
        return float3(0.0, 0.0, 0.0);
    float3 c;
    if ((pc.flags & kFlagFromSum) != 0)
    {
        const float4 s = Sum[p];
        // s.a is the total weight (1 for a complete frame): dividing keeps
        // a frame with a missing sample exposed right.
        c = Encode(s.rgb / max(s.a, 1e-6));
    }
    else
    {
        c = Picture.Load(int3(p, 0)).rgb;
    }
    if (pc.grain > 0.0)
    {
        const uint h = Hash(p.x * 1973u + p.y * 9277u + pc.seed * 26699u);
        const float n = (float(h & 0xFFFFu) + float(h >> 16)) / 65535.0 - 1.0; // triangular, -1..1
        c = saturate(c + n * pc.grain);
    }
    return c;
}

// Ordered dither offset in code values (-0.5..0.5), 4 x 4 Bayer.
float Dither(uint2 p)
{
    if ((pc.flags & kFlagDither) == 0)
        return 0.0;
    const uint bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (float(bayer[(p.y & 3u) * 4u + (p.x & 3u)]) + 0.5) / 16.0 - 0.5;
}

// Y'CbCr, limited (TV) range, BT.709 or BT.2020 (non constant luminance).
// Returns Y in 0..1 of the code range scale (16..235 / 64..940 mapped by
// the caller) as normalised values: y in [0,1], cb/cr in [-0.5, 0.5].
float3 ToYcc(float3 c)
{
    const bool bt2020 = (pc.flags & kFlagBt2020) != 0;
    const float kr = bt2020 ? 0.2627 : 0.2126;
    const float kb = bt2020 ? 0.0593 : 0.0722;
    const float y = kr * c.r + (1.0 - kr - kb) * c.g + kb * c.b;
    return float3(y, (c.b - y) / (2.0 * (1.0 - kb)), (c.r - y) / (2.0 * (1.0 - kr)));
}

uint Code8Y(float y, float d) { return (uint)clamp(floor(16.0 + 219.0 * y + 0.5 + d), 0.0, 255.0); }
uint Code8C(float c, float d) { return (uint)clamp(floor(128.0 + 224.0 * c + 0.5 + d), 0.0, 255.0); }
uint Code10Y(float y) { return (uint)clamp(floor(64.0 + 876.0 * y + 0.5), 0.0, 1023.0); }
uint Code10C(float c) { return (uint)clamp(floor(512.0 + 896.0 * c + 0.5), 0.0, 1023.0); }

[numthreads(8, 8, 1)]
void CSResolve(uint3 id : SV_DispatchThreadID)
{
    const uint x0 = id.x * 4u;
    const uint y0 = id.y * 2u;
    if (x0 >= pc.width || y0 >= pc.height)
        return;
    float3 c[2][4];
    [unroll] for (uint r = 0; r < 2; ++r)
        [unroll] for (uint k = 0; k < 4; ++k)
            c[r][k] = Pixel(uint2(x0 + k, y0 + r));

    const uint w = pc.width;
    const uint h = pc.height;
    if (pc.format == 2 || pc.format == 3)
    {
        // Packed RGB: 4 bytes per pixel, rows tight.
        [unroll] for (uint r = 0; r < 2; ++r)
            [unroll] for (uint k = 0; k < 4; ++k)
            {
                const float3 v = c[r][k];
                uint word;
                if (pc.format == 2)
                {
                    const float d = Dither(uint2(x0 + k, y0 + r));
                    const uint3 b = (uint3)clamp(floor(v * 255.0 + 0.5 + d), 0.0, 255.0);
                    word = b.r | (b.g << 8) | (b.b << 16) | (0xFFu << 24);
                }
                else
                {
                    const uint3 b = (uint3)clamp(floor(v * 1023.0 + 0.5), 0.0, 1023.0);
                    word = b.r | (b.g << 10) | (b.b << 20) | (3u << 30);
                }
                Store(((y0 + r) * w + x0 + k) * 4u, word);
            }
        return;
    }

    float3 ycc[2][4];
    [unroll] for (uint r2 = 0; r2 < 2; ++r2)
        [unroll] for (uint k2 = 0; k2 < 4; ++k2)
            ycc[r2][k2] = ToYcc(c[r2][k2]);
    // Chroma of each 2 x 2 block from the average of its encoded colour.
    const float3 a0 = ToYcc((c[0][0] + c[0][1] + c[1][0] + c[1][1]) * 0.25);
    const float3 a1 = ToYcc((c[0][2] + c[0][3] + c[1][2] + c[1][3]) * 0.25);

    if (pc.format == 0)
    {
        // NV12: Y plane (w x h bytes), then interleaved CbCr (w x h/2 bytes).
        [unroll] for (uint r3 = 0; r3 < 2; ++r3)
        {
            uint word = 0;
            [unroll] for (uint k3 = 0; k3 < 4; ++k3)
                word |= Code8Y(ycc[r3][k3].x, Dither(uint2(x0 + k3, y0 + r3))) << (8u * k3);
            Store((y0 + r3) * w + x0, word);
        }
        const float dc = Dither(uint2(x0 >> 1, y0 >> 1));
        const uint uv = Code8C(a0.y, dc) | (Code8C(a0.z, dc) << 8) | (Code8C(a1.y, dc) << 16) | (Code8C(a1.z, dc) << 24);
        Store(w * h + (y0 >> 1) * w + x0, uv);
        return;
    }

    // P010: 16 bit little endian words, the 10 bit value in the high bits.
    [unroll] for (uint r4 = 0; r4 < 2; ++r4)
    {
        const uint base = ((y0 + r4) * w + x0) * 2u;
        const uint y0v = Code10Y(ycc[r4][0].x) << 6;
        const uint y1v = Code10Y(ycc[r4][1].x) << 6;
        const uint y2v = Code10Y(ycc[r4][2].x) << 6;
        const uint y3v = Code10Y(ycc[r4][3].x) << 6;
        Store2(base, uint2(y0v | (y1v << 16), y2v | (y3v << 16)));
    }
    const uint cbase = w * h * 2u + ((y0 >> 1) * w + x0) * 2u;
    const uint u0 = Code10C(a0.y) << 6, v0 = Code10C(a0.z) << 6;
    const uint u1 = Code10C(a1.y) << 6, v1 = Code10C(a1.z) << 6;
    Store2(cbase, uint2(u0 | (v0 << 16), u1 | (v1 << 16)));
}
