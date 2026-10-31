#pragma once

#include "Common.hpp"
#include "ChromaGLSL.hpp"
#include "Config.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/ShaderLoader.hpp>

#include <map>
#include <set>

// Compiles and caches chromakey-patched copies of hyprland's own surface shaders.
//
// Hyprland builds a fragment shader per (shader, variant) pair on demand. We take the exact
// source it would have used, splice our key test in right after the texture sample, and hand the
// result back from the getShaderVariant hook. Everything downstream - color management, rounding,
// blur blending, tint - keeps working because it is still hyprland's shader.
class CChromaShaders {
  public:
    // Returns a patched variant with `profile`'s uniforms loaded, or nullptr if it could not be
    // built - in which case the caller must fall back to the stock shader.
    WP<CShader> get(Render::ePreparedFragmentShader frag, const Render::SShaderVariant& variant, const SChromaProfile& profile);

    // Drops every compiled variant. Needs a current EGL context.
    void        clear();

  private:
    struct SVariant {
        SP<CShader> shader;
        GLint       locKeyCount = -1, locKeyColor = -1, locKeyParams = -1, locMinAlpha = -1;
        // (profile generation, profile pointer) whose values are currently in the program
        uint64_t    uploadedGeneration = 0;
        const void* uploadedProfile    = nullptr;
    };

    using VariantKey = std::pair<Render::ePreparedFragmentShader, Render::SShaderVariant>;

    SVariant*                      compile(Render::ePreparedFragmentShader frag, const Render::SShaderVariant& variant);

    std::map<VariantKey, SVariant> m_variants;
    // shaders we already tried and failed to build, so we don't recompile every frame
    std::set<VariantKey>           m_broken;
};

inline CChromaShaders g_chromaShaders;
