#include "ChromaEngine.hpp"
#include "ChromaShader.hpp"
#include "Common.hpp"
#include "Config.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <hyprutils/string/String.hpp>

// lua's headers carry no extern "C" guards, so without this the plugin ends up asking the dynamic
// linker for mangled C++ symbols and fails to load outright
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <optional>
#include <regex>
#include <vector>

using namespace Hyprutils::String;

static std::vector<CHyprSignalListener> g_listeners;
static SP<IPC::Socket1::SCommand>       g_hyprctlCommand;

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
        const auto& CLASS = WINDOW->metadata().appID();
        const auto& TITLE = WINDOW->metadata().title();

        if (KIND == "class" && std::regex_search(CLASS, pattern))
            return WINDOW;
        if (KIND == "title" && std::regex_search(TITLE, pattern))
            return WINDOW;
        if (KIND.empty() && (std::regex_search(TITLE, pattern) || std::regex_search(CLASS, pattern)))
            return WINDOW;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------- config plumbing

static void reloadConfigState();

// `hyprctl eval` changes config values without emitting a reload, without
// tripping hyprland's prop refresher and without damaging anything, so nothing tells us the config
// moved. Comparing a cheap hash per frame catches it - but only on a frame, and an idle compositor
// draws none, so a change made while nothing is moving lands whenever the screen next redraws for
// any reason. That is why the runtime switches below flip an override we own and damage the screen
// themselves rather than writing to the config.
static void pollConfigChanges() {
    static std::optional<uint64_t> s_last;

    const auto                     NOW = g_chromaConfig.fingerprint();
    if (s_last == NOW)
        return;

    const bool FIRST = !s_last.has_value();
    s_last           = NOW;

    if (!FIRST)
        reloadConfigState();
}

static void reloadConfigState() {
    g_chromaConfig.commit();
    g_chromaShaders.clear();
    g_chromaEngine.onConfigCommitted();
}

// ---------------------------------------------------------------------------- runtime switches

// the master switch argument: "on"/"off", or "config" to hand it back to
// plugin:hyprchromakey:enabled. The outer optional is "could not parse".
static std::optional<std::optional<bool>> parseSwitch(const std::string& value) {
    const auto V = trim(value);
    if (V == "on" || V == "1" || V == "true" || V == "enable")
        return std::optional<bool>{true};
    if (V == "off" || V == "0" || V == "false" || V == "disable")
        return std::optional<bool>{false};
    if (V == "config" || V.empty())
        return std::optional<bool>{std::nullopt};
    return std::nullopt;
}

// ---------------------------------------------------------------------------- lua api

// Since 0.57 hyprland has no dispatchers or config keywords for plugins, so these are the plugin's
// whole runtime interface. They are plain lua functions: wrap them in a closure for hl.bind, or
// call them through `hyprctl eval`.
//
// The ones acting on the focused window quietly do nothing when nothing is focused - they are
// usually on a key, and an error there would only pop up the error overlay. Naming a window that
// does not exist, or a profile that does not exist, is reported.

static const char* optString(lua_State* L, int index) {
    return lua_gettop(L) >= index ? lua_tostring(L, index) : nullptr;
}

static int luaResult(lua_State* L, const std::string& error) {
    if (!error.empty())
        return luaL_error(L, "hyprchromakey: %s", error.c_str());
    return 0;
}

static int luaToggle(lua_State* L) {
    const auto  WINDOW  = Desktop::focusState()->window();
    const char* PROFILE = optString(L, 1);
    return WINDOW ? luaResult(L, g_chromaEngine.toggleWindow(WINDOW, PROFILE ? PROFILE : "")) : 0;
}

static int luaToggleWindow(lua_State* L) {
    const char* WINDOW  = optString(L, 1);
    const char* PROFILE = optString(L, 2);
    if (!WINDOW)
        return luaL_error(L, "hyprchromakey.toggle_window: expected (window, [profile])");

    return luaResult(L, g_chromaEngine.toggleWindow(windowFromArg(WINDOW), PROFILE ? PROFILE : ""));
}

static int luaSet(lua_State* L) {
    const char* VALUE = optString(L, 1);
    if (!VALUE)
        return luaL_error(L, "hyprchromakey.set: expected \"on\", \"off\" or a profile name");

    const auto WINDOW = Desktop::focusState()->window();
    return WINDOW ? luaResult(L, g_chromaEngine.setWindowOverride(WINDOW, VALUE)) : 0;
}

static int luaSetWindow(lua_State* L) {
    const char* WINDOW = optString(L, 1);
    const char* VALUE  = optString(L, 2);
    if (!WINDOW || !VALUE)
        return luaL_error(L, "hyprchromakey.set_window: expected (window, \"on\"|\"off\"|profile)");

    return luaResult(L, g_chromaEngine.setWindowOverride(windowFromArg(WINDOW), VALUE));
}

// the whole-desktop switch, as opposed to luaToggle's focused window
static int luaToggleAll(lua_State* L) {
    g_chromaEngine.toggleAll();
    return 0;
}

static int luaSetAll(lua_State* L) {
    const char* VALUE  = optString(L, 1);
    const auto  PARSED = parseSwitch(VALUE ? VALUE : "");
    if (!PARSED)
        return luaL_error(L, "hyprchromakey.set_all: expected \"on\", \"off\" or \"config\"");

    g_chromaEngine.setEnabled(*PARSED);
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
    const std::pair<const char*, int (*)(lua_State*)> FUNCTIONS[] = {
        {"toggle", &luaToggle},         {"toggle_window", &luaToggleWindow}, {"set", &luaSet},       {"set_window", &luaSetWindow},
        {"toggle_all", &luaToggleAll}, {"set_all", &luaSetAll},             {"reset", &luaReset},   {"reload", &luaReload},
    };

    for (const auto& [NAME, FN] : FUNCTIONS) {
        if (!HyprlandAPI::addLuaFunction(handle, "hyprchromakey", NAME, FN))
            chromaLog(Log::ERR, "could not register hl.plugin.hyprchromakey.{}", NAME);
    }
}

// ---------------------------------------------------------------------------- registration

static void registerEvents() {
    auto& events = Event::bus()->m_events;

    g_listeners.emplace_back(events.config.reloaded.listen([] { reloadConfigState(); }));

    g_listeners.emplace_back(events.window.close.listen([](PHLWINDOW window) { g_chromaEngine.onWindowGone(window.get()); }));
    g_listeners.emplace_back(events.window.destroy.listen([](PHLWINDOWREF window) { g_chromaEngine.onWindowGone(window.get()); }));

    g_listeners.emplace_back(events.render.stage.listen([](const Event::SRenderStageEvent& event) {
        if (event.stage == RENDER_BEGIN) {
            pollConfigChanges();
            g_chromaEngine.onFrameBegin();
        } else if (event.stage == RENDER_POST)
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

    g_hyprctlCommand = HyprlandAPI::registerHyprCtlCommand(handle, IPC::Socket1::SCommand{
                                                                       .name    = "chromakey",
                                                                       .match   = IPC::Socket1::COMMAND_MATCH_EXACT,
                                                                       .handler = [](const IPC::Socket1::SRequest& request) -> IPC::Socket1::SResponse {
                                                                           return g_chromaEngine.status(request.format == IPC::Socket1::FORMAT_JSON);
                                                                       },
                                                                   });

    // when loaded after the config ran (hyprpm, `hyprctl plugin load`), the plugin's settings and
    // plugin:chromakey rules were rejected as unknown; run it again now that they exist
    HyprlandAPI::reloadConfig();

    return {
        "hyprchromakey",
        "Per-color window transparency: keys out background colors while text and images stay opaque",
        "TheWoomy27",
        "2.1.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();

    if (g_hyprctlCommand)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_hyprctlCommand);
    g_hyprctlCommand.reset();

    g_chromaEngine.uninstall();
    g_chromaEngine.unregisterEffects();
    g_chromaShaders.clear();
}
