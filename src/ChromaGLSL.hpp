#pragma once

// The chromakey GLSL and the splice that gets it into hyprland's shaders. Deliberately free of
// any hyprland includes so tests/ can compile and run this exact code against a real GL context.

#include <string>

constexpr size_t MAX_KEYS_PER_PROFILE = 16;

// Spliced in just before main(). Every name is prefixed so it can never collide with hyprland's.
inline const std::string CHROMA_GLSL = R"glsl(
// ---- hyprchromakey ----
#define HC_MAX_KEYS 16

uniform int   hcKeyCount;
uniform vec3  hcKeyColor[HC_MAX_KEYS];
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

void hcApplyChroma(inout vec4 color) {
    if (hcKeyCount <= 0 || color.a < hcMinAlpha || color.a <= 0.001)
        return;

    // hyprland textures are premultiplied, so undo that before comparing
    vec3  rgb  = color.rgb / color.a;
    float keep = 1.0;

    for (int i = 0; i < HC_MAX_KEYS; ++i) {
        if (i >= hcKeyCount)
            break;

        vec4  p = hcKeyParams[i];
        float d = hcDistance(rgb, hcKeyColor[i], p.w);
        // 0 at the key color, 1 once we are past similarity + smoothness
        float t = smoothstep(p.x, p.x + max(p.y, 1.0e-5), d);
        keep    = min(keep, mix(p.z, 1.0, t));
    }

    // still premultiplied, so rgb scales along with alpha
    color *= keep;
}
// ---- end hyprchromakey ----

)glsl";

// Splices the chroma code into a preprocessed hyprland fragment shader. Returns an empty string
// if the shader does not look like something we know how to patch.
inline std::string patchChromaSource(const std::string& src) {
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

    std::string out;
    out.reserve(src.length() + CHROMA_GLSL.length() + 32);
    out.append(src, 0, MAIN);
    out.append(CHROMA_GLSL);
    out.append(src, MAIN, SEMI + 1 - MAIN);
    out.append("\n    hcApplyChroma(pixColor);\n");
    out.append(src, SEMI + 1, std::string::npos);
    return out;
}
