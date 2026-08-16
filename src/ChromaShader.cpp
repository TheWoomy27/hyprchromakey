#include "ChromaShader.hpp"
#include "ChromaGLSL.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <vector>

using namespace Render;
using namespace Render::GL;

static uint32_t variantKey(ePreparedFragmentShader frag, ShaderFeatureFlags features) {
    return (sc<uint32_t>(frag) << 16) | sc<uint32_t>(features);
}

CChromaShaders::SVariant* CChromaShaders::compile(ePreparedFragmentShader frag, ShaderFeatureFlags features) {
    const auto KEY = variantKey(frag, features);

    if (const auto IT = m_variants.find(KEY); IT != m_variants.end())
        return &IT->second;

    if (m_broken.contains(KEY))
        return nullptr;

    const auto FAIL = [this, KEY](const std::string& why) -> SVariant* {
        chromaLog(Log::ERR, "{}, falling back to the stock shader", why);
        m_broken[KEY] = true;
        return nullptr;
    };

    if (!g_pShaderLoader || !g_pHyprOpenGL || !g_pHyprOpenGL->m_shaders)
        return FAIL("shader loader unavailable");

    const auto SRC = patchChromaSource(g_pShaderLoader->getVariantSource(frag, features));
    if (SRC.empty())
        return FAIL(std::format("could not patch shader {} (features {})", sc<int>(frag), features));

    SVariant variant;
    variant.shader = makeShared<CShader>();

    if (!variant.shader->createProgram(g_pHyprOpenGL->m_shaders->TEXVERTSRC, SRC, true, true))
        return FAIL(std::format("chromakey shader {} (features {}) failed to compile", sc<int>(frag), features));

    const auto PROG      = variant.shader->program();
    variant.locKeyCount  = glGetUniformLocation(PROG, "hcKeyCount");
    variant.locKeyColor  = glGetUniformLocation(PROG, "hcKeyColor");
    variant.locKeyParams = glGetUniformLocation(PROG, "hcKeyParams");
    variant.locMinAlpha  = glGetUniformLocation(PROG, "hcMinAlpha");

    if (variant.locKeyCount < 0 || variant.locKeyColor < 0 || variant.locKeyParams < 0 || variant.locMinAlpha < 0) {
        variant.shader->destroy();
        return FAIL(std::format("chromakey uniforms missing from shader {} (features {})", sc<int>(frag), features));
    }

    chromaLog(Log::INFO, "compiled chromakey variant for shader {}, features {}", sc<int>(frag), features);

    return &m_variants.emplace(KEY, std::move(variant)).first->second;
}

WP<CShader> CChromaShaders::get(ePreparedFragmentShader frag, ShaderFeatureFlags features, const SChromaProfile& profile) {
    auto* variant = compile(frag, features);
    if (!variant)
        return {};

    const bool STALE = variant->uploadedProfile != &profile || variant->uploadedGeneration != profile.generation;
    if (!STALE)
        return variant->shader;

    // uniforms live in the program object, so this only has to happen when the profile changes
    GLint previous = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
    glUseProgram(variant->shader->program());

    const auto        COUNT = std::min(profile.keys.size(), MAX_KEYS_PER_PROFILE);
    std::vector<GLfloat> colors(COUNT * 3), params(COUNT * 4);

    for (size_t i = 0; i < COUNT; ++i) {
        const auto& KEY   = profile.keys[i];
        colors[i * 3 + 0] = KEY.color[0];
        colors[i * 3 + 1] = KEY.color[1];
        colors[i * 3 + 2] = KEY.color[2];
        params[i * 4 + 0] = KEY.similarity;
        params[i * 4 + 1] = KEY.smoothness;
        params[i * 4 + 2] = KEY.opacity;
        params[i * 4 + 3] = sc<GLfloat>(KEY.mode);
    }

    glUniform1i(variant->locKeyCount, sc<GLint>(COUNT));
    glUniform1f(variant->locMinAlpha, profile.minAlpha);
    if (COUNT > 0) {
        glUniform3fv(variant->locKeyColor, sc<GLsizei>(COUNT), colors.data());
        glUniform4fv(variant->locKeyParams, sc<GLsizei>(COUNT), params.data());
    }

    glUseProgram(previous);

    variant->uploadedProfile    = &profile;
    variant->uploadedGeneration = profile.generation;

    return variant->shader;
}

void CChromaShaders::clear() {
    if (!m_variants.empty() && g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();

    for (auto& [key, variant] : m_variants) {
        if (variant.shader)
            variant.shader->destroy();
    }

    m_variants.clear();
    m_broken.clear();
}
