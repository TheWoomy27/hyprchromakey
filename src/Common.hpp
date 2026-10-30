#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <format>
#include <string>

inline HANDLE PHANDLE = nullptr;

// Formats here and hands hyprland one finished string. Passing a format string through to the
// logger instead picks its (level, location, message) overload and prints the format string
// itself as the location.
template <typename... Args>
inline void chromaLog(Hyprutils::CLI::eLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    Log::logger->log(level, std::string_view{"[hyprchromakey] " + std::format(fmt, std::forward<Args>(args)...)});
}
