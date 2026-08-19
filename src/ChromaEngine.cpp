#include "ChromaEngine.hpp"
#include "ChromaShader.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRuleApplicator.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>

using namespace Render;
using namespace Render::GL;

static CFunctionHook* g_renderTextureHook = nullptr;
static CFunctionHook* g_shaderVariantHook = nullptr;
static CFunctionHook* g_windowOpaqueHook = nullptr;

// Wraps every texture draw so we know which surface (and therefore which window or layer) the
// shader about to be picked belongs to. Decorations and other non-client textures come through
// here with a null surface, which is exactly how we keep them opaque.
static void hkRenderTexture(void* thisptr, SP<ITexture> tex, const CBox& box, CHyprOpenGLImpl::STextureRenderData data) {
    const auto PREVIOUS = g_chromaEngine.beginDraw(data.surface, data.currentLS.lock());
    ((decltype(&hkRenderTexture))g_renderTextureHook->m_original)(thisptr, tex, box, data);
    g_chromaEngine.endDraw(PREVIOUS);
}

static WP<CShader> hkGetShaderVariant(void* thisptr, ePreparedFragmentShader frag, ShaderFeatureFlags features) {
    const auto ORIGINAL = [&] { return ((decltype(&hkGetShaderVariant))g_shaderVariantHook->m_original)(thisptr, frag, features); };

    if (frag != SH_FRAG_SURFACE && frag != SH_FRAG_EXT)
        return ORIGINAL();

    const auto PROFILE = g_chromaEngine.currentProfile();
    if (!PROFILE)
        return ORIGINAL();

    const auto PATCHED = g_chromaShaders.get(frag, features, *PROFILE);
    return PATCHED ? PATCHED : ORIGINAL();
}

// Hyprland treats an opaque window as "nothing to composite behind": it skips blur, keeps
// blending off and even allows direct scanout. A keyed window is none of those things, but
// nothing in CWindow::opaque() knows that - it reads the window's own alpha, which is 1 once the
// open/move animation finishes. That is why blur appeared during transitions and vanished at rest.
static bool hkWindowOpaque(void* thisptr) {
    if (g_chromaEngine.keyedAndTranslucent(sc<Desktop::View::CWindow*>(thisptr)))
        return false;

    return ((decltype(&hkWindowOpaque))g_windowOpaqueHook->m_original)(thisptr);
}

bool CChromaEngine::keyedAndTranslucent(Desktop::View::CWindow* window) {
    if (!g_chromaConfig.enabled() || !g_chromaConfig.forceTranslucent())
        return false;

    const auto PROFILE = profileForWindow(window);
    return PROFILE && !PROFILE->keys.empty();
}

bool CChromaEngine::install(HANDLE handle) {
    m_windowEffect = Desktop::Rule::windowEffects()->registerEffect("plugin:chromakey");
    m_layerEffect  = Desktop::Rule::layerEffects()->registerEffect("plugin:chromakey");

    const auto HOOK = [this, handle](const char* name, const char* demangledPrefix, void* target) -> CFunctionHook* {
        const auto MATCHES = HyprlandAPI::findFunctionsByName(handle, name);
        const auto FOUND   = std::ranges::find_if(MATCHES, [demangledPrefix](const SFunctionMatch& m) { return m.demangled.starts_with(demangledPrefix); });

        if (FOUND == MATCHES.end()) {
            chromaLog(Log::ERR, "could not find {} to hook", demangledPrefix);
            return nullptr;
        }

        auto* hook = HyprlandAPI::createFunctionHook(handle, FOUND->address, target);
        if (!hook || !hook->hook()) {
            // hyprland allows exactly one hook per function, so this almost always means another
            // plugin got there first - most likely one that also recolors windows
            chromaLog(Log::ERR, "could not hook {} - another loaded plugin already hooks it. Hyprland allows only one hook per function, so the two cannot both run.",
                      demangledPrefix);
            m_collided = true;
            return nullptr;
        }

        return hook;
    };

    g_renderTextureHook = HOOK("renderTexture", "Render::GL::CHyprOpenGLImpl::renderTexture(", (void*)&hkRenderTexture);
    g_shaderVariantHook = HOOK("getShaderVariant", "Render::GL::CHyprOpenGLImpl::getShaderVariant(", (void*)&hkGetShaderVariant);

    // Not fatal: without it keying still works, blur just pops on and off during animations.
    g_windowOpaqueHook = HOOK("opaque", "Desktop::View::CWindow::opaque(", (void*)&hkWindowOpaque);
    if (!g_windowOpaqueHook)
        chromaLog(Log::WARN, "keyed windows will still be treated as opaque, so blur may flicker during animations");

    m_hooked = g_renderTextureHook && g_shaderVariantHook;
    return m_hooked;
}

void CChromaEngine::uninstall() {
    if (g_renderTextureHook)
        g_renderTextureHook->unhook();
    if (g_shaderVariantHook)
        g_shaderVariantHook->unhook();
    if (g_windowOpaqueHook)
        g_windowOpaqueHook->unhook();

    g_renderTextureHook = nullptr;
    g_shaderVariantHook = nullptr;
    g_windowOpaqueHook  = nullptr;
    m_hooked            = false;

    restoreTranslucency();
    damageEverything();

    Desktop::Rule::windowEffects()->unregisterEffect("plugin:chromakey");
    Desktop::Rule::layerEffects()->unregisterEffect("plugin:chromakey");
}

bool CChromaEngine::hooked() const {
    return m_hooked;
}

bool CChromaEngine::collided() const {
    return m_collided;
}

SP<SChromaProfile> CChromaEngine::beginDraw(SP<CWLSurfaceResource> surface, PHLLS layer) {
    auto previous = m_drawProfile;
    m_drawProfile.reset();

    if (!surface || !g_chromaConfig.enabled())
        return previous;

    const auto WINDOW = g_pHyprRenderer ? g_pHyprRenderer->m_renderData.currentWindow.lock() : nullptr;

    if (WINDOW) {
        if (g_chromaConfig.mainSurfaceOnly() && (!WINDOW->wlSurface() || WINDOW->wlSurface()->resource() != surface))
            return previous;
        m_drawProfile = profileForWindow(WINDOW);
    } else if (layer) {
        if (g_chromaConfig.mainSurfaceOnly() && (!layer->wlSurface() || layer->wlSurface()->resource() != surface))
            return previous;
        m_drawProfile = profileForLayer(layer);
    } else
        return previous;

    if (!m_drawProfile || m_drawProfile->keys.empty()) {
        m_drawProfile.reset();
        return previous;
    }

    if (g_chromaConfig.forceTranslucent())
        applyTranslucency(surface);

    return previous;
}

void CChromaEngine::endDraw(SP<SChromaProfile> previous) {
    m_drawProfile = std::move(previous);
}

SP<SChromaProfile> CChromaEngine::currentProfile() const {
    return m_drawProfile;
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
    return profileForWindow(window.get());
}

SP<SChromaProfile> CChromaEngine::profileForWindow(Desktop::View::CWindow* window) {
    if (!window)
        return nullptr;

    if (const auto IT = m_windowOverrides.find(window); IT != m_windowOverrides.end())
        return resolveCached(IT->second);

    if (window->m_ruleApplicator) {
        const auto& PROPS = window->m_ruleApplicator->m_otherProps.props;
        if (const auto IT = PROPS.find(m_windowEffect); IT != PROPS.end() && IT->second)
            return resolveCached(IT->second->effect);
    }

    return g_chromaConfig.defaultWindowProfile();
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
//
// Note this does not survive an animation whose curve overshoots past 1.0 - the product it keeps
// under 1 gets scaled back over by the overshoot. See "Known issue" in the README.
void CChromaEngine::applyTranslucency(SP<CWLSurfaceResource> surface) {
    const float TARGET = g_chromaConfig.translucency();
    if (TARGET >= 1.F)
        return;

    const auto WLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!WLSURFACE || WLSURFACE->m_alphaModifier != 1.F)
        return;

    WLSURFACE->m_alphaModifier = TARGET;

    // surfaces come and go; drop the dead ones instead of tracking them forever
    if (m_translucent.size() > 128)
        std::erase_if(m_translucent, [](const auto& e) { return !e.first; });

    m_translucent.emplace_back(WLSURFACE, TARGET);

    // occlusion was already decided for this frame, so ask for one more
    if (const auto BOX = WLSURFACE->getSurfaceBoxGlobal(); BOX)
        m_pendingDamage.emplace_back(*BOX);
}

void CChromaEngine::restoreTranslucency() {
    for (const auto& [SURFACE, VALUE] : m_translucent) {
        const auto WLSURFACE = SURFACE.lock();
        // leave it alone if something else has taken it over since
        if (WLSURFACE && WLSURFACE->m_alphaModifier == VALUE)
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
// A keyed window's client has no idea we are about to punch holes in it, so it goes on declaring
// itself fully opaque and both of those decisions come out wrong. It only shows up once something
// pushes alpha back to 1 mid-animation - an overshoot curve does exactly that on its way past the
// target - which is why blur would snap off for precisely the length of the overshoot.
//
// So the declared region gets cleared for keyed surfaces, which is simply true of them. Clients
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
        if (WINDOW->m_isMapped && WINDOW->wlSurface() && profileForWindow(WINDOW))
            CLEAR(WINDOW->wlSurface()->resource());
    }

    for (const auto& LAYER : Desktop::layerState()->layers()) {
        if (LAYER->m_mapped && LAYER->wlSurface() && profileForLayer(LAYER))
            CLEAR(LAYER->wlSurface()->resource());
    }
}

void CChromaEngine::onFrameEnd() {
    if (m_pendingDamage.empty() || !g_pHyprRenderer)
        return;

    for (const auto& BOX : m_pendingDamage) {
        g_pHyprRenderer->damageBox(BOX);
    }

    m_pendingDamage.clear();
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

// Flipping plugin:hyprchromakey:enabled through `hyprctl keyword`/`eval` works, but only becomes
// visible on the next frame the compositor happens to draw - see pollConfigChanges(). Going through
// our own override instead means the toggle takes effect at once, on every window, whether or not
// any of them is focused or drawing.
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

std::string CChromaEngine::status(bool json) {
    std::string out;

    if (json) {
        out += "{\n";
        out += std::format("  \"enabled\": {},\n", g_chromaConfig.enabled());
        out += std::format("  \"overridden\": {},\n", g_chromaConfig.enabledOverridden());
        out += std::format("  \"hooked\": {},\n", m_hooked);
        out += "  \"profiles\": [\n";

        bool first = true;
        for (const auto& [NAME, PROFILE] : g_chromaConfig.profiles()) {
            if (!first)
                out += ",\n";
            first = false;
            out += std::format("    {{\n      \"name\": \"{}\",\n      \"minAlpha\": {:.4f},\n      \"keys\": [\n", NAME, PROFILE->minAlpha);

            for (size_t i = 0; i < PROFILE->keys.size(); ++i) {
                const auto& KEY = PROFILE->keys[i];
                out += std::format("        {{\"color\": \"0x{:02x}{:02x}{:02x}\", \"similarity\": {:.4f}, \"smoothness\": {:.4f}, \"opacity\": {:.4f}, \"match\": \"{}\"}}{}\n",
                                   sc<int>(KEY.color[0] * 255.F + 0.5F), sc<int>(KEY.color[1] * 255.F + 0.5F), sc<int>(KEY.color[2] * 255.F + 0.5F), KEY.similarity, KEY.smoothness,
                                   KEY.opacity, matchModeToString(KEY.mode), i + 1 == PROFILE->keys.size() ? "" : ",");
            }

            out += "      ]\n    }";
        }

        out += "\n  ]\n}\n";
        return out;
    }

    out += std::format("hyprchromakey: {}{}, hooks {}\n", g_chromaConfig.enabled() ? "enabled" : "disabled",
                       g_chromaConfig.enabledOverridden() ? " (runtime override, `chromakey:reset` to follow the config again)" : "", m_hooked ? "installed" : "MISSING");

    if (m_collided)
        out += "\nanother loaded plugin already hooks the render path, so hyprchromakey cannot run.\nhyprland allows one hook per function. Run `hyprctl plugin list` - any other plugin\nthat recolors or filters window pixels will take the same hooks.\n";

    if (g_chromaConfig.profiles().empty())
        out += "no profiles defined - add a `chromakey = <color>` line to your config\n";

    for (const auto& [NAME, PROFILE] : g_chromaConfig.profiles()) {
        out += std::format("\nprofile \"{}\" (min_alpha {:.3f})\n", NAME, PROFILE->minAlpha);
        for (const auto& KEY : PROFILE->keys) {
            out += std::format("  0x{:02x}{:02x}{:02x}  similarity {:.3f}  smoothness {:.3f}  opacity {:.3f}  match {}\n", sc<int>(KEY.color[0] * 255.F + 0.5F),
                               sc<int>(KEY.color[1] * 255.F + 0.5F), sc<int>(KEY.color[2] * 255.F + 0.5F), KEY.similarity, KEY.smoothness, KEY.opacity, matchModeToString(KEY.mode));
        }
    }

    out += "\nkeyed right now:\n";
    bool any = false;
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW->m_isMapped)
            continue;
        const auto PROFILE = profileForWindow(WINDOW);
        if (!PROFILE)
            continue;
        any = true;
        out += std::format("  window {:x} [{}] -> {}\n", rc<uintptr_t>(WINDOW.get()), WINDOW->m_class, PROFILE->name);
    }
    for (const auto& LAYER : Desktop::layerState()->layers()) {
        if (!LAYER->m_mapped)
            continue;
        const auto PROFILE = profileForLayer(LAYER);
        if (!PROFILE)
            continue;
        any = true;
        out += std::format("  layer  {:x} [{}] -> {}\n", rc<uintptr_t>(LAYER.get()), LAYER->m_namespace, PROFILE->name);
    }
    if (!any)
        out += "  nothing\n";

    return out;
}
