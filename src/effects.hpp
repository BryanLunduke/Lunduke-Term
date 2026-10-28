// SPDX-License-Identifier: GPL-3.0-or-later
// Lunduke Terminal: CRT effects description shared by the GL (main) and CPU (fallback) back-ends.
//
// The maths follows cool-retro-term 1.2.0 (app/qml/ShaderTerminal.qml, BurnInEffect.qml,
// ShaderLibrary.qml), re-implemented from scratch for a single source of truth:
//   1. bloom source: 2x2 box downsample + separable Gaussian (transparent border)
//   2. burn-in: feedback buffer, new = max(old - decay, current) in source space (half
//      resolution by default, like CRT's burnInQuality 0.5; fed from the bloom downsample)
//   3. static pass: barrel warp, chroma/tint to phosphor colour, bloom add, brightness
//   4. dynamic pass: horizontal sync, jitter, static noise, glowing line, burn-in combine,
//      scanline rasterization, flicker, ambient light, outside-screen mask
// All values are gamma-encoded (sRGB-as-stored) floats in [0,1], as in CRT (no linearisation).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lterm {

struct Rgb { float r, g, b; };

struct EffectParams {
    // Profile values (cool-retro-term 1.2.0 "Default Amber"), raw 0..1 slider values.
    Rgb fontColor{1.0f, 129.0f / 255.0f, 0.0f};    // #ff8100
    Rgb backgroundColor{0.0f, 0.0f, 0.0f};
    float bloom = 0.5538f;
    float burnIn = 0.2517f;
    float chromaColor = 0.2483f;
    float brightness = 0.5f;
    float ambientLight = 0.2f;
    float flickering = 0.1f;
    float glowingLine = 0.2f;
    float horizontalSync = 0.08f;
    float jitter = 0.1997f;
    float screenCurvature = 0.3f;
    float staticNoise = 0.1198f;
    // Rasterization: 0 none, 1 scanlines (CRT mode 1). Amber uses 0; the spike defaults to 1
    // so the scanline cost is measured.
    int rasterization = 1;
    // Output pixels per virtual scanline (CRT derives this from font scaling).
    float scanlinePeriod = 3.0f;
    float rasterizationIntensity = 1.0f;
    // Global quality: bloom radius at half resolution (CRT: lint(16,64,bloomQuality)=40 full res).
    int bloomRadiusHalfRes = 20;
    // M1: keep the burn-in feedback buffer at half resolution (4x less memory and bandwidth).
    // The M0 golden comparison used full resolution; both are kept so the M0 numbers can be
    // reproduced.
    bool burnInHalfRes = true;

    // True when the picture changes on its own (noise, flicker, jitter, sync, glowing line),
    // i.e. the view needs an effects tick even when the terminal content is unchanged.
    bool animated() const {
        return staticNoise > 0 || flickering > 0 || jitter > 0 || horizontalSync > 0 || glowingLine > 0;
    }
    // Time after a content change until the burn-in ghost has fully faded (ms), 0 without burn-in.
    float burnInFadeMs() const;
};

// M2: effects quality. Software rasterisers (Mesa llvmpipe/softpipe/swrast, or the C++
// fallback) spend a whole core per animated frame at 1280x800, so `auto` picks `reduced` there
// and `full` on a real GPU.
//   full     the profile as designed (M1 output, unchanged)
//   reduced  no static noise / jitter (drops the per-pixel noise fetch from every tick), half
//            bloom radius, animation ticks at half rate under a CPU governor; flicker,
//            horizontal sync, glowing line, burn-in and scanlines stay
//   static   nothing animates: no noise, jitter, flicker, sync, glowing line or burn-in. The
//            remaining passes (warp, tint, bloom, scanlines, ambient: a fixed per-pixel
//            overlay) run once per content change and the view never ticks
enum class EffectsQuality { Auto, Full, Reduced, Static };
bool parseEffectsQuality(const std::string& s, EffectsQuality& out);
const char* effectsQualityName(EffectsQuality q);
// GL_RENDERER of a Mesa software rasteriser (llvmpipe, softpipe, swrast), case-insensitive.
bool isSoftwareRendererName(const std::string& glRenderer);
// LIBGL_ALWAYS_SOFTWARE set to anything but "", "0" or "false".
bool softwareRenderingForced();
// Auto -> Reduced on software rendering, Full otherwise; explicit choices are kept.
EffectsQuality resolveEffectsQuality(EffectsQuality q, bool softwareRendering);
// The profile parameters for a resolved quality (Full returns p unchanged).
EffectParams applyEffectsQuality(const EffectParams& p, EffectsQuality resolved);
bool sameEffectParams(const EffectParams& a, const EffectParams& b);

// Byte order of 8-bit source/destination pixels. BGRA is Cairo's ARGB32/RGB24 in memory on a
// little-endian machine, which lets the view hand its Cairo surface over without conversion.
enum class PixelOrder { RGBA, BGRA };

// Values derived from the profile the way CRT 1.2.0 does it (see ShaderTerminal.qml).
struct DerivedParams {
    float curvature;          // screenCurvature * 0.4
    float bloomStrength;      // bloom * 2.5
    float screenBrightness;   // lint(0.5,1.5,brightness)
    float ambient;            // ambientLight * 0.2
    float glowingLine;        // glowingLine * 0.2
    float hsyncStrength;      // lint(0.05,0.35,horizontalSync)
    float jitterX, jitterY;   // 0.007*j, 0.002*j
    float burnInTime;         // 1/lint(160,1600,burnIn)  (per ms)
};
DerivedParams derive(const EffectParams& p);

// Deterministic 512x512 RGBA noise texture (stand-in for CRT's allNoise512.png).
constexpr int kNoiseSize = 512;
const std::vector<float>& noiseTexture();   // RGBA float, kNoiseSize^2*4

// Per-frame uniforms computed on the CPU (identical for both back-ends).
struct FrameUniforms {
    float timeMs;
    float flickerBrightness;  // 1 + (n.g-0.5)*flickering
    float hsyncScale;
    float hsyncFreq;
    float noiseOffsetX, noiseOffsetY;
    float burnInDecay;        // clamp((time - burnInLastUpdate)*burnInTime, 0, 1)
};
FrameUniforms frameUniforms(const EffectParams& p, float timeMs, float burnInLastUpdateMs);

// Gaussian weights for the bloom blur, index 0..radius.
std::vector<float> bloomWeights(int radius);

// Common pixel helpers (exactly mirrored in GLSL).
inline float rgb2grey(float r, float g, float b) { return 0.21f * r + 0.72f * g + 0.04f * b; }

// Barrel warp: v - cc*(1+d)*d with cc = 0.5 - v, d = |cc|^2 * curvature.
inline void barrel(float u, float v, float curvature, float& ou, float& ov) {
    float cx = 0.5f - u, cy = 0.5f - v;
    float d = (cx * cx + cy * cy) * curvature;
    ou = u - cx * (1.0f + d) * d;
    ov = v - cy * (1.0f + d) * d;
}

// Abstract interface used by the benchmark, the comparison tool and the terminal window.
class EffectsPipeline {
public:
    virtual ~EffectsPipeline() = default;
    virtual void resize(int width, int height) = 0;
    // Terminal content changed. px is 8-bit 4-channel (see PixelOrder), row 0 = top. Only rows
    // [rowBegin, rowEnd) changed since the previous call (rowEnd < 0: all rows); the others must
    // be identical to what was passed before.
    virtual void updateSource(const uint8_t* px, int stride, PixelOrder order, float timeMs,
                              int rowBegin = 0, int rowEnd = -1) = 0;
    void setSource(const uint8_t* rgba, int stride, float timeMs) {
        updateSource(rgba, stride, PixelOrder::RGBA, timeMs);
    }
    // One effects tick; writes RGBA8 (row 0 = top) into out (width*height*4).
    virtual void render(float timeMs, uint8_t* out) = 0;
    virtual const char* name() const = 0;
};

}  // namespace lterm
