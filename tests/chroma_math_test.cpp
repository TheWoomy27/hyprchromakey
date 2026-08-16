// Runs hyprchromakey's key test on the GPU against known colors and checks the resulting alpha.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

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
};

static GLuint g_prog = 0;

static void   setKeys(const std::vector<SKey>& keys, float minAlpha) {
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "hcKeyCount"), (GLint)keys.size());
    glUniform1f(glGetUniformLocation(g_prog, "hcMinAlpha"), minAlpha);

    std::vector<float> colors, params;
    for (const auto& k : keys) {
        colors.insert(colors.end(), {k.color[0], k.color[1], k.color[2]});
        params.insert(params.end(), {k.similarity, k.smoothness, k.opacity, (float)k.mode});
    }
    if (!keys.empty()) {
        glUniform3fv(glGetUniformLocation(g_prog, "hcKeyColor"), (GLsizei)keys.size(), colors.data());
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

    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, VERT));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, FRAG_HEAD + CHROMA_GLSL + FRAG_TAIL));
    glLinkProgram(g_prog);

    GLint linked = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char info[4096];
        glGetProgramInfoLog(g_prog, sizeof(info), nullptr, info);
        std::cerr << "link failed:\n" << info << "\n";
        return 2;
    }

    const float           QUAD[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint                vbo = 0, vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);
    const GLint POS = glGetAttribLocation(g_prog, "pos");
    glEnableVertexAttribArray(POS);
    glVertexAttribPointer(POS, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

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

    std::cout << "\nno keys configured\n";
    setKeys({}, 0.99F);
    check("background untouched", run({BG[0], BG[1], BG[2], 1.F}), 1.F);

    std::cout << "\n" << (g_failures ? std::format("{} FAILURES", g_failures) : "all good") << "\n";
    return g_failures ? 1 : 0;
}
