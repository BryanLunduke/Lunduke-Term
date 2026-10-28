// SPDX-License-Identifier: GPL-3.0-or-later
// GLSL 3.30 versions of the passes in cpu_effects.cpp. Keep the two in sync.
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
void main() {
    vec2 uv = gl_FragCoord.xy / size;
    vec2 cv = curvature != 0.0 ? barrel(uv, curvature) : uv;
    vec3 t = texture(src, cv).rgb + vec3(0.0001);
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
float fractf(float x) { return x - floor(x); }
void main() {
    vec2 uv = gl_FragCoord.xy / size;
    vec2 cc = vec2(0.5) - uv;
    float dist = length(cc);
    vec2 sc = curvature != 0.0 ? barrel(uv, curvature) : uv;
    float inScreen = min(stepf(0.0, sc.x) - stepf(1.0, sc.x), stepf(0.0, sc.y) - stepf(1.0, sc.y));
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
    vec3 f = (t * flicker + vec3(amb)) * inScreen;
    fragColor = vec4(clamp(f, 0.0, 1.0), 1.0);
}
)";

}  // namespace lterm::glsl
