#include "ChromaEngine.hpp"
#include "ChromaShader.hpp"
#include "Common.hpp"
#include "Config.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <hyprutils/string/String.hpp>

// lua's headers carry no extern "C" guards, so without this the plugin ends up asking the dynamic
// linker for mangled C++ symbols and fails to load outright
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <regex>
#include <vector>

using namespace Hyprutils::String;

static std::vector<CHyprSignalListener> g_listeners;
static SP<SHyprCtlCommand>              g_hyprctlCommand;

// ---------------------------------------------------------------------------- window lookup

// accepts `class:<regex>`, `title:<regex>`, `address:0x...`, or a bare regex tried on both
static PHLWINDOW windowFromArg(const std::string& arg) {
    if (arg.empty())
        return Desktop::focusState()->window();

    const auto COLON = arg.find(':');
    const auto KIND  = COLON == std::string::npos ? "" : arg.substr(0, COLON);
    const auto VALUE = COLON == std::string::npos ? arg : arg.substr(COLON + 1);

    if (KIND == "address") {
        for (const auto& WINDOW : Desktop::windowState()->windows()) {
            if (std::format("0x{:x}", rc<uintptr_t>(WINDOW.get())) == VALUE)
                return WINDOW;
        }
        return nullptr;
    }

    std::regex pattern;
    try {
        pattern = std::regex(VALUE);
    } catch (const std::exception& e) {
        chromaLog(Log::ERR, "bad regex \"{}\": {}", VALUE, e.what());
        return nullptr;
    }

    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (KIND == "class" && std::regex_search(WINDOW->m_class, pattern))
            return WINDOW;
        if (KIND == "title" && std::regex_search(WINDOW->m_title, pattern))
            return WINDOW;
        if (KIND.empty() && (std::regex_search(WINDOW->m_title, pattern) || std::regex_search(WINDOW->m_class, pattern)))
            return WINDOW;
    }

    return nullptr;
}

// splits "<window> <value>" where the window part may be missing
static std::pair<std::string, std::string> splitArgs(const std::string& args) {
    const auto TRIMMED = trim(args);
    const auto SPACE   = TRIMMED.find_last_of(' ');

    if (SPACE == std::string::npos)
        return {"", TRIMMED};

    return {trim(TRIMMED.substr(0, SPACE)), trim(TRIMMED.substr(SPACE + 1))};
}

// ---------------------------------------------------------------------------- config plumbing

static void reloadConfigState();

static Hyprlang::CParseResult keywordResult(const std::expected<void, std::string>& res) {
    Hyprlang::CParseResult out;
    if (!res)
        out.setError(res.error().c_str());
    return out;
}

// `hyprctl eval` changes config values without emitting a reload, so nothing tells us the config
// moved: keying quietly starts or stops on the next draw while the screen keeps its stale pixels
// until something else damages it. Comparing a cheap snapshot each frame catches that.
static void pollConfigChanges() {
    static std::string s_last;

    auto               now = g_chromaConfig.fingerprint();
    if (now == s_last)
        return;

    const bool FIRST = s_last.empty();
    s_last           = std::move(now);

    if (!FIRST)
        reloadConfigState();
}

static void reloadConfigState() {
    g_chromaConfig.commit();
    g_chromaShaders.clear();
    g_chromaEngine.onConfigCommitted();
}

// ---------------------------------------------------------------------------- lua api

// hl.dsp only covers hyprland's own dispatchers and there is no way to reach a plugin's by name,
// so on a lua config these are the only way to drive the plugin. They are plain lua functions, so
// they can be handed straight to hl.bind without a hyprctl round trip.
static int luaToggle(lua_State* L) {
    const char* PROFILE = lua_gettop(L) > 0 ? lua_tostring(L, 1) : nullptr;
    g_chromaEngine.toggleWindow(Desktop::focusState()->window(), PROFILE ? PROFILE : "");
    return 0;
}

static int luaSet(lua_State* L) {
    const char* VALUE = lua_gettop(L) > 0 ? lua_tostring(L, 1) : nullptr;
    if (!VALUE)
        return luaL_error(L, "hyprchromakey.set: expected \"on\", \"off\" or a profile name");

    g_chromaEngine.setWindowOverride(Desktop::focusState()->window(), VALUE);
    return 0;
}

static int luaSetWindow(lua_State* L) {
    const char* WINDOW = lua_gettop(L) > 0 ? lua_tostring(L, 1) : nullptr;
    const char* VALUE  = lua_gettop(L) > 1 ? lua_tostring(L, 2) : nullptr;
    if (!WINDOW || !VALUE)
        return luaL_error(L, "hyprchromakey.set_window: expected (window, \"on\"|\"off\"|profile)");

    g_chromaEngine.setWindowOverride(windowFromArg(WINDOW), VALUE);
    return 0;
}

static int luaReset(lua_State* L) {
    g_chromaEngine.clearOverrides();
    return 0;
}

static int luaReload(lua_State* L) {
    reloadConfigState();
    return 0;
}

static void registerLuaFunctions(HANDLE handle) {
    // only present on a lua config; a no-op elsewhere
    HyprlandAPI::addLuaFunction(handle, "hyprchromakey", "toggle", &luaToggle);
    HyprlandAPI::addLuaFunction(handle, "hyprchromakey", "set", &luaSet);
    HyprlandAPI::addLuaFunction(handle, "hyprchromakey", "set_window", &luaSetWindow);
    HyprlandAPI::addLuaFunction(handle, "hyprchromakey", "reset", &luaReset);
    HyprlandAPI::addLuaFunction(handle, "hyprchromakey", "reload", &luaReload);
}

// ---------------------------------------------------------------------------- registration

static void registerDispatchers(HANDLE handle) {
    const auto OK   = [](const std::string& error) { return SDispatchResult{.success = error.empty(), .error = error}; };

    HyprlandAPI::addDispatcherV2(handle, "chromakey:toggle", [OK](std::string args) -> SDispatchResult { //
        return OK(g_chromaEngine.toggleWindow(Desktop::focusState()->window(), trim(args)));
    });

    HyprlandAPI::addDispatcherV2(handle, "chromakey:togglewindow", [OK](std::string args) -> SDispatchResult {
        const auto [WINDOW, PROFILE] = splitArgs(args);
        // a lone argument is the window, not the profile
        return WINDOW.empty() ? OK(g_chromaEngine.toggleWindow(windowFromArg(PROFILE), "")) : OK(g_chromaEngine.toggleWindow(windowFromArg(WINDOW), PROFILE));
    });

    HyprlandAPI::addDispatcherV2(handle, "chromakey:set", [OK](std::string args) -> SDispatchResult { //
        return OK(g_chromaEngine.setWindowOverride(Desktop::focusState()->window(), trim(args)));
    });

    HyprlandAPI::addDispatcherV2(handle, "chromakey:setwindow", [OK](std::string args) -> SDispatchResult {
        const auto [WINDOW, VALUE] = splitArgs(args);
        if (WINDOW.empty())
            return SDispatchResult{.success = false, .error = "usage: chromakey:setwindow <window> <on|off|profile>"};
        return OK(g_chromaEngine.setWindowOverride(windowFromArg(WINDOW), VALUE));
    });

    HyprlandAPI::addDispatcherV2(handle, "chromakey:reset", [](std::string) -> SDispatchResult {
        g_chromaEngine.clearOverrides();
        return {.success = true};
    });

    HyprlandAPI::addDispatcherV2(handle, "chromakey:reload", [](std::string) -> SDispatchResult {
        reloadConfigState();
        return {.success = true};
    });
}

static void registerKeywords(HANDLE handle) {
    HyprlandAPI::addConfigKeyword(
        handle, "chromakey", [](const char*, const char* value) -> Hyprlang::CParseResult { return keywordResult(g_chromaConfig.addKeySpec(value)); }, {.allowFlags = false});
}

static void registerEvents() {
    auto& events = Event::bus()->m_events;

    g_listeners.emplace_back(events.config.preReload.listen([] { g_chromaConfig.clearSpecs(); }));
    g_listeners.emplace_back(events.config.reloaded.listen([] { reloadConfigState(); }));

    g_listeners.emplace_back(events.window.close.listen([](PHLWINDOW window) { g_chromaEngine.onWindowGone(window.get()); }));
    g_listeners.emplace_back(events.window.destroy.listen([](PHLWINDOWREF window) { g_chromaEngine.onWindowGone(window.get()); }));

    g_listeners.emplace_back(events.render.stage.listen([](eRenderStage stage) {
        if (stage == RENDER_BEGIN) {
            pollConfigChanges();
            g_chromaEngine.clearOpaqueRegions();
        }
        else if (stage == RENDER_POST)
            g_chromaEngine.onFrameEnd();
    }));
}

// ---------------------------------------------------------------------------- plugin api

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const auto VERSION = HyprlandAPI::getHyprlandVersion(handle);
    if (VERSION.hash != std::string{GIT_COMMIT_HASH})
        chromaLog(Log::WARN, "built against hyprland {}, running on {}. Rebuild if things look wrong.", GIT_COMMIT_HASH, VERSION.hash);

    g_chromaConfig.registerConfig(handle);
    registerKeywords(handle);
    registerDispatchers(handle);
    registerLuaFunctions(handle);
    registerEvents();

    if (!g_chromaEngine.install(handle)) {
        HyprlandAPI::addNotification(handle,
                                    g_chromaEngine.collided() ?
                                        "[hyprchromakey] another plugin already hooks the render path. Only one may run - see `hyprctl chromakey`" :
                                        "[hyprchromakey] failed to install render hooks, the plugin will do nothing",
                                    CHyprColor{1.0, 0.2, 0.2, 1.0}, 12000);
        chromaLog(Log::ERR, "render hooks unavailable, giving up");
    }

    g_hyprctlCommand = HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                                       .name  = "chromakey",
                                                                       .exact = true,
                                                                       .fn    = [](eHyprCtlOutputFormat format, std::string) { return g_chromaEngine.status(format == FORMAT_JSON); },
                                                                   });

    // picks up chromakey keywords and plugin:chromakey rules from the user's config, which was
    // parsed before this plugin existed
    HyprlandAPI::reloadConfig();

    return {
        "hyprchromakey",
        "Per-color window transparency: keys out background colors while text and images stay opaque",
        "TheWoomy27",
        "2.0.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();

    if (g_hyprctlCommand)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_hyprctlCommand);
    g_hyprctlCommand.reset();

    g_chromaEngine.uninstall();
    g_chromaShaders.clear();
}
