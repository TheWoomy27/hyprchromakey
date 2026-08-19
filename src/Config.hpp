#pragma once

#include "ChromaGLSL.hpp"
#include "Common.hpp"

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

#include <array>
#include <expected>
#include <optional>
#include <unordered_map>
#include <vector>

// how a pixel is compared against a key color
enum eMatchMode : uint8_t {
    MATCH_RGB = 0, // largest per-channel difference. predictable, good for flat UI colors
    MATCH_HSV,     // hue/saturation/value difference. tolerates shading of the same hue
    MATCH_CHROMA,  // hue and saturation only, ignoring how light or dark the pixel is
    MATCH_LAST,
};

std::optional<eMatchMode> matchModeFromString(const std::string& s);
const char*               matchModeToString(eMatchMode m);

// one key color, fully resolved (globals already folded in)
struct SChromaKey {
    std::array<float, 3> color      = {0.F, 0.F, 0.F};
    float                similarity = 0.08F;
    float                smoothness = 0.02F;
    float                opacity    = 0.F;
    eMatchMode           mode       = MATCH_RGB;
};

// a key as written in the config: unset fields fall back to the globals at commit time
struct SKeySpec {
    std::string               profile = "default";
    std::array<float, 3>      color   = {0.F, 0.F, 0.F};
    std::optional<float>      similarity, smoothness, opacity;
    std::optional<eMatchMode> mode;
};

struct SChromaProfile {
    std::string             name;
    std::vector<SChromaKey> keys;
    float                   minAlpha = 0.99F;
    // bumped on every rebuild, so shaders know their uploaded uniforms are stale
    uint64_t generation = 0;
};


class CChromaConfig {
  public:
    // registers all config values + keywords. Only valid inside pluginInit.
    void registerConfig(HANDLE handle);

    // config keyword parsing. Errors are surfaced to the user by the caller.
    std::expected<void, std::string> addKeySpec(const std::string& value);

    static std::expected<SKeySpec, std::string> parseKeySpec(const std::string& value);

    void                             clearSpecs(); // on preReload
    void                             commit();     // on reloaded: fold globals into specs, build profiles

    SP<SChromaProfile>               profile(const std::string& name) const;
    // resolves a window/layer rule value: on/off/<profile name>. nullptr means "not keyed".
    SP<SChromaProfile>                                                 resolveRule(const std::string& value) const;

    SP<SChromaProfile>                                                 defaultWindowProfile() const;
    SP<SChromaProfile>                                                 defaultLayerProfile() const;

    const std::unordered_map<std::string, SP<SChromaProfile>>&         profiles() const;

    // A cheap hash of every value we care about. `hyprctl eval` and `hyprctl keyword` change config
    // values without emitting a reload or any other signal, so this is compared each frame to
    // notice changes that way. See pollConfigChanges() for what that does and does not catch.
    uint64_t                                                           fingerprint() const;

    // Runtime master switch, layered over plugin:hyprchromakey:enabled. nullopt follows the config.
    // Unlike a config edit this is ours, so we can damage the screen the moment it changes instead
    // of waiting to notice.
    void                                                               setEnabledOverride(std::optional<bool> value);
    bool                                                               enabledOverridden() const;

    bool                                                               enabled() const;
    bool                                                               forceTranslucent() const;
    bool                                                               mainSurfaceOnly() const;
    float                                                              translucency() const;

  private:
    std::vector<SKeySpec>                               m_specs;
    std::unordered_map<std::string, SP<SChromaProfile>> m_profiles;
    SP<SChromaProfile>                                  m_defaultWindows, m_defaultLayers;
    std::optional<bool>                                 m_enabledOverride;
    uint64_t                                            m_generation = 0;
    bool                                                m_registered = true;

    SP<Config::Values::CBoolValue>                      m_enabled, m_forceTranslucent, m_mainSurfaceOnly;
    SP<Config::Values::CFloatValue>                     m_similarity, m_smoothness, m_opacity, m_minAlpha, m_translucency;
    SP<Config::Values::CStringValue>                    m_match, m_defaultWindowsVal, m_defaultLayersVal, m_keys;
};

inline CChromaConfig g_chromaConfig;
