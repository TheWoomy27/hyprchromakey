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

class CChromaEngine {
  public:
    bool               install(HANDLE handle);
    void               uninstall();

    // called from the render hooks. beginDraw returns the state to hand back to endDraw.
    SP<SChromaProfile> beginDraw(SP<CWLSurfaceResource> surface, PHLLS layer);
    void               endDraw(SP<SChromaProfile> previous);
    SP<SChromaProfile> currentProfile() const;

    void               onConfigCommitted();
    void               refreshAll();
    void               clearOpaqueRegions();
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
    SP<SChromaProfile> profileForWindow(Desktop::View::CWindow* window);
    bool               keyedAndTranslucent(Desktop::View::CWindow* window);
    SP<SChromaProfile> profileForLayer(PHLLS layer);

    std::string        status(bool json);

    bool               hooked() const;
    bool               collided() const;

  private:
    SP<SChromaProfile> resolveCached(const std::string& value);
    void               applyTranslucency(SP<CWLSurfaceResource> surface);
    void               restoreTranslucency();

    SP<SChromaProfile>                                         m_drawProfile;

    Desktop::Rule::CWindowRuleEffectContainer::storageType     m_windowEffect = 0;
    Desktop::Rule::CLayerRuleEffectContainer::storageType      m_layerEffect  = 0;

    std::unordered_map<std::string, SP<SChromaProfile>>        m_resolveCache;
    std::unordered_map<Desktop::View::CWindow*, std::string>   m_windowOverrides;

    std::vector<std::pair<WP<Desktop::View::CWLSurface>, float>> m_translucent;
    std::vector<CBox>                                            m_pendingDamage;

    bool                                                         m_hooked   = false;
    bool                                                         m_collided = false;
};

inline CChromaEngine g_chromaEngine;
