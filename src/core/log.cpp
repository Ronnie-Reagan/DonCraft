#include "core/log.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "Dbghelp.lib")
#endif

namespace df
{
namespace
{
thread_local const char* gCrashContext = "Idle";

std::mutex& LogMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::FILE*& LogFileHandle()
{
    static std::FILE* handle = nullptr;
    return handle;
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

std::string BuildExecutableDirectory()
{
#if defined(_WIN32)
    char buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
    {
        return ".";
    }

    std::string path(buffer, buffer + length);
    const std::size_t separator = path.find_last_of("\\/");
    return separator == std::string::npos ? std::string(".") : path.substr(0, separator);
#else
    return ".";
#endif
}

auto BuildLogFilePath() -> std::string
{
    return BuildExecutableDirectory() + "/DonCraft.log";
}

void EnsureLogFileOpen()
{
    if (LogFileHandle() != nullptr)
    {
        return;
    }

#if defined(_WIN32)
    std::FILE* file = nullptr;
    if (fopen_s(&file, BuildLogFilePath().c_str(), "ab") == 0)
    {
        LogFileHandle() = file;
    }
#else
    LogFileHandle() = std::fopen(BuildLogFilePath().c_str(), "ab");
#endif
}

void WriteLineToFile(const std::string& line)
{
    EnsureLogFileOpen();
    if (LogFileHandle() == nullptr)
    {
        return;
    }

    std::fwrite(line.data(), 1, line.size(), LogFileHandle());
    std::fflush(LogFileHandle());
}

void WriteCrashLine(const std::string& message)
{
    const std::string line = "[" + BuildTimestamp() + "] [CRASH] " + message + "\n";
    if (LogMutex().try_lock())
    {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
        WriteLineToFile(line);
        LogMutex().unlock();
    }
    else
    {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }
#if defined(_WIN32)
    OutputDebugStringA(line.c_str());
#endif
}

#if defined(_WIN32)
auto BuildCrashDumpPath() -> std::string
{
    std::time_t nowTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm localTime{};
    localtime_s(&localTime, &nowTime);

    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &localTime);
    return BuildExecutableDirectory() + "/DonCraft_crash_" + std::string(timestamp) + ".dmp";
}

void WriteMiniDump(EXCEPTION_POINTERS* const exceptionPointers)
{
    const std::string dumpPath = BuildCrashDumpPath();
    HANDLE dumpFile = CreateFileA(
        dumpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE)
    {
        WriteCrashLine("Failed to create minidump file at '" + dumpPath + "'.");
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;

    const auto dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo);
    const BOOL wroteDump = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        dumpFile,
        dumpType,
        exceptionPointers != nullptr ? &exceptionInfo : nullptr,
        nullptr,
        nullptr);
    CloseHandle(dumpFile);

    if (wroteDump)
    {
        WriteCrashLine("Wrote minidump to '" + dumpPath + "'.");
    }
    else
    {
        WriteCrashLine("MiniDumpWriteDump failed for '" + dumpPath + "'.");
    }
}

LONG WINAPI UnhandledExceptionLogger(EXCEPTION_POINTERS* const exceptionPointers)
{
    std::ostringstream stream;
    stream << "Unhandled exception";
    if (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
    {
        stream << " code=0x" << std::hex << exceptionPointers->ExceptionRecord->ExceptionCode
               << " address=0x" << exceptionPointers->ExceptionRecord->ExceptionAddress;
    }
    stream << " context='" << (gCrashContext != nullptr ? gCrashContext : "unknown") << "'";
    WriteCrashLine(stream.str());
    WriteMiniDump(exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void TerminateLogger()
{
    std::string reason = "std::terminate invoked";
    if (const std::exception_ptr current = std::current_exception(); current != nullptr)
    {
        try
        {
            std::rethrow_exception(current);
        }
        catch (const std::exception& error)
        {
            reason += std::string(" due to exception: ") + error.what();
        }
        catch (...)
        {
            reason += " due to a non-standard exception";
        }
    }

    reason += std::string(" | context='") + (gCrashContext != nullptr ? gCrashContext : "unknown") + "'";
    WriteCrashLine(reason);
#if defined(_WIN32)
    WriteMiniDump(nullptr);
#endif
    std::abort();
}

void SignalLogger(const int signal)
{
    WriteCrashLine(
        std::string("Received fatal signal ") + std::to_string(signal) +
        " | context='" + (gCrashContext != nullptr ? gCrashContext : "unknown") + "'");
    std::_Exit(128 + signal);
}

void InstallCrashHandlers()
{
    static const bool installed = []()
    {
        EnsureLogFileOpen();
        std::set_terminate(&TerminateLogger);
        std::signal(SIGABRT, &SignalLogger);
#if defined(SIGSEGV)
        std::signal(SIGSEGV, &SignalLogger);
#endif
#if defined(_WIN32)
        SetUnhandledExceptionFilter(&UnhandledExceptionLogger);
#endif
        WriteCrashLine("Crash handlers installed.");
        return true;
    }();
    static_cast<void>(installed);
}

struct CrashHandlerInstaller
{
    CrashHandlerInstaller()
    {
        InstallCrashHandlers();
    }
} gCrashHandlerInstaller;
}

void LogMessage(const LogLevel level, const std::string_view message)
{
    const std::string line = "[" + BuildTimestamp() + "] [" + ToLabel(level) + "] " + std::string(message) + "\n";

    std::lock_guard<std::mutex> lock(LogMutex());
    std::FILE* stream = (level == LogLevel::Error) ? stderr : stdout;
    std::fwrite(line.data(), 1, line.size(), stream);
    std::fflush(stream);
    WriteLineToFile(line);

#if defined(_WIN32)
    OutputDebugStringA(line.c_str());
#endif
}

void SetCrashContext(const char* const context)
{
    gCrashContext = context != nullptr ? context : "unknown";
}

ScopedCrashContext::ScopedCrashContext(const char* const context)
    : previous_(gCrashContext)
{
    SetCrashContext(context);
}

ScopedCrashContext::~ScopedCrashContext()
{
    SetCrashContext(previous_);
}
}
