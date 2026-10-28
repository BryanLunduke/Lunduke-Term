// SPDX-License-Identifier: GPL-3.0-or-later
// GLSL 3.30 versions of the passes in cpu_effects.cpp (GL is the main path since M1; the C++
// code is the fallback). Keep the two in sync: tests/gl_match_test.cpp checks PSNR >= 40 dB.
#pragma once

namespace lterm::glsl {

inline const char* kVertex = R"(#version 330 core
void main() {
    // Full-screen triangle.
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

inline const char* kCommon = R"(#version 330 core
out vec4 fragColor;
float rgb2grey(vec3 v) { return dot(v, vec3(0.21, 0.72, 0.04)); }
float stepf(float e, float x) { return x >= e ? 1.0 : 0.0; }
vec2 barrel(vec2 v, float curvature) {
    vec2 cc = vec2(0.5) - v;
    float d = dot(cc, cc) * curvature;
    return v - cc * (1.0 + d) * d;
}
// Bezel inset (fraction of the size, 0 without a frame) + barrel warp; mirrors
// lterm::screenCoords(). Returns the source uv, `l` gets the warped screen-local uv.
vec2 screenCoords(vec2 uv, vec2 inset, float curvature, out vec2 l) {
    l = (uv - inset) / (vec2(1.0) - 2.0 * inset);
    if (curvature != 0.0) l = barrel(l, curvature);
    return inset + l * (vec2(1.0) - 2.0 * inset);
}
)";

inline const char* kDownsample = R"(
uniform sampler2D src;
uniform ivec2 srcSize;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 a = clamp(2 * p, ivec2(0), srcSize - 1);
    ivec2 b = clamp(2 * p + 1, ivec2(0), srcSize - 1);
    vec4 s = texelFetch(src, ivec2(a.x, a.y), 0) + texelFetch(src, ivec2(b.x, a.y), 0)
           + texelFetch(src, ivec2(a.x, b.y), 0) + texelFetch(src, ivec2(b.x, b.y), 0);
    fragColor = 0.25 * s;
}
)";

inline const char* kBlur = R"(
uniform sampler2D src;
uniform ivec2 size;
uniform ivec2 dir;
uniform int radius;
uniform float weights[65];
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 acc = vec4(0.0);
    for (int k = -radius; k <= radius; ++k) {
        ivec2 q = p + dir * k;
        if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;
        acc += weights[abs(k)] * texelFetch(src, q, 0);
    }
    fragColor = acc;
}
)";

inline const char* kBurnIn = R"(
uniform sampler2D src;
uniform sampler2D prev;
uniform float decay0;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 t = texelFetch(src, p, 0).rgb;
    vec4 acc = texelFetch(prev, p, 0);
    float decay = max(0.0, decay0 - acc.a);
    fragColor = vec4(clamp(max(acc.rgb - vec3(decay), t), 0.0, 1.0), rgb2grey(t));
}
)";

inline const char* kStatic = R"(
uniform sampler2D src;
uniform sampler2D bloomSrc;
uniform vec2 size;
uniform float curvature;
uniform vec3 fontColor;
uniform vec3 backgroundColor;
uniform float chroma;
uniform float bloomStrength;
uniform float screenBrightness;
uniform bool useBloom;
uniform vec2 inset;
uniform float rgbShift;     // uv
void main() {
    vec2 uv = gl_FragCoord.xy / size;
    vec2 l;
    vec2 cv = screenCoords(uv, inset, curvature, l);
    vec3 t = texture(src, cv).rgb;
    if (rgbShift != 0.0) {
        vec3 rc = texture(src, cv + vec2(rgbShift, 0.0)).rgb;
        vec3 lc = texture(src, cv - vec2(rgbShift, 0.0)).rgb;
        t.r = lc.r * 0.10 + rc.r * 0.30 + t.r * 0.60;
        t.g = lc.g * 0.20 + rc.g * 0.20 + t.g * 0.60;
        t.b = lc.b * 0.30 + rc.b * 0.10 + t.b * 0.60;
    }
    t += vec3(0.0001);
    float g = rgb2grey(t);
    float mask = clamp((stepf(0.0, cv.x) - stepf(1.0, cv.x)) + (stepf(0.0, cv.y) - stepf(1.0, cv.y)), 0.0, 1.0);
    vec3 fg = chroma != 0.0 ? mix(fontColor, t * fontColor / g, chroma) : fontColor;
    vec3 o = mix(backgroundColor, fg, g * mask);
    if (useBloom) {
        vec3 b = texture(bloomSrc, cv).rgb;
        vec3 bc = fontColor * mix(vec3(rgb2grey(b)), b, chroma);
        o += clamp(bc * bloomStrength, 0.0, 0.5);
    }
    fragColor = vec4(o * screenBrightness, 1.0);
}
)";

inline const char* kDynamic = R"(
uniform sampler2D staticTex;
uniform sampler2D burnTex;
uniform sampler2D noiseTex;
uniform vec2 size;
uniform float curvature;
uniform vec3 fontColor;
uniform float chroma;
uniform float time;
uniform float hsync;        // >0 when enabled
uniform float hsyncScale;
uniform float hsyncFreq;
uniform float jitter;
uniform vec2 jitterDisp;
uniform float staticNoise;
uniform float noiseAmount;
uniform vec2 noiseScale;
uniform vec2 noiseOffset;
uniform float glowingLine;
uniform float burnIn;
uniform float burnInDecay;
uniform int rasterization;
uniform float vrY;
uniform float rasterIntensity;
uniform float flicker;
uniform float ambient;
uniform bool flipY;         // drawing straight into a window framebuffer (origin bottom-left)
uniform vec2 inset;
uniform bool useFrame;
uniform sampler2D frameTex;
float fractf(float x) { return x - floor(x); }
void main() {
    vec2 fc = gl_FragCoord.xy;
    if (flipY) fc.y = size.y - fc.y;
    vec2 uv = fc / size;
    vec2 cc = vec2(0.5) - (uv - inset) / (vec2(1.0) - 2.0 * inset);
    float dist = length(cc);
    vec2 l;
    vec2 sc = screenCoords(uv, inset, curvature, l);
    float inScreen = min(stepf(0.0, l.x) - stepf(1.0, l.x), stepf(0.0, l.y) - stepf(1.0, l.y));
    vec2 coords = uv;
    if (hsync != 0.0) coords.x += sin((uv.y + time * 0.001) * hsyncFreq) * hsyncScale;
    vec4 nt = vec4(0.0);
    if (jitter != 0.0 || staticNoise != 0.0) nt = texture(noiseTex, noiseScale * coords + noiseOffset);
    vec2 tc = coords;
    if (jitter != 0.0) tc += (nt.ba - vec2(0.5)) * jitterDisp;
    float color = 0.0001;
    if (staticNoise != 0.0) color += nt.a * noiseAmount * (1.0 - dist * 1.3);
    if (glowingLine != 0.0) {
        color += fractf(smoothstep(-120.0, 0.0, uv.y * vrY - (vrY + 120.0) * fractf(time * 0.00015))) * glowingLine;
    }
    vec3 t = texture(staticTex, tc).rgb;
    if (burnIn != 0.0) {
        vec3 b = 0.65 * (texture(burnTex, sc).rgb - vec3(burnInDecay));
        t = max(t, fontColor * mix(vec3(rgb2grey(b)), b, chroma));
    }
    t += fontColor * color;
    if (rasterization == 1) {
        float c = fractf(sc.y * vrY) * 2.0 - 1.0;
        float mask = 1.0 - abs(c);
        vec3 hi = ((1.0 + 0.30) - (0.2 * t)) * t;
        vec3 lo = ((1.0 - 0.30) + (0.1 * t)) * t;
        t = mix(t, mix(lo, hi, mask), rasterIntensity);
    }
    float amb = ambient * (1.0 - dist) * (1.0 - dist);
    vec3 f = clamp((t * flicker + vec3(amb)) * inScreen, 0.0, 1.0);
    if (useFrame) {
        vec4 fr = texture(frameTex, uv);
        f = f * (1.0 - fr.a) + fr.rgb;
    }
    fragColor = vec4(clamp(f, 0.0, 1.0), 1.0);
}
)";

// Static bezel layer; mirrors lterm::frameLayerRows(). mode 0: premultiplied base, 1: reflection
// weight + mirrored source uv.
inline const char* kFrame = R"(
uniform vec2 size;
uniform vec2 inset;         // fraction
uniform float insetPx;
uniform vec2 inner;         // device px
uniform float radius;
uniform float curvature;
uniform vec3 frameColor;
uniform float shininess;
uniform float pixelScale;
uniform int mode;
float sstep(float e0, float e1, float x) { float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
void main() {
    vec2 uv = gl_FragCoord.xy / size;
    vec2 l;
    screenCoords(uv, inset, curvature, l);
    vec2 p = (l - vec2(0.5)) * inner;
    vec2 q = abs(p) - (0.5 * inner - vec2(radius));
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
    float a = clamp(d + 0.5, 0.0, 1.0);
    float F = insetPx;
    vec2 xy = gl_FragCoord.xy;
    float e = min(min(xy.x, size.x - xy.x), min(xy.y, size.y - xy.y));
    if (mode == 1) {
        float wr = a * shininess * 0.45 * exp(-max(d, 0.0) / max(1.0, 0.3 * F));
        if (wr < 1.0 / 1024.0) wr = 0.0;
        vec2 m = vec2(1.0) - abs(vec2(1.0) - abs(l));
        fragColor = vec4(wr, inset + m * (vec2(1.0) - 2.0 * inset), 0.0);
        return;
    }
    float shade = mix(0.45, 1.0, sstep(0.0, 0.6 * F, d)) * mix(0.75, 1.0, sstep(0.0, 0.2 * F, e)) * mix(1.08, 0.90, uv.y);
    vec2 n = sign(p) * max(q, vec2(0.0));
    float len = length(n);
    if (len > 0.0) n /= len;
    float lam = -dot(n, vec2(-0.4472136, -0.8944272));
    float band = 1.0 - sstep(0.0, 0.35 * F, d);
    shade *= 1.0 + 0.35 * lam * band;
    float spec = shininess * 0.55 * pow(max(lam, 0.0), 8.0) * band;
    float sh = (1.0 - a) * 0.6 * (1.0 - sstep(0.0, 0.10 * F + 2.0 * pixelScale, -d));
    float A = a + sh;
    vec2 gq = (l - vec2(0.30, 0.17)) / vec2(0.20, 0.11);
    float gl = shininess * 0.09 * exp(-dot(gq, gq));
    if (gl < 1.0 / 1024.0) gl = 0.0;
    fragColor = vec4(a * (frameColor * shade + vec3(spec)) + vec3((1.0 - A) * gl), A);
}
)";

// Per content change: base layer + the bloom mirrored onto the frame.
inline const char* kFrameComposite = R"(
uniform sampler2D baseTex;
uniform sampler2D reflTex;
uniform sampler2D bloomSrc;
uniform vec3 fontColor;
uniform float chroma;
uniform float bloomStrength;
uniform float screenBrightness;
uniform bool useRefl;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 o = texelFetch(baseTex, p, 0);
    vec4 r = texelFetch(reflTex, p, 0);
    if (useRefl && r.r > 0.0) {
        vec3 b = texture(bloomSrc, r.gb).rgb;
        vec3 bc = clamp(fontColor * mix(vec3(rgb2grey(b)), b, chroma) * bloomStrength, 0.0, 0.5);
        o.rgb += r.r * bc * screenBrightness;
    }
    fragColor = o;
}
)";

}  // namespace lterm::glsl
