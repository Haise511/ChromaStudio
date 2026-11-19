#include "core/Log.hpp"

#include <mutex>

namespace chroma {
namespace {
std::mutex& sinkMutex() {
    static std::mutex mutex;
    return mutex;
}
LogSink& sink() {
    static LogSink instance;
    return instance;
}
}  // namespace

void setLogSink(LogSink newSink) {
    const std::lock_guard lock(sinkMutex());
    sink() = std::move(newSink);
}

void logLine(std::string_view message) {
    const std::lock_guard lock(sinkMutex());
    if (sink()) sink()(message);
}

}  // namespace chroma
