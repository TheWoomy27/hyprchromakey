#pragma once

// The chromakey GLSL and the splice that gets it into hyprland's shaders. Deliberately free of
// any hyprland includes so tests/ can compile and run this exact code against a real GL context.

#include <string>

constexpr size_t MAX_KEYS_PER_PROFILE = 16;
// longest min_region accepted. Probes are spaced to stay at 8 per direction, so a huge region
// only gets coarser, not slower - this is just a sanity bound.
constexpr int    MAX_REGION           = 1024;

// What the shader being patched looks like, as far as the key code needs to know.
struct SChromaShaderTraits {
    // the texture carries real alpha (RGBA) rather than padding (RGBX)
    bool rgba = true;
    // `tex` is a sampler2D we may take extra samples from. False for external (OES) textures,
    // which can't be sized or sampled at an explicit LOD, so the region test is left out there.
    bool regionTest = true;
};

// Spliced in just before main(). Every name is prefixed so it can never collide with hyprland's.
inline const std::string CHROMA_GLSL = R"glsl(
// ---- hyprchromakey ----
#define HC_MAX_KEYS 16
// Under half an 8-bit step. Some GPUs hand 8-bit textures back at reduced precision (around 2e-5
// off on NVIDIA), so a similarity of 0 has to mean "the same 8-bit color", not identical floats -
// otherwise an exact match would land just outside the band and never be keyed.
#define HC_EXACT 0.0015
// How far from the key a pixel has to be to count as a clear edge - a glyph, a border, an icon -
// rather than the near misses a photo or video fades through.
#define HC_EDGE  0.2

uniform int   hcKeyCount;
// rgb: key color, a: min_region in buffer pixels (0 = off)
uniform vec4  hcKeyColor[HC_MAX_KEYS];
// x: similarity, y: smoothness, z: target opacity, w: match mode
uniform vec4  hcKeyParams[HC_MAX_KEYS];
uniform float hcMinAlpha;

vec3 hcRgbToHsv(vec3 c) {
    vec4  K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4  p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4  q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// chroma normalized by luma, so a color and a darker shade of it land in the same place
vec2 hcChromaOf(vec3 c) {
    float luma = max(dot(c, vec3(0.299, 0.587, 0.114)), 0.05);
    return vec2(dot(c, vec3(-0.168736, -0.331264, 0.5)), dot(c, vec3(0.5, -0.418688, -0.081312))) / luma;
}

float hcDistance(vec3 a, vec3 b, float mode) {
    if (mode < 0.5) {
        vec3 d = abs(a - b);
        return max(d.r, max(d.g, d.b));
    }

    if (mode < 1.5) {
        vec3  ha  = hcRgbToHsv(a);
        vec3  hb  = hcRgbToHsv(b);
        float dh  = abs(ha.x - hb.x);
        dh        = min(dh, 1.0 - dh) * 2.0;
        // hue only matters as far as the pixels are actually saturated
        float sat = min(ha.y, hb.y);
        return max(dh * sat, max(abs(ha.y - hb.y), abs(ha.z - hb.z)));
    }

    return clamp(length(hcChromaOf(a) - hcChromaOf(b)), 0.0, 1.0);
}

#if HC_REGION_TEST
// The texture at `uv`, un-premultiplied; a negative alpha marks a pixel too transparent to judge.
vec4 hcSample(vec2 uv) {
    // explicit lod: implicit derivatives are undefined inside the loops this is called from
    vec4 c = textureLod(tex, uv, 0.0);
#if HC_RGBA
    return c.a >= max(hcMinAlpha, 0.002) ? vec4(c.rgb / c.a, c.a) : vec4(0.0, 0.0, 0.0, -1.0);
#else
    return vec4(c.rgb, 1.0);
#endif
}

// whether the pixel at `uv` matches key `i` exactly enough to count as part of a flat patch
bool hcOnKey(vec2 uv, int i) {
    vec4 c = hcSample(uv);
    // matches only, not the smoothness band: a run of near misses is a gradient, not a background
    return c.a >= 0.0 && hcDistance(c.rgb, hcKeyColor[i].rgb, hcKeyParams[i].w) <= max(hcKeyParams[i].x, HC_EXACT);
}

// How far the key color runs from `uv` along `dir`, up to `limit` pixels: the neighbor first,
// then every `stride` after it. The neighbor is what turns media away - its pixels are never all
// exactly the key color - so it is never skipped. Strides further out can step over a thin glyph,
// but that only ever lets a real background pixel through.
float hcRun(vec2 uv, vec2 dir, float stride, float limit, int i) {
    float run = 0.0;

    // bounded so every driver compiles it; the stride keeps the probe count at or under 8
    for (int k = 0; k < 8; ++k) {
        float dist = min(1.0 + float(k) * stride, limit);
        if (!hcOnKey(uv + dir * dist, i))
            return run;

        run = dist;
        if (run >= limit)
            break;
    }

    return run;
}

// Whether the key color, having run `run` pixels along `dir`, stops at a clear edge: something
// well away from the key within the next few pixels. Glyphs get there in a pixel or two of
// antialiasing; a dark photo or video frame only ever fades into near misses of the key.
bool hcClearEdge(vec2 uv, vec2 dir, float run, int i) {
    for (int k = 1; k <= 3; ++k) {
        vec4 c = hcSample(uv + dir * (run + float(k)));
        if (c.a < 0.0 || hcDistance(c.rgb, hcKeyColor[i].rgb, hcKeyParams[i].w) > HC_EDGE)
            return true;
    }

    return false;
}

// Whether the pixel at `uv` belongs to a real background patch of key `i` rather than to media
// that happens to brush the key color. It does if
//  - along either axis the key color stretches at least `len` pixels: backgrounds are big flat
//    areas, and even a pixel touching a glyph has the rest of the background stretching away on
//    the other side, or
//  - it is a smaller patch walled in by clear edges all round: the inside of an `o` or an `e`.
// Inside a photo or video the key color never stretches far, and peters out into near misses.
bool hcInRegion(vec2 uv, int i, float len) {
    // the stretch counts this pixel too
    float need = len - 1.0;
    if (need <= 0.0)
        return true;

    vec2  texel   = 1.0 / vec2(textureSize(tex, 0));
    float stride  = max(1.0, ceil((need - 1.0) / 7.0));
    bool  enclosed = true;

    for (int axis = 0; axis < 2; ++axis) {
        vec2  dir     = axis == 0 ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
        float forward = hcRun(uv, dir, stride, need, i);
        if (forward >= need)
            return true;

        float back = hcRun(uv, -dir, stride, need - forward, i);
        if (forward + back >= need)
            return true;

        enclosed = enclosed && hcClearEdge(uv, dir, forward, i) && hcClearEdge(uv, -dir, back, i);
    }

    return enclosed;
}
#endif

void hcApplyChroma(inout vec4 color) {
    if (hcKeyCount <= 0 || color.a < hcMinAlpha || color.a <= 0.001)
        return;

    // hyprland textures are premultiplied, so undo that before comparing
    vec3  rgb  = color.rgb / color.a;
    float keep = 1.0;

    for (int i = 0; i < HC_MAX_KEYS; ++i) {
        if (i >= hcKeyCount)
            break;

        vec4  p   = hcKeyParams[i];
        float d   = hcDistance(rgb, hcKeyColor[i].rgb, p.w);
        float sim = max(p.x, HC_EXACT);
        // 0 at the key color, 1 once we are past similarity + smoothness
        float t   = smoothstep(sim, sim + max(p.y, 1.0e-5), d);
        if (t >= 1.0)
            continue;

#if HC_REGION_TEST
        if (hcKeyColor[i].a >= 1.0 && !hcInRegion(v_texcoord, i, hcKeyColor[i].a))
            continue;
#endif

        keep = min(keep, mix(p.z, 1.0, t));
    }

    // still premultiplied, so rgb scales along with alpha
    color *= keep;
}
// ---- end hyprchromakey ----

)glsl";

// The defines CHROMA_GLSL expects, for a shader with the given traits.
inline std::string chromaDefines(const SChromaShaderTraits& traits) {
    return std::string{"\n#define HC_RGBA "} + (traits.rgba ? "1" : "0") + "\n#define HC_REGION_TEST " + (traits.regionTest ? "1" : "0") + "\n";
}

// Splices the chroma code into a preprocessed hyprland fragment shader. Returns an empty string
// if the shader does not look like something we know how to patch.
inline std::string patchChromaSource(const std::string& src, const SChromaShaderTraits& traits) {
    const auto MAIN = src.find("void main(");
    if (MAIN == std::string::npos)
        return "";

    // every surface shader starts main() by sampling into `pixColor`; key it right there, before
    // the discard tests, color management and rounding get a say
    const auto DECL = src.find("pixColor", MAIN);
    if (DECL == std::string::npos)
        return "";

    const auto SEMI = src.find(';', DECL);
    if (SEMI == std::string::npos)
        return "";

    const auto  DEFINES = chromaDefines(traits);

    std::string out;
    out.reserve(src.length() + DEFINES.length() + CHROMA_GLSL.length() + 32);
    out.append(src, 0, MAIN);
    out.append(DEFINES);
    out.append(CHROMA_GLSL);
    out.append(src, MAIN, SEMI + 1 - MAIN);
    out.append("\n    hcApplyChroma(pixColor);\n");
    out.append(src, SEMI + 1, std::string::npos);
    return out;
}
