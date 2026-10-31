#pragma once

#include "Common.hpp"
#include "Config.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRuleEffectContainer.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Render {
    class ITexture;
}

class CChromaEngine {
  public:
    // what the outer draw hook has established: which profile applies, and to which texture
    struct SDrawState {
        SP<SChromaProfile>      profile;
        const Render::ITexture* texture = nullptr;
    };

    bool               install(HANDLE handle);
    void               uninstall();
    void               unregisterEffects();

    // called from the render hooks. Each returns the state to hand back to its counterpart.
    SDrawState         beginDraw(PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLLS layer, const Render::ITexture* texture);
    void               endDraw(SDrawState previous);
    SP<SChromaProfile> activate(const Render::ITexture* texture);
    void               deactivate(SP<SChromaProfile> previous);
    SP<SChromaProfile> currentProfile() const;

    void               onConfigCommitted();
    void               refreshAll();
    void               onFrameBegin();
    void               onFrameEnd();
    void               onWindowGone(Desktop::View::CWindow* window);
    void               damageEverything();

    // dispatchers
    std::string        setWindowOverride(PHLWINDOW window, const std::string& value);
    std::string        toggleWindow(PHLWINDOW window, const std::string& profile);
    void               setEnabled(std::optional<bool> value);
    void               toggleAll();
    void               clearOverrides();

    SP<SChromaProfile> profileForWindow(PHLWINDOW window);
    SP<SChromaProfile> profileForLayer(PHLLS layer);
    bool               keyedAndTranslucent(PHLWINDOW window);
    PHLWINDOW          windowForPresentation(const void* presentation);

    std::string        status(bool json);

    bool               collided() const;

  private:
    struct STranslucent {
        WP<Desktop::View::CWLSurface> surface;
        float                         value = 1.F;
        // whoever owns the surface, to tell when it stops being keyed
        PHLWINDOWREF                  window;
        PHLLSREF                      layer;
    };

    SP<SChromaProfile> resolveCached(const std::string& value);
    bool               isFullscreen(PHLWINDOW window);
    void               applyTranslucency(SP<CWLSurfaceResource> surface, PHLWINDOW window, PHLLS layer);
    void               releaseTranslucency();
    void               restoreTranslucency();
    void               clearOpaqueRegions();

    SDrawState                                               m_draw;
    SP<SChromaProfile>                                       m_active;

    Desktop::Rule::CWindowRuleEffectContainer::storageType   m_windowEffect = 0;
    Desktop::Rule::CLayerRuleEffectContainer::storageType    m_layerEffect  = 0;

    std::unordered_map<std::string, SP<SChromaProfile>>      m_resolveCache;
    std::unordered_map<Desktop::View::CWindow*, std::string> m_windowOverrides;

    std::vector<STranslucent>                                m_translucent;
    std::vector<CBox>                                        m_pendingDamage;

    bool                                                     m_hooked   = false;
    bool                                                     m_collided = false;
};

inline CChromaEngine g_chromaEngine;
