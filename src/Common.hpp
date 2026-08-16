#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <format>
#include <string>

inline HANDLE PHANDLE = nullptr;

template <typename... Args>
inline void chromaLog(Hyprutils::CLI::eLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    Log::logger->log(level, "[hyprchromakey] {}", std::format(fmt, std::forward<Args>(args)...));
}
