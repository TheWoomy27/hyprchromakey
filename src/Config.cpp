#include "Config.hpp"

#include <bit>

#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprutils/string/String.hpp>

#include <algorithm>
#include <charconv>

using namespace Hyprutils::String;

std::optional<eMatchMode> matchModeFromString(const std::string& s) {
    if (s == "rgb")
        return MATCH_RGB;
    if (s == "hsv")
        return MATCH_HSV;
    if (s == "chroma" || s == "ycbcr")
        return MATCH_CHROMA;
    return std::nullopt;
}

const char* matchModeToString(eMatchMode m) {
    switch (m) {
        case MATCH_HSV: return "hsv";
        case MATCH_CHROMA: return "chroma";
        default: return "rgb";
    }
}

// splits on `sep`, ignoring separators nested in parentheses so that rgba(1, 2, 3, 4) survives
static std::vector<std::string> splitTopLevel(const std::string& str, char sep) {
    std::vector<std::string> out;
    size_t                   depth = 0, start = 0;

    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '(')
            ++depth;
        else if (str[i] == ')' && depth > 0)
            --depth;
        else if (str[i] == sep && depth == 0) {
            out.emplace_back(trim(str.substr(start, i - start)));
            start = i + 1;
        }
    }

    out.emplace_back(trim(str.substr(start)));
    std::erase_if(out, [](const auto& e) { return e.empty(); });
    return out;
}

static std::expected<float, std::string> parseUnitFloat(const std::string& field, const std::string& value) {
    float      f   = 0.F;
    const auto RES = std::from_chars(value.data(), value.data() + value.length(), f);

    if (RES.ec != std::errc{} || RES.ptr != value.data() + value.length())
        return std::unexpected(std::format("{}: \"{}\" is not a number", field, value));

    if (f < 0.F || f > 1.F)
        return std::unexpected(std::format("{}: {} is out of range, expected 0.0 - 1.0", field, f));

    return f;
}

static std::expected<std::array<float, 3>, std::string> parseKeyColor(const std::string& value) {
    // hyprland color syntax: #rgb, #rrggbb, 0xAARRGGBB, rgb(...), rgba(...)
    const auto RES = Config::ParserUtils::parseColor(value);
    if (RES)
        return std::array<float, 3>{
            sc<float>((*RES >> 16) & 0xFF) / 255.F,
            sc<float>((*RES >> 8) & 0xFF) / 255.F,
            sc<float>(*RES & 0xFF) / 255.F,
        };

    // bare "r, g, b" in 0 - 255
    const auto PARTS = splitTopLevel(value, ',');
    if (PARTS.size() == 3) {
        std::array<float, 3> col{};
        for (size_t i = 0; i < 3; ++i) {
            float      f   = 0.F;
            const auto NUM = std::from_chars(PARTS[i].data(), PARTS[i].data() + PARTS[i].length(), f);
            if (NUM.ec != std::errc{} || NUM.ptr != PARTS[i].data() + PARTS[i].length())
                return std::unexpected(RES.error());
            col[i] = std::clamp(f, 0.F, 255.F) / 255.F;
        }
        return col;
    }

    return std::unexpected(RES.error());
}

std::expected<SKeySpec, std::string> CChromaConfig::parseKeySpec(const std::string& value) {
    SKeySpec spec;
    bool     hasColor = false;

    for (const auto& FIELD : splitTopLevel(value, ',')) {
        const auto SPACE = FIELD.find(' ');

        // a lone field is taken as the color, so `chromakey = rgb(11111b)` just works
        if (SPACE == std::string::npos) {
            const auto COL = parseKeyColor(FIELD);
            if (!COL)
                return std::unexpected(std::format("invalid field \"{}\": {}", FIELD, COL.error()));
            spec.color = *COL;
            hasColor   = true;
            continue;
        }

        const auto NAME = FIELD.substr(0, SPACE);
        const auto VAL  = trim(FIELD.substr(SPACE + 1));

        if (NAME == "color") {
            const auto COL = parseKeyColor(VAL);
            if (!COL)
                return std::unexpected(COL.error());
            spec.color = *COL;
            hasColor   = true;
        } else if (NAME == "similarity") {
            const auto F = parseUnitFloat(NAME, VAL);
            if (!F)
                return std::unexpected(F.error());
            spec.similarity = *F;
        } else if (NAME == "smoothness") {
            const auto F = parseUnitFloat(NAME, VAL);
            if (!F)
                return std::unexpected(F.error());
            spec.smoothness = *F;
        } else if (NAME == "opacity") {
            const auto F = parseUnitFloat(NAME, VAL);
            if (!F)
                return std::unexpected(F.error());
            spec.opacity = *F;
        } else if (NAME == "match") {
            const auto M = matchModeFromString(VAL);
            if (!M)
                return std::unexpected(std::format("match: \"{}\" is not one of rgb, hsv, chroma", VAL));
            spec.mode = *M;
        } else if (NAME == "profile") {
            if (VAL.empty())
                return std::unexpected("profile: name may not be empty");
            spec.profile = VAL;
        } else
            return std::unexpected(std::format("unknown field \"{}\"", NAME));
    }

    if (!hasColor)
        return std::unexpected("missing a color");

    return spec;
}

std::expected<void, std::string> CChromaConfig::addKeySpec(const std::string& value) {
    const auto SPEC = parseKeySpec(value);
    if (!SPEC)
        return std::unexpected(SPEC.error());

    m_specs.emplace_back(*SPEC);
    return {};
}

void CChromaConfig::clearSpecs() {
    m_specs.clear();
}

void CChromaConfig::commit() {
    ++m_generation;
    m_profiles.clear();

    if (!m_registered)
        return;

    auto specs = m_specs;

    // the `keys` value holds `;`-separated entries, for setups that can't use keywords
    for (const auto& ENTRY : splitTopLevel(m_keys->value(), ';')) {
        const auto SPEC = parseKeySpec(ENTRY);
        if (SPEC)
            specs.emplace_back(*SPEC);
        else
            chromaLog(Log::ERR, "plugin:hyprchromakey:keys: {}", SPEC.error());
    }

    const auto GLOBALMODE = matchModeFromString(m_match->value());
    if (!GLOBALMODE)
        chromaLog(Log::ERR, "plugin:hyprchromakey:match: \"{}\" is not one of rgb, hsv, chroma", m_match->value());

    const float MINALPHA = std::clamp(sc<float>(m_minAlpha->value()), 0.F, 1.F);

    for (const auto& SPEC : specs) {
        auto& profile = m_profiles[SPEC.profile];
        if (!profile) {
            profile             = makeShared<SChromaProfile>();
            profile->name       = SPEC.profile;
            profile->minAlpha   = MINALPHA;
            profile->generation = m_generation;
        }

        if (profile->keys.size() >= MAX_KEYS_PER_PROFILE) {
            chromaLog(Log::ERR, "profile \"{}\" has more than {} keys, ignoring the rest", SPEC.profile, MAX_KEYS_PER_PROFILE);
            continue;
        }

        profile->keys.emplace_back(SChromaKey{
            .color      = SPEC.color,
            .similarity = SPEC.similarity.value_or(std::clamp(sc<float>(m_similarity->value()), 0.F, 1.F)),
            .smoothness = SPEC.smoothness.value_or(std::clamp(sc<float>(m_smoothness->value()), 0.F, 1.F)),
            .opacity    = SPEC.opacity.value_or(std::clamp(sc<float>(m_opacity->value()), 0.F, 1.F)),
            .mode       = SPEC.mode.value_or(GLOBALMODE.value_or(MATCH_RGB)),
        });
    }

    m_defaultWindows = resolveRule(m_defaultWindowsVal->value());
    m_defaultLayers  = resolveRule(m_defaultLayersVal->value());

    chromaLog(Log::INFO, "config: {} profile(s), {} key(s), enabled={}", m_profiles.size(), specs.size(), m_enabled->value());
}

SP<SChromaProfile> CChromaConfig::profile(const std::string& name) const {
    const auto IT = m_profiles.find(name);
    return IT == m_profiles.end() ? nullptr : IT->second;
}

SP<SChromaProfile> CChromaConfig::resolveRule(const std::string& value) const {
    const auto VAL = trim(value);

    if (VAL.empty() || VAL == "0" || VAL == "off" || VAL == "false" || VAL == "no" || VAL == "disable" || VAL == "disabled")
        return nullptr;

    if (VAL == "1" || VAL == "on" || VAL == "true" || VAL == "yes" || VAL == "enable" || VAL == "enabled") {
        const auto DEFAULT = profile("default");
        if (!DEFAULT)
            chromaLog(Log::ERR, "\"{}\" wants the default profile, but no key colors are defined outside of a named profile", VAL);
        return DEFAULT;
    }

    const auto PROFILE = profile(VAL);
    if (!PROFILE)
        chromaLog(Log::ERR, "no chromakey profile named \"{}\" is defined", VAL);

    return PROFILE;
}

SP<SChromaProfile> CChromaConfig::defaultWindowProfile() const {
    return m_defaultWindows;
}

SP<SChromaProfile> CChromaConfig::defaultLayerProfile() const {
    return m_defaultLayers;
}

const std::unordered_map<std::string, SP<SChromaProfile>>& CChromaConfig::profiles() const {
    return m_profiles;
}

uint64_t CChromaConfig::fingerprint() const {
    if (!m_registered)
        return 0;

    uint64_t hash = 0;
    const auto MIX = [&hash](uint64_t v) { hash = (hash ^ (v + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2))); };
    const auto NUM = [&MIX](float f) { MIX(std::bit_cast<uint32_t>(f)); };
    const auto STR = [&MIX](const std::string& v) { MIX(std::hash<std::string_view>{}(v)); };

    // deliberately the raw value, not enabled(): a runtime override is not a config change and
    // must not drag the whole config through a rebuild
    MIX(m_enabled->value());
    MIX(m_mainSurfaceOnly->value());
    MIX(m_forceTranslucent->value());
    NUM(m_similarity->value());
    NUM(m_smoothness->value());
    NUM(m_opacity->value());
    NUM(m_minAlpha->value());
    NUM(m_translucency->value());
    STR(m_match->value());
    STR(m_defaultWindowsVal->value());
    STR(m_defaultLayersVal->value());
    STR(m_keys->value());

    return hash;
}

void CChromaConfig::setEnabledOverride(std::optional<bool> value) {
    m_enabledOverride = value;
}

bool CChromaConfig::enabledOverridden() const {
    return m_enabledOverride.has_value();
}

bool CChromaConfig::enabled() const {
    // if registration failed the values are unbound and reading them would crash, so stay out of
    // the way entirely
    if (!m_registered)
        return false;

    return m_enabledOverride.value_or(m_enabled->value());
}

bool CChromaConfig::forceTranslucent() const {
    return m_forceTranslucent->value();
}

bool CChromaConfig::mainSurfaceOnly() const {
    return m_mainSurfaceOnly->value();
}

float CChromaConfig::translucency() const {
    return std::clamp(sc<float>(m_translucency->value()), 0.F, 1.F);
}

void CChromaConfig::registerConfig(HANDLE handle) {
    using namespace Config::Values;

    const auto ADD = [this, handle](const auto& value) {
        if (!HyprlandAPI::addConfigValueV2(handle, value)) {
            chromaLog(Log::ERR, "hyprland rejected config value {}", value->name());
            m_registered = false;
        }
        return value;
    };

    const SFloatValueOptions UNIT{.min = 0.F, .max = 1.F};

    m_enabled    = ADD(makeShared<CBoolValue>("plugin:hyprchromakey:enabled", "master switch for the chromakey effect", true));
    m_similarity = ADD(makeShared<CFloatValue>("plugin:hyprchromakey:similarity", "default distance below which a pixel is fully keyed", 0.08F, SFloatValueOptions{UNIT}));
    m_smoothness = ADD(makeShared<CFloatValue>("plugin:hyprchromakey:smoothness", "default fade band above the similarity threshold", 0.02F, SFloatValueOptions{UNIT}));
    m_opacity    = ADD(makeShared<CFloatValue>("plugin:hyprchromakey:opacity", "default alpha given to fully keyed pixels", 0.F, SFloatValueOptions{UNIT}));
    m_minAlpha   = ADD(makeShared<CFloatValue>("plugin:hyprchromakey:min_alpha", "only key pixels whose source alpha is at least this", 0.99F, SFloatValueOptions{UNIT}));
    m_match      = ADD(makeShared<CStringValue>("plugin:hyprchromakey:match", "default color comparison: rgb, hsv or chroma", "rgb"));
    m_keys       = ADD(makeShared<CStringValue>("plugin:hyprchromakey:keys", "`;`-separated key definitions, same syntax as the chromakey keyword", ""));

    m_defaultWindowsVal = ADD(makeShared<CStringValue>("plugin:hyprchromakey:default_windows", "profile applied to windows without a matching rule: off, on or a profile name", "off"));
    m_defaultLayersVal  = ADD(makeShared<CStringValue>("plugin:hyprchromakey:default_layers", "profile applied to layers without a matching rule: off, on or a profile name", "off"));

    m_forceTranslucent = ADD(makeShared<CBoolValue>("plugin:hyprchromakey:force_translucent", "mark keyed surfaces translucent so hyprland composites and blurs behind them", true));
    m_mainSurfaceOnly  = ADD(makeShared<CBoolValue>("plugin:hyprchromakey:main_surface_only", "only key the main surface, leaving popups and subsurfaces untouched", false));
    m_translucency     = ADD(makeShared<CFloatValue>("plugin:hyprchromakey:translucency", "alpha used by force_translucent. 1.0 disables the nudge", 0.9995F, SFloatValueOptions{UNIT}));
}
