#pragma once
// ChromaStudio Engine · core · process log. The executable installs the sink (file + debugger);
// libraries only format lines.

#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace chroma {

using LogSink = std::function<void(std::string_view line)>;

void setLogSink(LogSink sink);
void logLine(std::string_view message);

template <class... Args>
void logf(std::format_string<Args...> format, Args&&... args) {
    logLine(std::format(format, std::forward<Args>(args)...));
}

}  // namespace chroma
