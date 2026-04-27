#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Shell32.lib")

#ifndef DON_CRAFT_DEFAULT_UPDATE_MANIFEST_URL
#define DON_CRAFT_DEFAULT_UPDATE_MANIFEST_URL "https://raw.githubusercontent.com/Ronnie-Reagan/DonCraft/main/dist/update/alpha/manifest.json"
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace
{
namespace fs = std::filesystem;

constexpr wchar_t kLauncherTitle[] = L"DonCraft Launcher";
constexpr wchar_t kAppDataDirectoryName[] = L"DonCraft";
constexpr wchar_t kPortablePayloadDirectoryName[] = L"DonCraftRuntime";
constexpr wchar_t kBootstrapPayloadDirectoryName[] = L"DonCraftBootstrap";
constexpr wchar_t kDefaultPayloadDirectoryName[] = L"Runtime";
constexpr wchar_t kLauncherStateDirectoryName[] = L".launcher";
constexpr wchar_t kLauncherLogFileName[] = L"launcher.log";
constexpr wchar_t kUpdateStagingDirectoryName[] = L".update";
constexpr wchar_t kUpdateMutexName[] = L"Local\\DonCraftLauncherUpdateMutex";
constexpr wchar_t kPortableMarkerFileName[] = L"doncraft_portable.txt";

constexpr char kLauncherVersion[] = "1.2.0";
constexpr char kDefaultEntrypoint[] = "Don_Craft_client.exe";
constexpr std::uint64_t kMaxManifestBytes = 1024u * 1024u;
constexpr int kDownloadAttemptCount = 5;
constexpr int kResolveTimeoutMs = 30000;
constexpr int kConnectTimeoutMs = 30000;
constexpr int kSendTimeoutMs = 90000;
constexpr int kReceiveTimeoutMs = 90000;

struct ManifestFile
{
    std::string path;
    std::string sha256;
    std::uint64_t size = 0;
};

struct Manifest
{
    std::string version;
    std::string baseUrl;
    std::string entrypoint = kDefaultEntrypoint;
    bool cleanExtra = false;
    std::vector<ManifestFile> files;
};

struct UpdateResult
{
    std::string version;
    std::string entrypoint = kDefaultEntrypoint;
    std::size_t downloadedFiles = 0;
    std::size_t installedFiles = 0;
};

[[nodiscard]] std::wstring Utf8ToWide(const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::runtime_error("Text is too large to decode as UTF-8.");
    }

    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
    {
        throw std::runtime_error("Failed to decode UTF-8 text.");
    }

    std::wstring output(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), required);
    if (written != required)
    {
        throw std::runtime_error("Failed to decode UTF-8 text.");
    }
    return output;
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::runtime_error("Text is too large to encode as UTF-8.");
    }

    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        throw std::runtime_error("Failed to encode UTF-16 text.");
    }

    std::string output(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), required, nullptr, nullptr);
    if (written != required)
    {
        throw std::runtime_error("Failed to encode UTF-16 text.");
    }
    return output;
}

[[nodiscard]] std::string ToLowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

[[nodiscard]] std::wstring ToLowerWide(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return text;
}

[[nodiscard]] bool IsHexSha256(const std::string_view text)
{
    if (text.size() != 64)
    {
        return false;
    }

    for (const char ch : text)
    {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (std::isxdigit(value) == 0)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string WinHttpErrorName(const DWORD error)
{
    switch (error)
    {
    case ERROR_WINHTTP_TIMEOUT:
        return "ERROR_WINHTTP_TIMEOUT";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "ERROR_WINHTTP_NAME_NOT_RESOLVED";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "ERROR_WINHTTP_CANNOT_CONNECT";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "ERROR_WINHTTP_CONNECTION_ERROR";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "ERROR_WINHTTP_SECURE_FAILURE";
    case ERROR_WINHTTP_INVALID_URL:
        return "ERROR_WINHTTP_INVALID_URL";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "ERROR_WINHTTP_OPERATION_CANCELLED";
#ifdef ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW
    case ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW:
        return "ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW";
#endif
    default:
        return {};
    }
}

[[nodiscard]] std::string Win32Message(const DWORD error)
{
    const std::string winHttpName = WinHttpErrorName(error);

    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr)
    {
        if (!winHttpName.empty())
        {
            return winHttpName + " (" + std::to_string(error) + ")";
        }
        return "Windows error " + std::to_string(error);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.' || message.back() == L' '))
    {
        message.pop_back();
    }

    std::string text = WideToUtf8(message);
    if (!winHttpName.empty())
    {
        text = winHttpName + " (" + std::to_string(error) + "): " + text;
    }
    return text;
}

[[noreturn]] void ThrowLastError(const char* const operation)
{
    throw std::runtime_error(std::string(operation) + ": " + Win32Message(GetLastError()));
}

[[nodiscard]] fs::path ModuleDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            ThrowLastError("GetModuleFileNameW failed");
        }
        if (length < buffer.size() - 1)
        {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

[[nodiscard]] std::optional<fs::path> EnvironmentPath(const wchar_t* const variableName)
{
    const DWORD required = GetEnvironmentVariableW(variableName, nullptr, 0);
    if (required == 0)
    {
        return std::nullopt;
    }

    std::wstring buffer(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(variableName, buffer.data(), required);
    if (length == 0 || length >= required)
    {
        return std::nullopt;
    }

    buffer.resize(length);
    if (buffer.empty())
    {
        return std::nullopt;
    }
    return fs::path(buffer);
}

[[nodiscard]] fs::path DefaultAppDataDirectory()
{
    if (const std::optional<fs::path> localAppData = EnvironmentPath(L"LOCALAPPDATA"))
    {
        return *localAppData / kAppDataDirectoryName;
    }
    return ModuleDirectory() / kAppDataDirectoryName;
}

class Logger
{
public:
    explicit Logger(fs::path logPath)
        : logPath_(std::move(logPath))
    {
        std::error_code error;
        fs::create_directories(logPath_.parent_path(), error);
        stream_.open(logPath_, std::ios::app);
        Log("launcher start; version=" + std::string(kLauncherVersion));
    }

    void Log(const std::string& message)
    {
        if (!stream_)
        {
            return;
        }

        stream_ << Timestamp() << " " << message << "\n";
        stream_.flush();
    }

    [[nodiscard]] const fs::path& Path() const
    {
        return logPath_;
    }

private:
    [[nodiscard]] static std::string Timestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t raw = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_s(&local, &raw);

        std::ostringstream output;
        output << std::put_time(&local, "[%Y-%m-%d %H:%M:%S]");
        return output.str();
    }

    fs::path logPath_;
    std::ofstream stream_;
};

struct HttpHandle
{
    HINTERNET value = nullptr;

    HttpHandle() = default;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    HttpHandle(const HttpHandle&) = delete;
    auto operator=(const HttpHandle&) -> HttpHandle& = delete;

    HttpHandle(HttpHandle&& other) noexcept : value(other.value)
    {
        other.value = nullptr;
    }

    auto operator=(HttpHandle&& other) noexcept -> HttpHandle&
    {
        if (this != &other)
        {
            Reset();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }

    ~HttpHandle()
    {
        Reset();
    }

    void Reset()
    {
        if (value != nullptr)
        {
            WinHttpCloseHandle(value);
            value = nullptr;
        }
    }

    [[nodiscard]] explicit operator bool() const
    {
        return value != nullptr;
    }
};

struct WinHandle
{
    HANDLE value = nullptr;

    WinHandle() = default;
    explicit WinHandle(HANDLE handle) : value(handle) {}
    WinHandle(const WinHandle&) = delete;
    auto operator=(const WinHandle&) -> WinHandle& = delete;

    WinHandle(WinHandle&& other) noexcept : value(other.value)
    {
        other.value = nullptr;
    }

    auto operator=(WinHandle&& other) noexcept -> WinHandle&
    {
        if (this != &other)
        {
            Reset();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }

    ~WinHandle()
    {
        Reset();
    }

    void Reset()
    {
        if (value != nullptr && value != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value);
            value = nullptr;
        }
    }

    [[nodiscard]] explicit operator bool() const
    {
        return value != nullptr && value != INVALID_HANDLE_VALUE;
    }
};

class UpdateMutex
{
public:
    explicit UpdateMutex(Logger& logger)
        : logger_(logger),
          handle_(CreateMutexW(nullptr, FALSE, kUpdateMutexName))
    {
        if (!handle_)
        {
            ThrowLastError("CreateMutexW failed");
        }

        logger_.Log("waiting for launcher update mutex");
        const DWORD result = WaitForSingleObject(handle_.value, INFINITE);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        {
            ThrowLastError("WaitForSingleObject failed");
        }

        locked_ = true;
        logger_.Log("launcher update mutex acquired");
    }

    ~UpdateMutex()
    {
        if (locked_)
        {
            ReleaseMutex(handle_.value);
            logger_.Log("launcher update mutex released");
        }
    }

private:
    Logger& logger_;
    WinHandle handle_;
    bool locked_ = false;
};

struct ParsedUrl
{
    std::wstring host;
    std::wstring pathAndQuery;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

[[nodiscard]] ParsedUrl ParseUrl(const std::string& url)
{
    std::wstring wideUrl = Utf8ToWide(url);
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.data(), static_cast<DWORD>(wideUrl.size()), 0, &components))
    {
        ThrowLastError("Invalid update URL");
    }

    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS)
    {
        throw std::runtime_error("Update URL must use http or https.");
    }

    ParsedUrl parsed{};
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.pathAndQuery.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0)
    {
        parsed.pathAndQuery.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (parsed.pathAndQuery.empty())
    {
        parsed.pathAndQuery = L"/";
    }
    parsed.port = components.nPort;
    parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return parsed;
}

[[nodiscard]] bool IsLoopbackHost(std::wstring host)
{
    host = ToLowerWide(std::move(host));
    return host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
}

class HttpClient
{
public:
    HttpClient(Logger& logger, const bool allowInsecureHttp)
        : logger_(logger),
          allowInsecureHttp_(allowInsecureHttp),
          session_(WinHttpOpen(L"DonCraftLauncher/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
    {
        if (!session_)
        {
            ThrowLastError("WinHttpOpen failed");
        }

        WinHttpSetTimeouts(session_.value, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);
        logger_.Log("http timeouts set; resolve=" + std::to_string(kResolveTimeoutMs) + "ms; connect=" + std::to_string(kConnectTimeoutMs) + "ms; send=" + std::to_string(kSendTimeoutMs) + "ms; receive=" + std::to_string(kReceiveTimeoutMs) + "ms");
    }

    [[nodiscard]] std::vector<unsigned char> Download(const std::string& url, const std::uint64_t maxBytes, const char* const label) const
    {
        std::string lastError;
        for (int attempt = 1; attempt <= kDownloadAttemptCount; ++attempt)
        {
            try
            {
                if (attempt > 1)
                {
                    const DWORD sleepMs = static_cast<DWORD>(500 * attempt);
                    Sleep(sleepMs);
                }

                logger_.Log("download attempt " + std::to_string(attempt) + "/" + std::to_string(kDownloadAttemptCount) + " for " + label + ": " + url);
                return DownloadOnce(url, maxBytes);
            }
            catch (const std::exception& error)
            {
                lastError = error.what();
                logger_.Log("download failed for " + std::string(label) + ": " + lastError);
            }
        }

        throw std::runtime_error("Download failed after retries for " + std::string(label) + ": " + lastError);
    }

private:
    [[nodiscard]] std::vector<unsigned char> DownloadOnce(const std::string& url, const std::uint64_t maxBytes) const
    {
        const ParsedUrl parsed = ParseUrl(url);
        logger_.Log("http request target; host=" + WideToUtf8(parsed.host) + "; path=" + WideToUtf8(parsed.pathAndQuery) + "; secure=" + std::string(parsed.secure ? "yes" : "no"));
        if (!parsed.secure && !allowInsecureHttp_ && !IsLoopbackHost(parsed.host))
        {
            throw std::runtime_error("Refusing insecure HTTP update URL. Use --doncraft-allow-insecure-http only for local testing.");
        }

        HttpHandle connection(WinHttpConnect(session_.value, parsed.host.c_str(), parsed.port, 0));
        if (!connection)
        {
            ThrowLastError("WinHttpConnect failed");
        }

        const DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
        HttpHandle request(WinHttpOpenRequest(
            connection.value,
            L"GET",
            parsed.pathAndQuery.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags));
        if (!request)
        {
            ThrowLastError("WinHttpOpenRequest failed");
        }

        constexpr wchar_t kHeaders[] = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";
        if (!WinHttpSendRequest(request.value, kHeaders, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        {
            ThrowLastError("WinHttpSendRequest failed");
        }
        if (!WinHttpReceiveResponse(request.value, nullptr))
        {
            ThrowLastError("WinHttpReceiveResponse failed");
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(
                request.value,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX))
        {
            ThrowLastError("WinHttpQueryHeaders failed");
        }
        if (status < 200 || status >= 300)
        {
            throw std::runtime_error("HTTP " + std::to_string(status) + " while downloading " + url);
        }

        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(
                request.value,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &contentLength,
                &contentLengthSize,
                WINHTTP_NO_HEADER_INDEX))
        {
            if (contentLength > maxBytes)
            {
                throw std::runtime_error("Server response is larger than the allowed download size.");
            }
        }

        std::vector<unsigned char> bytes;
        if (contentLength > 0)
        {
            bytes.reserve(contentLength);
        }

        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.value, &available))
            {
                ThrowLastError("WinHttpQueryDataAvailable failed");
            }
            if (available == 0)
            {
                break;
            }

            if (bytes.size() + available > maxBytes)
            {
                throw std::runtime_error("Downloaded data exceeded the allowed size.");
            }

            const std::size_t offset = bytes.size();
            bytes.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.value, bytes.data() + offset, available, &read))
            {
                ThrowLastError("WinHttpReadData failed");
            }
            bytes.resize(offset + read);

            if (read == 0)
            {
                break;
            }
        }

        return bytes;
    }

    Logger& logger_;
    bool allowInsecureHttp_ = false;
    HttpHandle session_;
};

void SkipWhitespace(const std::string_view text, std::size_t& index)
{
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0)
    {
        ++index;
    }
}

[[nodiscard]] std::optional<std::string> ParseJsonStringAt(const std::string_view text, std::size_t& index)
{
    SkipWhitespace(text, index);
    if (index >= text.size() || text[index] != '"')
    {
        return std::nullopt;
    }
    ++index;

    std::string output;
    while (index < text.size())
    {
        const char ch = text[index++];
        if (ch == '"')
        {
            return output;
        }
        if (ch != '\\')
        {
            output.push_back(ch);
            continue;
        }
        if (index >= text.size())
        {
            return std::nullopt;
        }
        const char escaped = text[index++];
        switch (escaped)
        {
        case '"':
        case '\\':
        case '/':
            output.push_back(escaped);
            break;
        case 'b':
            output.push_back('\b');
            break;
        case 'f':
            output.push_back('\f');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> ParseJsonUnsignedAt(const std::string_view text, std::size_t& index)
{
    SkipWhitespace(text, index);
    if (index >= text.size() || std::isdigit(static_cast<unsigned char>(text[index])) == 0)
    {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0)
    {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[index] - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
        {
            return std::nullopt;
        }

        value = (value * 10u) + digit;
        ++index;
    }
    return value;
}

[[nodiscard]] std::optional<bool> ParseJsonBoolAt(const std::string_view text, std::size_t& index)
{
    SkipWhitespace(text, index);
    if (text.substr(index, 4) == "true")
    {
        index += 4;
        return true;
    }
    if (text.substr(index, 5) == "false")
    {
        index += 5;
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> FindValueStart(const std::string_view text, const std::string_view key)
{
    const std::string pattern = "\"" + std::string(key) + "\"";
    std::size_t searchFrom = 0;
    while (searchFrom < text.size())
    {
        const std::size_t keyPosition = text.find(pattern, searchFrom);
        if (keyPosition == std::string_view::npos)
        {
            return std::nullopt;
        }

        const std::size_t colonPosition = text.find(':', keyPosition + pattern.size());
        if (colonPosition == std::string_view::npos)
        {
            return std::nullopt;
        }

        std::size_t valuePosition = colonPosition + 1;
        SkipWhitespace(text, valuePosition);
        return valuePosition;
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> FindStringField(const std::string_view text, const std::string_view key)
{
    std::optional<std::size_t> valuePosition = FindValueStart(text, key);
    if (!valuePosition)
    {
        return std::nullopt;
    }
    return ParseJsonStringAt(text, *valuePosition);
}

[[nodiscard]] std::optional<std::uint64_t> FindUnsignedField(const std::string_view text, const std::string_view key)
{
    std::optional<std::size_t> valuePosition = FindValueStart(text, key);
    if (!valuePosition)
    {
        return std::nullopt;
    }
    return ParseJsonUnsignedAt(text, *valuePosition);
}

[[nodiscard]] std::optional<bool> FindBoolField(const std::string_view text, const std::string_view key)
{
    std::optional<std::size_t> valuePosition = FindValueStart(text, key);
    if (!valuePosition)
    {
        return std::nullopt;
    }
    return ParseJsonBoolAt(text, *valuePosition);
}

[[nodiscard]] std::size_t FindMatchingBracket(const std::string_view text, const std::size_t openPosition, const char open, const char close)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;

    for (std::size_t index = openPosition; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
        }
        else if (ch == open)
        {
            ++depth;
        }
        else if (ch == close)
        {
            --depth;
            if (depth == 0)
            {
                return index;
            }
        }
    }

    return std::string_view::npos;
}

[[nodiscard]] Manifest ParseManifest(const std::string& manifestText, const std::string& manifestUrl)
{
    Manifest manifest{};
    manifest.version = FindStringField(manifestText, "version").value_or("");
    manifest.baseUrl = FindStringField(manifestText, "base_url").value_or("");
    manifest.entrypoint = FindStringField(manifestText, "entrypoint").value_or(kDefaultEntrypoint);
    manifest.cleanExtra = FindBoolField(manifestText, "clean_extra").value_or(false);

    if (manifest.baseUrl.empty())
    {
        const std::size_t slash = manifestUrl.rfind('/');
        if (slash == std::string::npos)
        {
            throw std::runtime_error("Update manifest does not include base_url and the manifest URL cannot be used as a base.");
        }
        manifest.baseUrl = manifestUrl.substr(0, slash + 1) + "files/";
    }
    if (!manifest.baseUrl.empty() && manifest.baseUrl.back() != '/')
    {
        manifest.baseUrl.push_back('/');
    }

    const std::optional<std::size_t> filesValue = FindValueStart(manifestText, "files");
    if (!filesValue || *filesValue >= manifestText.size() || manifestText[*filesValue] != '[')
    {
        throw std::runtime_error("Update manifest is missing a files array.");
    }

    const std::size_t filesEnd = FindMatchingBracket(manifestText, *filesValue, '[', ']');
    if (filesEnd == std::string_view::npos)
    {
        throw std::runtime_error("Update manifest files array is malformed.");
    }

    std::set<std::string> seenFiles;
    std::size_t index = *filesValue + 1;
    while (index < filesEnd)
    {
        SkipWhitespace(manifestText, index);
        if (index < filesEnd && manifestText[index] == ',')
        {
            ++index;
            continue;
        }
        if (index >= filesEnd)
        {
            break;
        }
        if (manifestText[index] != '{')
        {
            throw std::runtime_error("Update manifest files array contains a non-object entry.");
        }

        const std::size_t objectEnd = FindMatchingBracket(manifestText, index, '{', '}');
        if (objectEnd == std::string_view::npos || objectEnd > filesEnd)
        {
            throw std::runtime_error("Update manifest file entry is malformed.");
        }

        const std::string_view object(manifestText.data() + index, objectEnd - index + 1);
        ManifestFile file{};
        file.path = FindStringField(object, "path").value_or("");
        file.sha256 = ToLowerAscii(FindStringField(object, "sha256").value_or(""));
        file.size = FindUnsignedField(object, "size").value_or(0);

        if (file.path.empty())
        {
            throw std::runtime_error("Update manifest contains a file entry without a path.");
        }
        if (!IsHexSha256(file.sha256))
        {
            throw std::runtime_error("Update manifest contains an invalid sha256 for: " + file.path);
        }
        if (file.size == 0)
        {
            throw std::runtime_error("Update manifest contains an invalid size for: " + file.path);
        }
        if (!seenFiles.insert(file.path).second)
        {
            throw std::runtime_error("Update manifest contains a duplicate file path: " + file.path);
        }

        manifest.files.push_back(std::move(file));
        index = objectEnd + 1;
    }

    if (manifest.files.empty())
    {
        throw std::runtime_error("Update manifest contains no runtime files.");
    }
    return manifest;
}

[[nodiscard]] fs::path SafePayloadRelativePath(const std::string& relativePath)
{
    if (relativePath.empty() || relativePath.front() == '/' || relativePath.front() == '\\' || relativePath.find(':') != std::string::npos)
    {
        throw std::runtime_error("Update manifest contains an unsafe file path: " + relativePath);
    }

    fs::path output;
    std::size_t start = 0;
    while (start <= relativePath.size())
    {
        const std::size_t slash = relativePath.find('/', start);
        const std::string part = relativePath.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string::npos)
        {
            throw std::runtime_error("Update manifest contains an unsafe file path: " + relativePath);
        }
        output /= Utf8ToWide(part);
        if (slash == std::string::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return output;
}

[[nodiscard]] std::string BytesToHex(const unsigned char* const bytes, const std::size_t size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index)
    {
        output.push_back(digits[(bytes[index] >> 4u) & 0x0Fu]);
        output.push_back(digits[bytes[index] & 0x0Fu]);
    }
    return output;
}

struct BcryptAlgorithm
{
    BCRYPT_ALG_HANDLE value = nullptr;

    explicit BcryptAlgorithm(const wchar_t* const algorithm)
    {
        const NTSTATUS status = BCryptOpenAlgorithmProvider(&value, algorithm, nullptr, 0);
        if (!NT_SUCCESS(status))
        {
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed.");
        }
    }

    ~BcryptAlgorithm()
    {
        if (value != nullptr)
        {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }

    BcryptAlgorithm(const BcryptAlgorithm&) = delete;
    auto operator=(const BcryptAlgorithm&) -> BcryptAlgorithm& = delete;
};

struct BcryptHash
{
    BCRYPT_HASH_HANDLE value = nullptr;

    BcryptHash(BCRYPT_ALG_HANDLE algorithm, std::vector<unsigned char>& objectBuffer)
    {
        const NTSTATUS status = BCryptCreateHash(algorithm, &value, objectBuffer.data(), static_cast<ULONG>(objectBuffer.size()), nullptr, 0, 0);
        if (!NT_SUCCESS(status))
        {
            throw std::runtime_error("BCryptCreateHash failed.");
        }
    }

    ~BcryptHash()
    {
        if (value != nullptr)
        {
            BCryptDestroyHash(value);
        }
    }

    BcryptHash(const BcryptHash&) = delete;
    auto operator=(const BcryptHash&) -> BcryptHash& = delete;
};

[[nodiscard]] std::string Sha256File(const fs::path& path)
{
    BcryptAlgorithm algorithm(BCRYPT_SHA256_ALGORITHM);

    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD propertyLength = 0;
    NTSTATUS status = BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &propertyLength, 0);
    if (!NT_SUCCESS(status))
    {
        throw std::runtime_error("BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed.");
    }
    status = BCryptGetProperty(algorithm.value, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &propertyLength, 0);
    if (!NT_SUCCESS(status))
    {
        throw std::runtime_error("BCryptGetProperty(BCRYPT_HASH_LENGTH) failed.");
    }

    std::vector<unsigned char> objectBuffer(objectLength);
    std::vector<unsigned char> hashBuffer(hashLength);
    BcryptHash hash(algorithm.value, objectBuffer);

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Could not open file for hashing: " + WideToUtf8(path.wstring()));
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
        {
            status = BCryptHashData(hash.value, buffer.data(), static_cast<ULONG>(count), 0);
            if (!NT_SUCCESS(status))
            {
                throw std::runtime_error("BCryptHashData failed.");
            }
        }
    }

    status = BCryptFinishHash(hash.value, hashBuffer.data(), hashLength, 0);
    if (!NT_SUCCESS(status))
    {
        throw std::runtime_error("BCryptFinishHash failed.");
    }

    return BytesToHex(hashBuffer.data(), hashBuffer.size());
}

[[nodiscard]] bool FileMatches(const fs::path& path, const ManifestFile& file, Logger& logger)
{
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
    {
        return false;
    }

    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size != file.size)
    {
        return false;
    }

    try
    {
        return Sha256File(path) == file.sha256;
    }
    catch (const std::exception& hashError)
    {
        logger.Log("hash failed for " + WideToUtf8(path.wstring()) + ": " + hashError.what());
        return false;
    }
}

void WriteBytesAtomic(const fs::path& path, const std::vector<unsigned char>& bytes)
{
    fs::create_directories(path.parent_path());

    const fs::path temporaryPath = path.wstring() + L".download";
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not write downloaded file: " + WideToUtf8(temporaryPath.wstring()));
        }
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            throw std::runtime_error("Could not finish writing downloaded file: " + WideToUtf8(temporaryPath.wstring()));
        }
    }

    if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const std::string error = Win32Message(GetLastError());
        std::error_code ignored;
        fs::remove(temporaryPath, ignored);
        throw std::runtime_error("Could not move downloaded file into place: " + error);
    }
}

[[nodiscard]] std::string UrlEncodeRelativePath(const std::string& relativePath)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(relativePath.size());

    for (const unsigned char ch : relativePath)
    {
        const bool unreserved =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_' ||
            ch == '.' ||
            ch == '~' ||
            ch == '/';

        if (unreserved)
        {
            output.push_back(static_cast<char>(ch));
        }
        else
        {
            output.push_back('%');
            output.push_back(digits[(ch >> 4u) & 0x0Fu]);
            output.push_back(digits[ch & 0x0Fu]);
        }
    }

    return output;
}

[[nodiscard]] std::string JoinUrl(std::string baseUrl, const std::string& relativePath)
{
    if (!baseUrl.empty() && baseUrl.back() != '/')
    {
        baseUrl.push_back('/');
    }
    return baseUrl + UrlEncodeRelativePath(relativePath);
}

void RemoveDirectoryIfExists(const fs::path& path)
{
    std::error_code error;
    if (fs::exists(path, error))
    {
        fs::remove_all(path, error);
        if (error)
        {
            throw std::runtime_error("Could not remove directory: " + WideToUtf8(path.wstring()));
        }
    }
}

void CleanExtraPayloadFiles(const fs::path& payloadDirectory, const std::set<std::string>& wantedFiles, Logger& logger)
{
    if (!fs::exists(payloadDirectory))
    {
        return;
    }

    std::vector<fs::path> directories;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(payloadDirectory))
    {
        const fs::path relative = fs::relative(entry.path(), payloadDirectory);
        const std::string relativeText = relative.generic_string();

        if (relativeText.rfind(".update", 0) == 0 || relativeText.rfind(".launcher", 0) == 0)
        {
            continue;
        }

        std::error_code error;
        if (entry.is_directory(error))
        {
            directories.push_back(entry.path());
        }
        else if (entry.is_regular_file(error) && wantedFiles.find(relativeText) == wantedFiles.end())
        {
            logger.Log("removing stale runtime file: " + relativeText);
            fs::remove(entry.path(), error);
            if (error)
            {
                throw std::runtime_error("Could not remove stale runtime file: " + WideToUtf8(entry.path().wstring()));
            }
        }
    }

    std::sort(directories.rbegin(), directories.rend());
    for (const fs::path& directory : directories)
    {
        std::error_code error;
        fs::remove(directory, error);
    }
}

void WriteInstalledVersion(const fs::path& payloadDirectory, const Manifest& manifest)
{
    const fs::path stateDirectory = payloadDirectory / kLauncherStateDirectoryName;
    fs::create_directories(stateDirectory);

    std::ofstream output(stateDirectory / L"installed.txt", std::ios::trunc);
    if (!output)
    {
        return;
    }

    output << "version=" << manifest.version << "\n";
    output << "entrypoint=" << manifest.entrypoint << "\n";
    output << "launcher=" << kLauncherVersion << "\n";
}

[[nodiscard]] UpdateResult UpdateRuntime(
    const fs::path& payloadDirectory,
    const std::string& manifestUrl,
    const bool allowInsecureHttp,
    const bool forceRepair,
    const bool cleanRuntime,
    Logger& logger)
{
    UpdateMutex mutex(logger);

    HttpClient http(logger, allowInsecureHttp);
    const std::vector<unsigned char> manifestBytes = http.Download(manifestUrl, kMaxManifestBytes, "manifest");
    const std::string manifestText(manifestBytes.begin(), manifestBytes.end());
    const Manifest manifest = ParseManifest(manifestText, manifestUrl);

    logger.Log("manifest parsed; version=" + manifest.version + "; files=" + std::to_string(manifest.files.size()) + "; entrypoint=" + manifest.entrypoint);

    fs::create_directories(payloadDirectory);
    const fs::path stagingDirectory = payloadDirectory / kUpdateStagingDirectoryName;
    RemoveDirectoryIfExists(stagingDirectory);
    fs::create_directories(stagingDirectory);

    std::set<std::string> wantedFiles;
    std::vector<std::pair<fs::path, fs::path>> stagedFiles;

    UpdateResult result{};
    result.version = manifest.version;
    result.entrypoint = manifest.entrypoint;

    try
    {
        for (const ManifestFile& file : manifest.files)
        {
            const fs::path relativePath = SafePayloadRelativePath(file.path);
            const fs::path targetPath = payloadDirectory / relativePath;
            wantedFiles.insert(file.path);

            if (!forceRepair && FileMatches(targetPath, file, logger))
            {
                logger.Log("runtime file is current: " + file.path);
                continue;
            }

            const fs::path stagedPath = stagingDirectory / relativePath;
            const std::vector<unsigned char> bytes = http.Download(JoinUrl(manifest.baseUrl, file.path), file.size, file.path.c_str());
            if (bytes.size() != file.size)
            {
                throw std::runtime_error("Downloaded file size mismatch for " + file.path + ": expected " + std::to_string(file.size) + ", got " + std::to_string(bytes.size()));
            }

            WriteBytesAtomic(stagedPath, bytes);
            if (!FileMatches(stagedPath, file, logger))
            {
                throw std::runtime_error("Downloaded file failed sha256 validation: " + file.path);
            }

            ++result.downloadedFiles;
            stagedFiles.emplace_back(stagedPath, targetPath);
        }

        for (const auto& [stagedPath, targetPath] : stagedFiles)
        {
            fs::create_directories(targetPath.parent_path());
            if (!MoveFileExW(stagedPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::runtime_error("Could not install updated runtime file " + WideToUtf8(targetPath.wstring()) + ": " + Win32Message(GetLastError()));
            }
            ++result.installedFiles;
        }

        if (cleanRuntime || manifest.cleanExtra)
        {
            CleanExtraPayloadFiles(payloadDirectory, wantedFiles, logger);
        }

        (void)SafePayloadRelativePath(manifest.entrypoint);
        WriteInstalledVersion(payloadDirectory, manifest);
    }
    catch (...)
    {
        RemoveDirectoryIfExists(stagingDirectory);
        throw;
    }

    RemoveDirectoryIfExists(stagingDirectory);
    logger.Log("runtime update complete; downloaded=" + std::to_string(result.downloadedFiles) + "; installed=" + std::to_string(result.installedFiles));
    return result;
}


[[nodiscard]] std::optional<std::string> ReadInstalledEntrypoint(const fs::path& payloadDirectory, Logger& logger)
{
    const fs::path installedPath = payloadDirectory / kLauncherStateDirectoryName / L"installed.txt";
    std::ifstream input(installedPath);
    if (!input)
    {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(input, line))
    {
        constexpr std::string_view prefix = "entrypoint=";
        if (line.rfind(prefix, 0) == 0)
        {
            std::string value = line.substr(prefix.size());
            if (!value.empty())
            {
                (void)SafePayloadRelativePath(value);
                logger.Log("installed entrypoint loaded from state: " + value);
                return value;
            }
        }
    }

    return std::nullopt;
}

[[nodiscard]] bool RuntimeEntrypointExists(const fs::path& payloadDirectory, const std::string& entrypoint)
{
    std::error_code error;
    const fs::path executable = payloadDirectory / SafePayloadRelativePath(entrypoint);
    return fs::is_regular_file(executable, error);
}

[[nodiscard]] bool HasLaunchableRuntime(const fs::path& payloadDirectory, Logger& logger)
{
    if (RuntimeEntrypointExists(payloadDirectory, kDefaultEntrypoint))
    {
        return true;
    }

    const std::optional<std::string> installedEntrypoint = ReadInstalledEntrypoint(payloadDirectory, logger);
    return installedEntrypoint && RuntimeEntrypointExists(payloadDirectory, *installedEntrypoint);
}

[[nodiscard]] bool ShouldSkipSeedPath(const std::string& relativePath)
{
    if (relativePath.empty())
    {
        return true;
    }
    if (relativePath.rfind(".update", 0) == 0 || relativePath.rfind(".launcher", 0) == 0)
    {
        return true;
    }
    return false;
}

void CopyRuntimeBootstrap(const fs::path& bootstrapDirectory, const fs::path& payloadDirectory, Logger& logger)
{
    if (!fs::exists(bootstrapDirectory / Utf8ToWide(kDefaultEntrypoint)))
    {
        logger.Log("runtime bootstrap skipped: default entrypoint not found in " + WideToUtf8(bootstrapDirectory.wstring()));
        return;
    }

    logger.Log("seeding runtime from bootstrap directory: " + WideToUtf8(bootstrapDirectory.wstring()));
    std::size_t copiedFiles = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(bootstrapDirectory))
    {
        std::error_code error;
        if (!entry.is_regular_file(error))
        {
            continue;
        }

        const fs::path relativePath = fs::relative(entry.path(), bootstrapDirectory, error);
        if (error)
        {
            continue;
        }

        const std::string relativeText = relativePath.generic_string();
        if (ShouldSkipSeedPath(relativeText))
        {
            continue;
        }

        const fs::path destination = payloadDirectory / relativePath;
        fs::create_directories(destination.parent_path());
        fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, error);
        if (error)
        {
            throw std::runtime_error("Could not seed runtime file " + relativeText + ": " + error.message());
        }
        ++copiedFiles;
    }

    logger.Log("runtime bootstrap complete; copied_files=" + std::to_string(copiedFiles));
}

void SeedRuntimeFromBundledBootstrapIfNeeded(const fs::path& payloadDirectory, Logger& logger)
{
    if (HasLaunchableRuntime(payloadDirectory, logger))
    {
        logger.Log("runtime bootstrap not needed: launchable runtime already installed");
        return;
    }

    const fs::path bootstrapDirectory = ModuleDirectory() / kBootstrapPayloadDirectoryName;
    std::error_code error;
    if (!fs::exists(bootstrapDirectory, error))
    {
        logger.Log("runtime bootstrap not available: " + WideToUtf8(bootstrapDirectory.wstring()));
        return;
    }

    CopyRuntimeBootstrap(bootstrapDirectory, payloadDirectory, logger);
}

[[nodiscard]] std::wstring QuoteCommandLineArgument(const std::wstring_view argument)
{
    if (argument.empty())
    {
        return L"\"\"";
    }

    const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needsQuotes)
    {
        return std::wstring(argument);
    }

    std::wstring output = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (ch == L'"')
        {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(ch);
            backslashes = 0;
            continue;
        }

        output.append(backslashes, L'\\');
        backslashes = 0;
        output.push_back(ch);
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

struct LauncherOptions
{
    bool skipUpdate = false;
    bool forceRepair = false;
    bool cleanRuntime = false;
    bool portable = false;
    bool noWait = false;
    bool showHelp = false;
    bool allowInsecureHttp = false;
    std::string manifestUrl = DON_CRAFT_DEFAULT_UPDATE_MANIFEST_URL;
    std::optional<fs::path> runtimeDirectory;
    std::vector<std::wstring> gameArguments;
};

[[nodiscard]] LauncherOptions ParseLauncherOptions()
{
    int argumentCount = 0;
    LPWSTR* const arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr)
    {
        ThrowLastError("CommandLineToArgvW failed");
    }

    LauncherOptions options{};
    bool passThrough = false;

    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring_view argument(arguments[index]);

        if (passThrough)
        {
            options.gameArguments.emplace_back(argument);
            continue;
        }

        if (argument == L"--")
        {
            passThrough = true;
            continue;
        }
        if (argument == L"--help" || argument == L"-h" || argument == L"/?")
        {
            options.showHelp = true;
            continue;
        }
        if (argument == L"--doncraft-no-update")
        {
            options.skipUpdate = true;
            continue;
        }
        if (argument == L"--doncraft-repair" || argument == L"--doncraft-force-update")
        {
            options.forceRepair = true;
            continue;
        }
        if (argument == L"--doncraft-clean-runtime")
        {
            options.cleanRuntime = true;
            continue;
        }
        if (argument == L"--doncraft-portable")
        {
            options.portable = true;
            continue;
        }
        if (argument == L"--doncraft-no-wait")
        {
            options.noWait = true;
            continue;
        }
        if (argument == L"--doncraft-allow-insecure-http")
        {
            options.allowInsecureHttp = true;
            continue;
        }
        if (argument == L"--doncraft-manifest-url" && index + 1 < argumentCount)
        {
            ++index;
            options.manifestUrl = WideToUtf8(arguments[index]);
            continue;
        }
        constexpr std::wstring_view manifestPrefix = L"--doncraft-manifest-url=";
        if (argument.rfind(manifestPrefix, 0) == 0)
        {
            options.manifestUrl = WideToUtf8(argument.substr(manifestPrefix.size()));
            continue;
        }
        if (argument == L"--doncraft-runtime-dir" && index + 1 < argumentCount)
        {
            ++index;
            options.runtimeDirectory = fs::path(arguments[index]);
            continue;
        }
        constexpr std::wstring_view runtimePrefix = L"--doncraft-runtime-dir=";
        if (argument.rfind(runtimePrefix, 0) == 0)
        {
            options.runtimeDirectory = fs::path(std::wstring(argument.substr(runtimePrefix.size())));
            continue;
        }

        options.gameArguments.emplace_back(argument);
    }

    LocalFree(arguments);
    return options;
}

[[nodiscard]] fs::path SelectPayloadDirectory(const LauncherOptions& options, Logger& logger)
{
    if (options.runtimeDirectory)
    {
        logger.Log("runtime directory selected from command line: " + WideToUtf8(options.runtimeDirectory->wstring()));
        return *options.runtimeDirectory;
    }

    const fs::path launcherDirectory = ModuleDirectory();
    const fs::path portableDirectory = launcherDirectory / kPortablePayloadDirectoryName;
    const fs::path portableMarker = launcherDirectory / kPortableMarkerFileName;

    std::error_code error;
    if (options.portable || fs::exists(portableMarker, error) || fs::exists(portableDirectory, error))
    {
        logger.Log("runtime directory selected in portable mode: " + WideToUtf8(portableDirectory.wstring()));
        return portableDirectory;
    }

    const fs::path appDataRuntime = DefaultAppDataDirectory() / kDefaultPayloadDirectoryName;
    logger.Log("runtime directory selected in appdata mode: " + WideToUtf8(appDataRuntime.wstring()));
    return appDataRuntime;
}

[[nodiscard]] DWORD LaunchGame(
    const fs::path& executable,
    const std::vector<std::wstring>& arguments,
    const bool waitForExit,
    Logger& logger)
{
    std::wstring commandLine = QuoteCommandLineArgument(executable.wstring());
    for (const std::wstring& argument : arguments)
    {
        commandLine.push_back(L' ');
        commandLine.append(QuoteCommandLineArgument(argument));
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring workingDirectory = executable.parent_path().wstring();

    SetEnvironmentVariableW(L"DONCRAFT_LAUNCHER_VERSION", Utf8ToWide(kLauncherVersion).c_str());
    SetEnvironmentVariableW(L"DONCRAFT_RUNTIME_DIR", workingDirectory.c_str());

    logger.Log("launching game: " + WideToUtf8(commandLine));
    if (!CreateProcessW(
            executable.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            workingDirectory.c_str(),
            &startupInfo,
            &processInfo))
    {
        ThrowLastError("CreateProcessW failed");
    }

    WinHandle thread(processInfo.hThread);
    WinHandle process(processInfo.hProcess);

    if (!waitForExit)
    {
        logger.Log("game process started without waiting");
        return 0;
    }

    WaitForSingleObject(process.value, INFINITE);
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.value, &exitCode))
    {
        logger.Log("GetExitCodeProcess failed: " + Win32Message(GetLastError()));
        exitCode = 1;
    }
    logger.Log("game exited with code " + std::to_string(exitCode));
    return exitCode;
}

void ShowErrorMessage(const std::string& message)
{
    const std::wstring wideMessage = Utf8ToWide(message);
    MessageBoxW(nullptr, wideMessage.c_str(), kLauncherTitle, MB_OK | MB_ICONERROR);
}

void ShowInfoMessage(const std::string& message)
{
    const std::wstring wideMessage = Utf8ToWide(message);
    MessageBoxW(nullptr, wideMessage.c_str(), kLauncherTitle, MB_OK | MB_ICONINFORMATION);
}

[[nodiscard]] std::string HelpText()
{
    return
        "DonCraft Launcher " + std::string(kLauncherVersion) + "\n\n"
        "Launcher options:\n"
        "  --doncraft-no-update             Launch the installed runtime without checking for updates.\n"
        "  --doncraft-repair                Redownload and verify every manifest file.\n"
        "  --doncraft-clean-runtime         Remove runtime files that are not listed in the manifest.\n"
        "  --doncraft-portable              Store runtime beside the launcher in DonCraftRuntime.\n"
        "  --doncraft-runtime-dir <path>    Store or read runtime from a specific directory.\n"
        "  --doncraft-manifest-url <url>    Use a different update manifest.\n"
        "  --doncraft-no-wait               Start the game and close the launcher immediately.\n"
        "  --doncraft-allow-insecure-http   Allow non-HTTPS manifest/files for local testing only.\n"
        "  --                               Pass all remaining arguments to the game.\n\n"
        "Default manifest:\n"
        "  " DON_CRAFT_DEFAULT_UPDATE_MANIFEST_URL "\n\n"
        "First-run bootstrap:\n"
        "  If DonCraftBootstrap exists beside the launcher, it is copied into the selected runtime directory before updating.\n";
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Logger logger(DefaultAppDataDirectory() / kLauncherLogFileName);

    try
    {
        const LauncherOptions options = ParseLauncherOptions();
        if (options.showHelp)
        {
            ShowInfoMessage(HelpText());
            return 0;
        }

        const fs::path payloadDirectory = SelectPayloadDirectory(options, logger);
        SeedRuntimeFromBundledBootstrapIfNeeded(payloadDirectory, logger);

        std::string entrypoint = ReadInstalledEntrypoint(payloadDirectory, logger).value_or(kDefaultEntrypoint);
        std::string updateError;

        if (!options.skipUpdate)
        {
            try
            {
                const UpdateResult result = UpdateRuntime(
                    payloadDirectory,
                    options.manifestUrl,
                    options.allowInsecureHttp,
                    options.forceRepair,
                    options.cleanRuntime,
                    logger);
                entrypoint = result.entrypoint;
            }
            catch (const std::exception& error)
            {
                updateError = error.what();
                logger.Log("update failed: " + updateError);
            }
        }
        else
        {
            logger.Log("update skipped by command line");
        }

        const fs::path executable = payloadDirectory / SafePayloadRelativePath(entrypoint);
        std::error_code existsError;
        if (!fs::exists(executable, existsError))
        {
            std::string message = "DonCraft could not start because no local runtime is installed.\n\n";
            message += "Expected executable:\n" + WideToUtf8(executable.wstring()) + "\n\n";
            if (!updateError.empty())
            {
                message += "Update failed:\n" + updateError + "\n\n";
            }
            message += "Launcher log:\n" + WideToUtf8(logger.Path().wstring());
            ShowErrorMessage(message);
            return 1;
        }

        if (!updateError.empty())
        {
            logger.Log("continuing with installed runtime after update failure");
        }

        return static_cast<int>(LaunchGame(executable, options.gameArguments, !options.noWait, logger));
    }
    catch (const std::exception& error)
    {
        logger.Log(std::string("fatal launcher error: ") + error.what());

        std::string message = error.what();
        message += "\n\nLauncher log:\n" + WideToUtf8(logger.Path().wstring());
        ShowErrorMessage(message);
        return 1;
    }
}
