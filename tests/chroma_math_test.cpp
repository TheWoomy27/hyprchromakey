// Runs hyprchromakey's key test on the GPU against known colors and checks the resulting alpha.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "../src/ChromaGLSL.hpp"

static const std::string VERT = R"glsl(#version 300 es
in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)glsl";

// single-pixel checks: the color comes in as a uniform, no texture involved
static const std::string FRAG_HEAD = R"glsl(#version 300 es
precision highp float;
uniform vec4 inColor;
layout(location = 0) out vec4 fragColor;
)glsl";

static const std::string FRAG_TAIL = R"glsl(
void main() {
    vec4 pixColor = inColor;
    hcApplyChroma(pixColor);
    fragColor = pixColor;
}
)glsl";

// region checks: shaped like hyprland's surface shader, sampling `tex` at `v_texcoord`
static const std::string TEX_VERT = R"glsl(#version 300 es
in vec2 pos;
out vec2 v_texcoord;
void main() {
    v_texcoord  = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

static const std::string TEX_FRAG_HEAD = R"glsl(#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
layout(location = 0) out vec4 fragColor;
)glsl";

static const std::string TEX_FRAG_TAIL = R"glsl(
void main() {
    vec4 pixColor = texture(tex, v_texcoord);
    hcApplyChroma(pixColor);
    fragColor = pixColor;
}
)glsl";

static GLuint compile(GLenum type, const std::string& src) {
    GLuint      s = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(s, 1, &c, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[4096];
        glGetShaderInfoLog(s, sizeof(info), nullptr, info);
        std::cerr << "shader compile failed:\n" << info << "\n";
        exit(2);
    }
    return s;
}

struct SKey {
    std::array<float, 3> color;
    float                similarity, smoothness, opacity;
    int                  mode;
    int                  region = 0;
};

static GLuint g_prog = 0;

static GLuint link(const std::string& vert, const std::string& frag) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vert));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, frag));
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char info[4096];
        glGetProgramInfoLog(prog, sizeof(info), nullptr, info);
        std::cerr << "link failed:\n" << info << "\n";
        exit(2);
    }
    return prog;
}

static void setKeys(const std::vector<SKey>& keys, float minAlpha) {
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "hcKeyCount"), (GLint)keys.size());
    glUniform1f(glGetUniformLocation(g_prog, "hcMinAlpha"), minAlpha);

    std::vector<float> colors, params;
    for (const auto& k : keys) {
        colors.insert(colors.end(), {k.color[0], k.color[1], k.color[2], (float)k.region});
        params.insert(params.end(), {k.similarity, k.smoothness, k.opacity, (float)k.mode});
    }
    if (!keys.empty()) {
        glUniform4fv(glGetUniformLocation(g_prog, "hcKeyColor"), (GLsizei)keys.size(), colors.data());
        glUniform4fv(glGetUniformLocation(g_prog, "hcKeyParams"), (GLsizei)keys.size(), params.data());
    }
}

// returns the resulting straight (un-premultiplied) alpha for an input pixel
static float run(std::array<float, 4> straight) {
    // hyprland hands the shader premultiplied colors
    const float A = straight[3];
    glUseProgram(g_prog);
    glUniform4f(glGetUniformLocation(g_prog, "inColor"), straight[0] * A, straight[1] * A, straight[2] * A, A);

    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    std::array<uint8_t, 4> px{};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px[3] / 255.F;
}

static int  g_failures = 0;

static void check(const std::string& what, float got, float want, float tolerance = 0.01F) {
    const bool OK = std::fabs(got - want) <= tolerance;
    if (!OK)
        ++g_failures;
    std::cout << std::format("[{}] {}: alpha {:.3f} (want {:.3f})\n", OK ? " ok " : "FAIL", what, got, want);
}

// ---- min_region: a synthetic window -----------------------------------------------------------
//
//   flat background of the key color, with
//   - a 2px white "glyph" stroke running top to bottom
//   - a white ring around a 2x2 hole of key color: a letter counter
//   - a block of noisy "media" with one pixel, and one 3x3 patch, of exactly the key color
//   - an antialiased pixel (25% white over the key) right next to the stroke

constexpr int W = 64, H = 32;

struct SScene {
    std::vector<uint8_t> rgba = std::vector<uint8_t>(W * H * 4);

    void                 set(int x, int y, std::array<float, 3> c) {
        auto* p = &rgba[(y * W + x) * 4];
        for (int i = 0; i < 3; ++i)
            p[i] = (uint8_t)std::lround(std::clamp(c[i], 0.F, 1.F) * 255.F);
        p[3] = 255;
    }
};

static SScene makeScene(std::array<float, 3> bg) {
    SScene scene;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            scene.set(x, y, bg);

    // glyph stroke at x = 10, 11
    for (int y = 0; y < H; ++y) {
        scene.set(10, y, {1, 1, 1});
        scene.set(11, y, {1, 1, 1});
    }

    // faint antialiasing just right of the stroke, on row 5: 5% white over the key
    scene.set(12, 5, {bg[0] * 0.95F + 0.05F, bg[1] * 0.95F + 0.05F, bg[2] * 0.95F + 0.05F});

    // letter counter: ring from (20,10) to (23,13), hole at (21..22, 11..12)
    for (int y = 10; y <= 13; ++y)
        for (int x = 20; x <= 23; ++x)
            if (x == 20 || x == 23 || y == 10 || y == 13)
                scene.set(x, y, {1, 1, 1});

    // media: x 36..59, y 4..27, a dark frame - deterministic noise just above the key color, the
    // way a dark video scene sits close to a dark theme's background
    uint32_t seed = 12345;
    for (int y = 4; y < 28; ++y)
        for (int x = 36; x < 60; ++x) {
            seed = seed * 1664525 + 1013904223;
            const float N = 0.01F + ((seed >> 24) & 0xFF) / 255.F * 0.11F;
            scene.set(x, y, {bg[0] + N, bg[1] + N * 0.8F, bg[2] + N * 1.2F});
        }
    scene.set(40, 8, bg); // one stray pixel that happens to be the key color
    for (int y = 18; y <= 20; ++y)
        for (int x = 48; x <= 50; ++x)
            scene.set(x, y, bg); // a 3x3 patch of it

    // a bright patch at x 26..31, y 20..27 with one key-colored speck in it
    for (int y = 20; y < 28; ++y)
        for (int x = 26; x < 32; ++x)
            scene.set(x, y, {0.85F, 0.8F, 0.7F});
    scene.set(28, 23, bg);

    return scene;
}

static std::vector<uint8_t> renderScene(GLuint prog, const SScene& scene) {
    GLuint src = 0, dst = 0, fbo = 0;
    glGenTextures(1, &src);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, scene.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);

    glViewport(0, 0, W, H);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    std::vector<uint8_t> out(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &src);
    glDeleteTextures(1, &dst);
    glViewport(0, 0, 1, 1);
    return out;
}

static void regionChecks(GLuint prog, std::array<float, 3> bg) {
    const auto SCENE = makeScene(bg);
    const auto PREV  = g_prog;
    g_prog           = prog;

    // texture rows and framebuffer rows both start at the bottom, so (x, y) lines up directly
    const auto ALPHA = [](const std::vector<uint8_t>& out, int x, int y) { return out[(y * W + x) * 4 + 3] / 255.F; };

    std::cout << "\nmin_region 0 (off): everything matching is keyed, media included\n";
    setKeys({{bg, 0.0F, 0.0F, 0.F, 0, 0}}, 0.99F);
    auto out = renderScene(prog, SCENE);
    check("open background", ALPHA(out, 20, 25), 0.F);
    check("stray key pixel inside media", ALPHA(out, 40, 8), 0.F);

    std::cout << "\nmin_region 8: only flat runs of the key color are keyed\n";
    setKeys({{bg, 0.0F, 0.1F, 0.F, 0, 8}}, 0.99F);
    out = renderScene(prog, SCENE);
    check("open background", ALPHA(out, 20, 25), 0.F);
    check("background touching the glyph, left side", ALPHA(out, 9, 16), 0.F);
    check("background touching the glyph, right side", ALPHA(out, 13, 16), 0.F);
    check("background at the texture edge", ALPHA(out, 0, 0), 0.F);
    check("background just outside the counter ring", ALPHA(out, 24, 11), 0.F);
    // ~0.048 into a 0.1 smoothness band
    check("antialiased edge pixel keeps its partial key", ALPHA(out, 12, 5), 0.47F, 0.06F);
    check("glyph itself", ALPHA(out, 10, 16), 1.F);
    check("stray key pixel inside media", ALPHA(out, 40, 8), 1.F);
    check("3x3 key patch inside media", ALPHA(out, 49, 19), 1.F);
    check("ordinary media pixel", ALPHA(out, 44, 14), 1.F);
    check("2x2 counter inside a letter: walled in, so still keyed", ALPHA(out, 21, 11), 0.F);
    check("dark media pixel inside the smoothness band", ALPHA(out, 37, 5), 1.F);
    // the documented cost: a lone key-colored speck in a bright area looks exactly like a tiny counter
    check("key speck inside a bright area (known cost)", ALPHA(out, 28, 23), 0.F);

    std::cout << "\nmin_region 3: the 3x3 patch is now big enough, all of it\n";
    setKeys({{bg, 0.0F, 0.0F, 0.F, 0, 3}}, 0.99F);
    out = renderScene(prog, SCENE);
    check("3x3 patch, center", ALPHA(out, 49, 19), 0.F);
    check("3x3 patch, corner", ALPHA(out, 48, 18), 0.F);
    check("stray key pixel inside media", ALPHA(out, 40, 8), 1.F);
    check("2x2 counter inside a letter", ALPHA(out, 21, 11), 0.F);

    std::cout << "\nmin_region 2\n";
    setKeys({{bg, 0.0F, 0.0F, 0.F, 0, 2}}, 0.99F);
    out = renderScene(prog, SCENE);
    check("2x2 counter inside a letter", ALPHA(out, 21, 11), 0.F);
    check("stray key pixel inside media", ALPHA(out, 40, 8), 1.F);

    std::cout << "\nmin_region 40: long probes are strided, background still keys\n";
    setKeys({{bg, 0.0F, 0.0F, 0.F, 0, 40}}, 0.99F);
    out = renderScene(prog, SCENE);
    check("open background", ALPHA(out, 20, 25), 0.F);
    check("background touching the glyph", ALPHA(out, 13, 16), 0.F);
    check("stray key pixel inside media", ALPHA(out, 40, 8), 1.F);

    g_prog = PREV;
}

int main() {
    auto       getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy                = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    eglInitialize(dpy, nullptr, nullptr);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint     ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx        = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttrs);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    // 1x1 RGBA8 target
    GLuint fbo = 0, tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 1, 1);

    const GLuint POINT_PROG  = link(VERT, FRAG_HEAD + chromaDefines({.rgba = true, .regionTest = false}) + CHROMA_GLSL + FRAG_TAIL);
    const GLuint REGION_PROG = link(TEX_VERT, TEX_FRAG_HEAD + chromaDefines({.rgba = true, .regionTest = true}) + CHROMA_GLSL + TEX_FRAG_TAIL);
    g_prog                   = POINT_PROG;

    const float QUAD[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint      vbo = 0, vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // ---- a single dark background key, the classic setup -----------------------------------
    const std::array<float, 3> BG = {7 / 255.F, 8 / 255.F, 17 / 255.F};
    setKeys({{BG, 0.08F, 0.02F, 0.F, 0}}, 0.99F);

    std::cout << "\nsingle rgb key #070811, similarity 0.08, smoothness 0.02, opacity 0\n";
    check("exact background", run({BG[0], BG[1], BG[2], 1.F}), 0.F);
    check("background +4/255 (inside)", run({BG[0] + 4 / 255.F, BG[1] + 4 / 255.F, BG[2] + 4 / 255.F, 1.F}), 0.F);
    check("white text", run({1.F, 1.F, 1.F, 1.F}), 1.F);
    check("mid grey", run({0.5F, 0.5F, 0.5F, 1.F}), 1.F);
    check("bright red", run({1.F, 0.F, 0.F, 1.F}), 1.F);
    // just past similarity, inside the smooth band -> partially keyed
    check("edge of band (0.09)", run({BG[0] + 0.09F, BG[1], BG[2], 1.F}), 0.5F);
    check("past the band (0.15)", run({BG[0] + 0.15F, BG[1], BG[2], 1.F}), 1.F);

    std::cout << "\nalpha gate (min_alpha 0.99)\n";
    check("half transparent background", run({BG[0], BG[1], BG[2], 0.5F}), 0.5F);
    check("nearly opaque background (0.995)", run({BG[0], BG[1], BG[2], 0.995F}), 0.F);

    std::cout << "\npartial opacity (0.35)\n";
    setKeys({{BG, 0.08F, 0.02F, 0.35F, 0}}, 0.99F);
    check("exact background", run({BG[0], BG[1], BG[2], 1.F}), 0.35F);
    check("white text", run({1.F, 1.F, 1.F, 1.F}), 1.F);

    std::cout << "\ntwo keys in one profile\n";
    const std::array<float, 3> MAUVE = {0xcb / 255.F, 0xa6 / 255.F, 0xf7 / 255.F};
    setKeys({{BG, 0.05F, 0.01F, 0.F, 0}, {MAUVE, 0.05F, 0.01F, 0.5F, 0}}, 0.99F);
    check("first key -> transparent", run({BG[0], BG[1], BG[2], 1.F}), 0.F);
    check("second key -> half", run({MAUVE[0], MAUVE[1], MAUVE[2], 1.F}), 0.5F);
    check("neither", run({0.2F, 0.6F, 0.3F, 1.F}), 1.F);

    std::cout << "\nhsv mode: same hue, different brightness\n";
    setKeys({{{0.2F, 0.4F, 0.8F}, 0.15F, 0.02F, 0.F, 1}}, 0.99F);
    check("exact", run({0.2F, 0.4F, 0.8F, 1.F}), 0.F);
    check("same hue much darker", run({0.05F, 0.1F, 0.2F, 1.F}), 1.F); // value differs a lot
    check("different hue", run({0.8F, 0.4F, 0.2F, 1.F}), 1.F);

    std::cout << "\nchroma mode: brightness is ignored\n";
    setKeys({{{0.2F, 0.4F, 0.8F}, 0.15F, 0.02F, 0.F, 2}}, 0.99F);
    check("exact", run({0.2F, 0.4F, 0.8F, 1.F}), 0.F);
    check("same chroma, half brightness", run({0.1F, 0.2F, 0.4F, 1.F}), 0.F);
    check("same chroma, much darker", run({0.05F, 0.1F, 0.2F, 1.F}), 0.F);
    check("white", run({1.F, 1.F, 1.F, 1.F}), 1.F);
    check("mid grey", run({0.5F, 0.5F, 0.5F, 1.F}), 1.F);
    check("different chroma", run({0.8F, 0.4F, 0.2F, 1.F}), 1.F);

    std::cout << "\nsimilarity 0, smoothness 0: exact 8-bit matches only\n";
    setKeys({{BG, 0.0F, 0.0F, 0.F, 0}}, 0.99F);
    check("exact background", run({BG[0], BG[1], BG[2], 1.F}), 0.F);
    check("one 8-bit step off", run({BG[0] + 1 / 255.F, BG[1], BG[2], 1.F}), 1.F);

    std::cout << "\nno keys configured\n";
    setKeys({}, 0.99F);
    check("background untouched", run({BG[0], BG[1], BG[2], 1.F}), 1.F);

    regionChecks(REGION_PROG, BG);

    std::cout << "\n" << (g_failures ? std::format("{} FAILURES", g_failures) : "all good") << "\n";
    return g_failures ? 1 : 0;
}
