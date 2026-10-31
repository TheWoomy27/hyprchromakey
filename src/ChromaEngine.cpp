#include "ChromaEngine.hpp"
#include "ChromaShader.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRuleApplicator.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>

using namespace Render;
using namespace Render::GL;

static CFunctionHook* g_renderTextureHook         = nullptr;
static CFunctionHook* g_renderTextureInternalHook = nullptr;
static CFunctionHook* g_shaderVariantHook         = nullptr;
static CFunctionHook* g_windowOpaqueHook          = nullptr;

// Every surface draw comes through here, and it is the last place that still knows which surface
// (and therefore which window or layer) the texture belongs to. Decorations and other non-client
// textures come through with a null surface, which is exactly how we keep them opaque.
static void hkRenderTexture(void* thisptr, CRenderContext& ctx, SP<ITexture> tex, const CBox& box, CHyprOpenGLImpl::STextureRenderData data) {
    const auto PREVIOUS = g_chromaEngine.beginDraw(ctx.m_data.currentWindow.lock(), data.surface, data.currentLS.lock(), tex.get());
    ((decltype(&hkRenderTexture))g_renderTextureHook->m_original)(thisptr, ctx, tex, box, std::move(data));
    g_chromaEngine.endDraw(PREVIOUS);
}

// One renderTexture can turn into more than one draw: with blur on, hyprland first lays down the
// blurred backdrop and only then the window itself, both through here. Only the window's own
// texture gets keyed - keying the backdrop too would punch holes in the blur wherever the blurred
// wallpaper happens to come close to a key color.
static void hkRenderTextureInternal(void* thisptr, CRenderContext& ctx, SP<ITexture> tex, const CBox& box, const CHyprOpenGLImpl::STextureRenderData& data) {
    const auto PREVIOUS = g_chromaEngine.activate(tex.get());
    ((decltype(&hkRenderTextureInternal))g_renderTextureInternalHook->m_original)(thisptr, ctx, tex, box, data);
    g_chromaEngine.deactivate(PREVIOUS);
}

static WP<CShader> hkGetShaderVariant(void* thisptr, ePreparedFragmentShader frag, const SShaderVariant& variant) {
    const auto ORIGINAL = [&] { return ((decltype(&hkGetShaderVariant))g_shaderVariantHook->m_original)(thisptr, frag, variant); };

    if (frag != SH_FRAG_SURFACE && frag != SH_FRAG_EXT)
        return ORIGINAL();

    const auto PROFILE = g_chromaEngine.currentProfile();
    if (!PROFILE)
        return ORIGINAL();

    const auto PATCHED = g_chromaShaders.get(frag, variant, *PROFILE);
    return PATCHED ? PATCHED : ORIGINAL();
}

// Hyprland treats an opaque window as "nothing to composite behind": it skips blur, keeps
// blending off and even allows direct scanout. opaque() only looks at the window's own alpha,
// which is 1 at rest, so a keyed window would get blur while it animates and lose it once still.
static bool hkWindowOpaque(void* thisptr, const SWindowRenderPresentation& presentation) {
    if (g_chromaEngine.keyedAndTranslucent(g_chromaEngine.windowForPresentation(thisptr)))
        return false;

    return ((decltype(&hkWindowOpaque))g_windowOpaqueHook->m_original)(thisptr, presentation);
}

bool CChromaEngine::keyedAndTranslucent(PHLWINDOW window) {
    if (!window || !g_chromaConfig.enabled() || !g_chromaConfig.forceTranslucent())
        return false;

    return profileForWindow(window) != nullptr;
}

PHLWINDOW CChromaEngine::windowForPresentation(const void* presentation) {
    // the presentation keeps its window in a private member, so match it the long way round
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (&WINDOW->presentation() == presentation)
            return WINDOW;
    }

    return nullptr;
}

bool CChromaEngine::install(HANDLE handle) {
    m_windowEffect = Desktop::Rule::windowEffects()->registerEffect("plugin:chromakey");
    m_layerEffect  = Desktop::Rule::layerEffects()->registerEffect("plugin:chromakey");

    const auto HOOK = [this, handle](const char* name, const char* demangledPrefix, void* target) -> CFunctionHook* {
        const auto MATCHES = HyprlandAPI::findFunctionsByName(handle, name);
        const auto FOUND   = std::ranges::find_if(MATCHES, [demangledPrefix](const SFunctionMatch& m) { return m.demangled.starts_with(demangledPrefix); });

        if (FOUND == MATCHES.end()) {
            chromaLog(Log::ERR, "could not find {} to hook - this hyprland is probably newer than the plugin", demangledPrefix);
            return nullptr;
        }

        auto* hook = HyprlandAPI::createFunctionHook(handle, FOUND->address, target);
        if (!hook)
            return nullptr;

        if (!hook->hook()) {
            // hyprland allows exactly one hook per function, so this almost always means another
            // plugin got there first - most likely one that also recolors windows
            chromaLog(Log::ERR, "could not hook {} - another loaded plugin already hooks it. Hyprland allows only one hook per function, so the two cannot both run.",
                      demangledPrefix);
            HyprlandAPI::removeFunctionHook(handle, hook);
            m_collided = true;
            return nullptr;
        }

        return hook;
    };

    g_renderTextureHook         = HOOK("renderTexture", "Render::GL::CHyprOpenGLImpl::renderTexture(", (void*)&hkRenderTexture);
    g_renderTextureInternalHook = HOOK("renderTextureInternal", "Render::GL::CHyprOpenGLImpl::renderTextureInternal(", (void*)&hkRenderTextureInternal);
    g_shaderVariantHook =
        HOOK("getShaderVariant", "Render::GL::CHyprOpenGLImpl::getShaderVariant(Render::ePreparedFragmentShader, Render::SShaderVariant const&)", (void*)&hkGetShaderVariant);

    m_hooked = g_renderTextureHook && g_renderTextureInternalHook && g_shaderVariantHook;
    if (!m_hooked) {
        // half a render path does more harm than none: windows would be nudged translucent
        // without ever being keyed
        uninstall();
        return false;
    }

    // Not fatal: without it keying still works, blur just pops on and off during animations.
    g_windowOpaqueHook = HOOK("opaque", "Desktop::View::CWindowPresentation::opaque(Render::SWindowRenderPresentation const&)", (void*)&hkWindowOpaque);
    if (!g_windowOpaqueHook)
        chromaLog(Log::WARN, "keyed windows will still be treated as opaque, so blur may flicker during animations");

    return true;
}

void CChromaEngine::uninstall() {
    for (auto** hook : {&g_renderTextureHook, &g_renderTextureInternalHook, &g_shaderVariantHook, &g_windowOpaqueHook}) {
        if (*hook)
            (*hook)->unhook();
        *hook = nullptr;
    }

    m_hooked = false;

    restoreTranslucency();
    damageEverything();
}

void CChromaEngine::unregisterEffects() {
    Desktop::Rule::windowEffects()->unregisterEffect("plugin:chromakey");
    Desktop::Rule::layerEffects()->unregisterEffect("plugin:chromakey");
}

bool CChromaEngine::collided() const {
    return m_collided;
}

CChromaEngine::SDrawState CChromaEngine::beginDraw(PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLLS layer, const ITexture* texture) {
    auto previous = m_draw;
    m_draw        = {};

    if (!surface || !g_chromaConfig.enabled())
        return previous;

    SP<SChromaProfile> profile;

    if (window) {
        if (g_chromaConfig.mainSurfaceOnly() && (!window->wlSurface() || window->wlSurface()->resource() != surface))
            return previous;
        profile = profileForWindow(window);
    } else if (layer) {
        if (g_chromaConfig.mainSurfaceOnly() && (!layer->wlSurface() || layer->wlSurface()->resource() != surface))
            return previous;
        profile = profileForLayer(layer);
    }

    if (!profile)
        return previous;

    m_draw = {.profile = profile, .texture = texture};

    if (g_chromaConfig.forceTranslucent())
        applyTranslucency(surface, window, layer);

    return previous;
}

void CChromaEngine::endDraw(SDrawState previous) {
    m_draw = std::move(previous);
}

SP<SChromaProfile> CChromaEngine::activate(const ITexture* texture) {
    auto previous = m_active;
    m_active      = m_draw.texture && m_draw.texture == texture ? m_draw.profile : nullptr;
    return previous;
}

void CChromaEngine::deactivate(SP<SChromaProfile> previous) {
    m_active = std::move(previous);
}

SP<SChromaProfile> CChromaEngine::currentProfile() const {
    return m_active;
}

SP<SChromaProfile> CChromaEngine::resolveCached(const std::string& value) {
    const auto IT = m_resolveCache.find(value);
    if (IT != m_resolveCache.end())
        return IT->second;

    // resolveRule logs unknown profile names, so going through the cache keeps that to once
    // per distinct value per config reload instead of once per frame
    return m_resolveCache.emplace(value, g_chromaConfig.resolveRule(value)).first->second;
}

SP<SChromaProfile> CChromaEngine::profileForWindow(PHLWINDOW window) {
    if (!window)
        return nullptr;

    // a manual override is an explicit request, so it beats ignore_fullscreen too
    if (const auto IT = m_windowOverrides.find(window.get()); IT != m_windowOverrides.end())
        return resolveCached(IT->second);

    if (g_chromaConfig.ignoreFullscreen() && isFullscreen(window))
        return nullptr;

    if (window->m_ruleApplicator) {
        const auto& PROPS = window->m_ruleApplicator->m_otherProps.props;
        if (const auto IT = PROPS.find(m_windowEffect); IT != PROPS.end() && IT->second)
            return resolveCached(IT->second->effect);
    }

    return g_chromaConfig.defaultWindowProfile();
}

// true fullscreen only; a maximized window still shows the bar and gaps around it
bool CChromaEngine::isFullscreen(PHLWINDOW window) {
    return Fullscreen::controller() && Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
}

SP<SChromaProfile> CChromaEngine::profileForLayer(PHLLS layer) {
    if (!layer)
        return nullptr;

    if (layer->m_ruleApplicator) {
        const auto& PROPS = layer->m_ruleApplicator->m_otherProps.props;
        if (const auto IT = PROPS.find(m_layerEffect); IT != PROPS.end() && IT->second)
            return resolveCached(IT->second->effect);
    }

    return g_chromaConfig.defaultLayerProfile();
}

// Hyprland skips compositing (and blur) behind anything it believes is fully opaque, so a keyed
// surface has to be marked ever so slightly translucent or its transparent pixels would reveal
// nothing at all. The nudge is far below one 8-bit step, so it never changes what you see.
void CChromaEngine::applyTranslucency(SP<CWLSurfaceResource> surface, PHLWINDOW window, PHLLS layer) {
    const float TARGET = g_chromaConfig.translucency();
    if (TARGET >= 1.F)
        return;

    const auto WLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!WLSURFACE || WLSURFACE->m_alphaModifier != 1.F)
        return;

    WLSURFACE->m_alphaModifier = TARGET;
    m_translucent.emplace_back(STranslucent{.surface = WLSURFACE, .value = TARGET, .window = window, .layer = layer});

    // occlusion was already decided for this frame, so ask for one more
    if (const auto BOX = WLSURFACE->getSurfaceBoxGlobal(); BOX)
        m_pendingDamage.emplace_back(*BOX);
}

// Hands translucency back to surfaces whose window or layer is no longer keyed - toggled off,
// re-ruled, gone fullscreen with ignore_fullscreen. Left nudged they would keep paying for blur
// behind them and never qualify for direct scanout.
void CChromaEngine::releaseTranslucency() {
    std::erase_if(m_translucent, [this](const STranslucent& entry) {
        const auto WLSURFACE = entry.surface.lock();
        if (!WLSURFACE)
            return true;

        const bool KEYED = g_chromaConfig.enabled() && g_chromaConfig.forceTranslucent() &&
            (entry.window ? profileForWindow(entry.window.lock()) != nullptr : entry.layer && profileForLayer(entry.layer.lock()) != nullptr);
        if (KEYED)
            return false;

        // leave it alone if something else has taken it over since
        if (WLSURFACE->m_alphaModifier == entry.value) {
            WLSURFACE->m_alphaModifier = 1.F;
            if (const auto BOX = WLSURFACE->getSurfaceBoxGlobal(); BOX)
                m_pendingDamage.emplace_back(*BOX);
        }

        return true;
    });
}

void CChromaEngine::restoreTranslucency() {
    for (const auto& ENTRY : m_translucent) {
        const auto WLSURFACE = ENTRY.surface.lock();
        // leave it alone if something else has taken it over since
        if (WLSURFACE && WLSURFACE->m_alphaModifier == ENTRY.value)
            WLSURFACE->m_alphaModifier = 1.F;
    }

    m_translucent.clear();
}

void CChromaEngine::damageEverything() {
    if (!g_pHyprRenderer || !State::monitorState())
        return;

    for (const auto& MONITOR : State::monitorState()->monitors()) {
        g_pHyprRenderer->damageMonitor(MONITOR);
    }
}

// A client declares which parts of its surface are fully opaque. Hyprland trusts that: once a
// surface is being drawn at alpha >= 1 it inverts the declared region, finds nothing translucent
// left to composite behind, and switches blur off for it entirely (ElementRenderer, "amazing hack:
// the surface has an opaque region"). The same region drives occlusion culling.
//
// A keyed window's client has no idea we punch holes in it, so it goes on declaring itself fully
// opaque and both decisions come out wrong whenever alpha is back at 1 - including midway through
// an overshooting animation curve, where blur would drop out for the length of the overshoot.
//
// So the declared region is cleared for keyed surfaces, which is simply true of them. Clients
// re-declare it on their next commit, hence doing this every frame.
void CChromaEngine::clearOpaqueRegions() {
    if (!g_chromaConfig.enabled() || !g_chromaConfig.forceTranslucent())
        return;

    const auto CLEAR = [](SP<CWLSurfaceResource> root) {
        if (!root)
            return;

        root->breadthfirst([](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { surface->m_current.opaque = CRegion{}; }, nullptr);
    };

    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (WINDOW->mapped() && WINDOW->wlSurface() && profileForWindow(WINDOW))
            CLEAR(WINDOW->wlSurface()->resource());
    }

    for (const auto& LAYER : Desktop::layerState()->layers()) {
        if (LAYER->mapped() && LAYER->wlSurface() && profileForLayer(LAYER))
            CLEAR(LAYER->wlSurface()->resource());
    }
}

void CChromaEngine::onFrameBegin() {
    releaseTranslucency();
    clearOpaqueRegions();
}

void CChromaEngine::onFrameEnd() {
    if (m_pendingDamage.empty() || !g_pHyprRenderer)
        return;

    // swap out first: damaging can re-enter rendering on some paths
    const auto PENDING = std::exchange(m_pendingDamage, {});
    for (const auto& BOX : PENDING) {
        g_pHyprRenderer->damageBox(BOX);
    }
}

void CChromaEngine::onConfigCommitted() {
    m_resolveCache.clear();
    refreshAll();
}

// surfaces that are no longer keyed have to go back to being opaque; anything still keyed gets
// nudged again on its next draw. damageEverything also schedules a frame, which is what makes a
// change land on an idle screen instead of sitting there until something else redraws.
void CChromaEngine::refreshAll() {
    restoreTranslucency();
    damageEverything();
}

// An override rather than a config write, so the change lands at once instead of waiting for the
// next frame to notice it - see pollConfigChanges().
void CChromaEngine::setEnabled(std::optional<bool> value) {
    g_chromaConfig.setEnabledOverride(value);
    refreshAll();
}

void CChromaEngine::toggleAll() {
    setEnabled(!g_chromaConfig.enabled());
}

void CChromaEngine::onWindowGone(Desktop::View::CWindow* window) {
    if (window)
        m_windowOverrides.erase(window);
}

std::string CChromaEngine::setWindowOverride(PHLWINDOW window, const std::string& value) {
    if (!window)
        return "no such window";

    if (const auto CHECK = g_chromaConfig.checkRule(value); !CHECK)
        return CHECK.error();

    m_windowOverrides[window.get()] = value;

    if (g_pHyprRenderer)
        g_pHyprRenderer->damageWindow(window, true);

    return "";
}

std::string CChromaEngine::toggleWindow(PHLWINDOW window, const std::string& profile) {
    if (!window)
        return "no such window";

    const bool KEYED = profileForWindow(window) != nullptr;
    return setWindowOverride(window, KEYED ? "off" : (profile.empty() ? "on" : profile));
}

void CChromaEngine::clearOverrides() {
    m_windowOverrides.clear();
    g_chromaConfig.setEnabledOverride(std::nullopt);
    refreshAll();
}

static std::string jsonString(std::string_view in) {
    std::string out = "\"";
    for (const char C : in) {
        switch (C) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (sc<unsigned char>(C) < 0x20)
                    out += std::format("\\u{:04x}", sc<int>(C));
                else
                    out += C;
        }
    }
    return out + "\"";
}

static std::string hexColor(const std::array<float, 3>& c) {
    return std::format("0x{:02x}{:02x}{:02x}", sc<int>(c[0] * 255.F + 0.5F), sc<int>(c[1] * 255.F + 0.5F), sc<int>(c[2] * 255.F + 0.5F));
}

std::string CChromaEngine::status(bool json) {
    struct SKeyed {
        const char*        kind;
        uintptr_t          address;
        std::string        name;
        SP<SChromaProfile> profile;
    };

    std::vector<SKeyed> keyed;
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (const auto PROFILE = WINDOW->mapped() ? profileForWindow(WINDOW) : nullptr)
            keyed.emplace_back("window", rc<uintptr_t>(WINDOW.get()), WINDOW->metadata().appID(), PROFILE);
    }
    for (const auto& LAYER : Desktop::layerState()->layers()) {
        if (const auto PROFILE = LAYER->mapped() ? profileForLayer(LAYER) : nullptr)
            keyed.emplace_back("layer", rc<uintptr_t>(LAYER.get()), LAYER->m_namespace, PROFILE);
    }

    std::string out;

    if (json) {
        out += "{\n";
        out += std::format("  \"enabled\": {},\n", g_chromaConfig.enabled());
        out += std::format("  \"overridden\": {},\n", g_chromaConfig.enabledOverridden());
        out += std::format("  \"hooked\": {},\n", m_hooked);
        out += std::format("  \"ignoreFullscreen\": {},\n", g_chromaConfig.ignoreFullscreen());
        out += "  \"profiles\": [";

        bool first = true;
        for (const auto& [NAME, PROFILE] : g_chromaConfig.profiles()) {
            out += std::format("{}\n    {{\n      \"name\": {},\n      \"minAlpha\": {:.4f},\n      \"keys\": [", first ? "" : ",", jsonString(NAME), PROFILE->minAlpha);
            first = false;

            for (size_t i = 0; i < PROFILE->keys.size(); ++i) {
                const auto& KEY = PROFILE->keys[i];
                out += std::format("{}\n        {{\"color\": \"{}\", \"similarity\": {:.4f}, \"smoothness\": {:.4f}, \"opacity\": {:.4f}, \"match\": \"{}\", \"minRegion\": {}}}",
                                   i ? "," : "", hexColor(KEY.color), KEY.similarity, KEY.smoothness, KEY.opacity, matchModeToString(KEY.mode), KEY.minRegion);
            }

            out += "\n      ]\n    }";
        }

        out += "\n  ],\n  \"keyed\": [";
        for (size_t i = 0; i < keyed.size(); ++i) {
            out += std::format("{}\n    {{\"kind\": \"{}\", \"address\": \"0x{:x}\", \"name\": {}, \"profile\": {}}}", i ? "," : "", keyed[i].kind, keyed[i].address,
                               jsonString(keyed[i].name), jsonString(keyed[i].profile->name));
        }
        out += "\n  ]\n}\n";
        return out;
    }

    out += std::format("hyprchromakey: {}{}, hooks {}\n", g_chromaConfig.enabled() ? "enabled" : "disabled",
                       g_chromaConfig.enabledOverridden() ? " (runtime override; hl.plugin.hyprchromakey.reset() follows the config again)" : "",
                       m_hooked ? "installed" : "MISSING");

    if (g_chromaConfig.ignoreFullscreen())
        out += "fullscreen windows are left unkeyed\n";

    if (m_collided)
        out += "\nanother loaded plugin already hooks the render path, so hyprchromakey cannot run.\nhyprland allows one hook per function. Run `hyprctl plugin list` - any other plugin\nthat recolors or filters window pixels will take the same hooks.\n";

    if (g_chromaConfig.profiles().empty())
        out += "no key colors defined - set plugin:hyprchromakey:keys in your config\n";

    for (const auto& [NAME, PROFILE] : g_chromaConfig.profiles()) {
        out += std::format("\nprofile \"{}\" (min_alpha {:.3f})\n", NAME, PROFILE->minAlpha);
        for (const auto& KEY : PROFILE->keys) {
            out += std::format("  {}  similarity {:.3f}  smoothness {:.3f}  opacity {:.3f}  match {}{}\n", hexColor(KEY.color), KEY.similarity, KEY.smoothness, KEY.opacity,
                               matchModeToString(KEY.mode), KEY.minRegion > 0 ? std::format("  min_region {}", KEY.minRegion) : "");
        }
    }

    out += "\nkeyed right now:\n";
    for (const auto& K : keyed) {
        out += std::format("  {:<6} 0x{:x} [{}] -> {}\n", K.kind, K.address, K.name, K.profile->name);
    }
    if (keyed.empty())
        out += "  nothing\n";

    return out;
}
