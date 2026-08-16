// Offline check: reproduce hyprland's shader variant preprocessing, apply hyprchromakey's patch,
// and actually compile the result on a headless GLES context.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <cstring>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../src/ChromaGLSL.hpp"

static std::string       g_shaderDir;
static std::string       g_overrideDefines;
static std::map<std::string, std::string> g_includes;

static std::string readFile(const std::string& path) {
    std::ifstream     f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// mirrors CShaderLoader::loadShader: defines get injected after the #version/#extension header
static std::string loadShader(const std::string& filename) {
    auto src = readFile(g_shaderDir + "/" + filename);
    if (g_overrideDefines.empty())
        return src;

    // hyprland replaces the contents of defines.h with the computed defines; do the same by
    // stashing them as the include payload
    return src;
}

static glsl_include_result_t* includeLocal(void* ctx, const char* headerName, const char* includerName, size_t depth) {
    static std::vector<glsl_include_result_t*> results;
    static std::vector<std::string*>           storage;

    std::string                                name = headerName;
    std::string                                content;

    if (name == "defines.h")
        content = g_overrideDefines;
    else
        content = readFile(g_shaderDir + "/" + name);

    auto* data   = new std::string(content);
    auto* result = new glsl_include_result_t{};
    result->header_name   = strdup(name.c_str());
    result->header_data   = data->c_str();
    result->header_length = data->length();
    storage.push_back(data);
    results.push_back(result);
    return result;
}

static int freeInclude(void* ctx, glsl_include_result_t* result) {
    return 0;
}

static std::string processSource(const std::string& source) {
    glsl_include_callbacks_t callbacks{};
    callbacks.include_local  = includeLocal;
    callbacks.include_system = includeLocal;
    callbacks.free_include_result = freeInclude;

    glslang_input_t input{};
    input.language                          = GLSLANG_SOURCE_GLSL;
    input.stage                             = GLSLANG_STAGE_FRAGMENT;
    input.client                            = GLSLANG_CLIENT_NONE;
    input.target_language                   = GLSLANG_TARGET_NONE;
    input.code                              = source.c_str();
    input.default_version                   = 100;
    input.default_profile                   = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = false;
    input.forward_compatible                = false;
    input.messages                          = GLSLANG_MSG_DEFAULT_BIT;
    input.resource                          = glslang_default_resource();
    input.callbacks                         = callbacks;
    input.callbacks_ctx                     = nullptr;

    glslang_shader_t* shader = glslang_shader_create(&input);
    if (!glslang_shader_preprocess(shader, &input)) {
        std::cerr << "preprocess failed:\n" << glslang_shader_get_info_log(shader) << "\n";
        glslang_shader_delete(shader);
        return "";
    }

    std::stringstream stream(glslang_shader_get_preprocessed_code(shader));
    std::string       code, line;
    while (std::getline(stream, line)) {
        if (!line.starts_with("#line "))
            code += line + "\n";
    }
    glslang_shader_delete(shader);
    return code;
}

static std::string getDefines(uint16_t features) {
    static const std::array<std::pair<const char*, uint16_t>, 14> DEFINES = {{
        {"USE_RGBA", 1 << 0},
        {"USE_DISCARD", 1 << 1},
        {"USE_TINT", 1 << 2},
        {"USE_ROUNDING", 1 << 3},
        {"USE_CM", 1 << 4},
        {"USE_TONEMAP", 1 << 5},
        {"USE_SDR_MOD", 1 << 6},
        {"USE_BLUR", 1 << 7},
        {"USE_ICC", 1 << 8},
        {"USE_MIRROR", 1 << 9},
        {"USE_MOTION_BLUR", 1 << 10},
        {"USE_BLUR_ALPHA_MASK", 1 << 11},
        {"USE_BLUR_MATTE", 1 << 12},
        {"USE_ALT_TONEMAP", 1 << 13},
    }};

    std::string res;
    for (const auto& [name, flag] : DEFINES) {
        res += std::format("#define {} {}\n", name, (features & flag) != 0 ? '1' : '0');
    }
    return res;
}


static bool compileGLES(const std::string& src, std::string& log) {
    GLuint      shader = glCreateShader(GL_FRAGMENT_SHADER);
    const char* c      = src.c_str();
    glShaderSource(shader, 1, &c, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[8192];
        glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
        log = info;
    }
    glDeleteShader(shader);
    return ok;
}

int main(int argc, char** argv) {
    g_shaderDir = argv[1];

    // headless GLES context
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY) {
        std::cerr << "no surfaceless EGL display\n";
        return 2;
    }

    if (!eglInitialize(dpy, nullptr, nullptr)) {
        std::cerr << "eglInitialize failed\n";
        return 2;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint    cfgAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint    n = 0;
    eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n);

    EGLint     ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx        = eglCreateContext(dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT) {
        std::cerr << "eglCreateContext failed\n";
        return 2;
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    std::cout << "GL_VERSION: " << (const char*)glGetString(GL_VERSION) << "\n\n";

    // the feature combos hyprland actually builds for window surfaces
    const std::vector<std::pair<const char*, uint16_t>> CASES = {
        {"surface: plain RGBX", 0},
        {"surface: RGBA", 1 << 0},
        {"surface: RGBA+discard", (1 << 0) | (1 << 1)},
        {"surface: RGBA+rounding", (1 << 0) | (1 << 3)},
        {"surface: RGBA+tint+rounding", (1 << 0) | (1 << 2) | (1 << 3)},
        {"surface: RGBA+CM", (1 << 0) | (1 << 4)},
        {"surface: RGBA+CM+tonemap", (1 << 0) | (1 << 4) | (1 << 5)},
        {"surface: RGBA+CM+tonemap+sdrmod", (1 << 0) | (1 << 4) | (1 << 5) | (1 << 6)},
        {"surface: RGBA+blur", (1 << 0) | (1 << 7)},
        {"surface: RGBA+blur+matte+discard", (1 << 0) | (1 << 1) | (1 << 7) | (1 << 12)},
        {"surface: RGBA+mirror", (1 << 0) | (1 << 9)},
        {"surface: RGBA+mirror+CM", (1 << 0) | (1 << 9) | (1 << 4)},
        {"surface: RGBA+motionblur", (1 << 0) | (1 << 10)},
        {"surface: everything", 0x3FFF & ~(1 << 8)},
    };

    int failures = 0;

    for (const auto& [name, features] : CASES) {
        g_overrideDefines = getDefines(features);
        const auto RAW    = processSource(readFile(g_shaderDir + "/surface.frag"));
        if (RAW.empty()) {
            std::cout << std::format("[SKIP] {} (preprocess failed)\n", name);
            continue;
        }

        const auto PATCHED = patchChromaSource(RAW);
        if (PATCHED.empty()) {
            std::cout << std::format("[FAIL] {}: patch anchors not found\n", name);
            ++failures;
            continue;
        }

        std::string log;
        if (!compileGLES(PATCHED, log)) {
            std::cout << std::format("[FAIL] {}:\n{}\n", name, log);
            ++failures;
            if (argc > 2)
                std::cout << PATCHED << "\n";
            continue;
        }

        // sanity: is our call actually in there, and only once?
        const auto CALLS = [&] {
            size_t c = 0, p = 0;
            while ((p = PATCHED.find("hcApplyChroma(pixColor)", p)) != std::string::npos) {
                ++c;
                ++p;
            }
            return c;
        }();

        std::cout << std::format("[ ok ] {} ({} chars, {} call site)\n", name, PATCHED.length(), CALLS);
        if (CALLS != 1) {
            std::cout << std::format("       ^ expected exactly 1 call site\n");
            ++failures;
        }
    }

    // ext.frag has no feature variants
    {
        g_overrideDefines  = getDefines(0);
        const auto RAW     = processSource(readFile(g_shaderDir + "/ext.frag"));
        const auto PATCHED = patchChromaSource(RAW);
        std::string log;
        if (PATCHED.empty() || !compileGLES(PATCHED, log)) {
            std::cout << std::format("[FAIL] ext: {}\n", log.empty() ? "patch anchors not found" : log);
            ++failures;
        } else
            std::cout << std::format("[ ok ] ext ({} chars)\n", PATCHED.length());
    }

    std::cout << "\n" << (failures ? std::format("{} FAILURES", failures) : "all good") << "\n";
    return failures ? 1 : 0;
}
