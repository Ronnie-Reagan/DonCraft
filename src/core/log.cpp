#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace df
{
namespace
{
std::mutex& LogMutex()
{
    static std::mutex mutex;
    return mutex;
}

const char* ToLabel(const LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Info:
        return "INFO ";
    case LogLevel::Warning:
        return "WARN ";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "UNKWN";
    }
}

std::string BuildTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &nowTime);
#else
    localtime_r(&nowTime, &localTime);
#endif

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &localTime);
    return buffer;
}
}

void LogMessage(const LogLevel level, const std::string_view message)
{
    const std::string line = "[" + BuildTimestamp() + "] [" + ToLabel(level) + "] " + std::string(message) + "\n";

    std::lock_guard<std::mutex> lock(LogMutex());
    std::FILE* stream = (level == LogLevel::Error) ? stderr : stdout;
    std::fwrite(line.data(), 1, line.size(), stream);
    std::fflush(stream);

#if defined(_WIN32)
    OutputDebugStringA(line.c_str());
#endif
}
}
