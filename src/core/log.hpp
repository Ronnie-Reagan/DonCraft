#pragma once

#include <sstream>
#include <string_view>
#include <utility>

namespace df
{
enum class LogLevel
{
    Trace,
    Info,
    Warning,
    Error,
};

void LogMessage(LogLevel level, std::string_view message);
void SetCrashContext(const char* context);

class ScopedCrashContext
{
public:
    explicit ScopedCrashContext(const char* context);
    ~ScopedCrashContext();

private:
    const char* previous_ = nullptr;
};

template <typename... Args>
void Log(LogLevel level, Args&&... args)
{
    std::ostringstream stream;
    (stream << ... << std::forward<Args>(args));
    LogMessage(level, stream.str());
}

template <typename... Args>
void LogTrace(Args&&... args)
{
    Log(LogLevel::Trace, std::forward<Args>(args)...);
}

template <typename... Args>
void LogInfo(Args&&... args)
{
    Log(LogLevel::Info, std::forward<Args>(args)...);
}

template <typename... Args>
void LogWarning(Args&&... args)
{
    Log(LogLevel::Warning, std::forward<Args>(args)...);
}

template <typename... Args>
void LogError(Args&&... args)
{
    Log(LogLevel::Error, std::forward<Args>(args)...);
}
}
