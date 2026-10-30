#include "ChromaShader.hpp"
#include "ChromaGLSL.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <vector>

using namespace Render;
using namespace Render::GL;

CChromaShaders::SVariant* CChromaShaders::compile(ePreparedFragmentShader frag, const SShaderVariant& shaderVariant) {
    const VariantKey KEY{frag, shaderVariant};

    if (const auto IT = m_variants.find(KEY); IT != m_variants.end())
        return &IT->second;

    if (m_broken.contains(KEY))
        return nullptr;

    const auto NAME = std::format("shader {} (features {}, tf {} -> {})", sc<int>(frag), shaderVariant.features, sc<int>(shaderVariant.sourceTF), sc<int>(shaderVariant.targetTF));

    const auto FAIL = [this, &KEY](const std::string& why) -> SVariant* {
        chromaLog(Log::ERR, "{}, falling back to the stock shader", why);
        m_broken.insert(KEY);
        return nullptr;
    };

    if (!g_pShaderLoader || !g_pHyprOpenGL || !g_pHyprOpenGL->m_shaders)
        return FAIL("shader loader unavailable");

    const SChromaShaderTraits TRAITS{
        .rgba       = (shaderVariant.features & SH_FEAT_RGBA) != 0,
        .regionTest = frag == SH_FRAG_SURFACE,
    };

    const auto SRC = patchChromaSource(g_pShaderLoader->getVariantSource(frag, shaderVariant), TRAITS);
    if (SRC.empty())
        return FAIL(std::format("could not patch {}", NAME));

    SVariant variant;
    variant.shader = makeShared<CShader>();

    if (!variant.shader->createProgram(g_pHyprOpenGL->m_shaders->TEXVERTSRC, SRC, true, true))
        return FAIL(std::format("chromakey {} failed to compile", NAME));

    const auto PROG      = variant.shader->program();
    variant.locKeyCount  = glGetUniformLocation(PROG, "hcKeyCount");
    variant.locKeyColor  = glGetUniformLocation(PROG, "hcKeyColor");
    variant.locKeyParams = glGetUniformLocation(PROG, "hcKeyParams");
    variant.locMinAlpha  = glGetUniformLocation(PROG, "hcMinAlpha");

    if (variant.locKeyCount < 0 || variant.locKeyColor < 0 || variant.locKeyParams < 0 || variant.locMinAlpha < 0) {
        variant.shader->destroy();
        return FAIL(std::format("chromakey uniforms missing from {}", NAME));
    }

    chromaLog(Log::INFO, "compiled chromakey variant for {}", NAME);

    return &m_variants.emplace(KEY, std::move(variant)).first->second;
}

WP<CShader> CChromaShaders::get(ePreparedFragmentShader frag, const SShaderVariant& shaderVariant, const SChromaProfile& profile) {
    auto* variant = compile(frag, shaderVariant);
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
    std::vector<GLfloat> colors(COUNT * 4), params(COUNT * 4);

    for (size_t i = 0; i < COUNT; ++i) {
        const auto& KEY   = profile.keys[i];
        colors[i * 4 + 0] = KEY.color[0];
        colors[i * 4 + 1] = KEY.color[1];
        colors[i * 4 + 2] = KEY.color[2];
        colors[i * 4 + 3] = sc<GLfloat>(KEY.minRegion);
        params[i * 4 + 0] = KEY.similarity;
        params[i * 4 + 1] = KEY.smoothness;
        params[i * 4 + 2] = KEY.opacity;
        params[i * 4 + 3] = sc<GLfloat>(KEY.mode);
    }

    glUniform1i(variant->locKeyCount, sc<GLint>(COUNT));
    glUniform1f(variant->locMinAlpha, profile.minAlpha);
    if (COUNT > 0) {
        glUniform4fv(variant->locKeyColor, sc<GLsizei>(COUNT), colors.data());
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
