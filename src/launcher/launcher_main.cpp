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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
constexpr wchar_t kPayloadDirectoryName[] = L"DonCraftRuntime";
constexpr char kDefaultEntrypoint[] = "Don_Craft_client.exe";

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
    std::vector<ManifestFile> files;
};

[[nodiscard]] std::wstring Utf8ToWide(const std::string_view text)
{
    if (text.empty())
    {
        return {};
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

[[nodiscard]] std::string Win32Message(const DWORD error)
{
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
        return "Windows error " + std::to_string(error);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.' || message.back() == L' '))
    {
        message.pop_back();
    }
    return WideToUtf8(message);
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

class HttpClient
{
public:
    HttpClient()
        : session_(WinHttpOpen(L"DonCraftLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
    {
        if (!session_)
        {
            ThrowLastError("WinHttpOpen failed");
        }
        WinHttpSetTimeouts(session_.value, 10000, 10000, 30000, 30000);
    }

    [[nodiscard]] std::vector<unsigned char> Download(const std::string& url) const
    {
        const ParsedUrl parsed = ParseUrl(url);
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

        if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
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

        std::vector<unsigned char> bytes;
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

            const std::size_t offset = bytes.size();
            bytes.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.value, bytes.data() + offset, available, &read))
            {
                ThrowLastError("WinHttpReadData failed");
            }
            bytes.resize(offset + read);
        }
        return bytes;
    }

private:
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
        value = (value * 10u) + static_cast<unsigned char>(text[index] - '0');
        ++index;
    }
    return value;
}

[[nodiscard]] std::optional<std::size_t> FindValueStart(const std::string_view text, const std::string_view key)
{
    const std::string pattern = "\"" + std::string(key) + "\"";
    const std::size_t keyPosition = text.find(pattern);
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
        if (file.path.empty() || file.sha256.size() != 64 || file.size == 0)
        {
            throw std::runtime_error("Update manifest contains an invalid file entry.");
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

[[nodiscard]] std::string Sha256File(const fs::path& path)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(status))
    {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed.");
    }

    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD propertyLength = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &propertyLength, 0);
    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed.");
    }
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &propertyLength, 0);
    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptGetProperty(BCRYPT_HASH_LENGTH) failed.");
    }

    std::vector<unsigned char> objectBuffer(objectLength);
    std::vector<unsigned char> hashBuffer(hashLength);
    status = BCryptCreateHash(algorithm, &hash, objectBuffer.data(), objectLength, nullptr, 0, 0);
    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptCreateHash failed.");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Could not open file for hashing: " + WideToUtf8(path.wstring()));
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
        {
            status = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0);
            if (!NT_SUCCESS(status))
            {
                BCryptDestroyHash(hash);
                BCryptCloseAlgorithmProvider(algorithm, 0);
                throw std::runtime_error("BCryptHashData failed.");
            }
        }
    }

    status = BCryptFinishHash(hash, hashBuffer.data(), hashLength, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!NT_SUCCESS(status))
    {
        throw std::runtime_error("BCryptFinishHash failed.");
    }
    return BytesToHex(hashBuffer.data(), hashBuffer.size());
}

[[nodiscard]] bool FileMatches(const fs::path& path, const ManifestFile& file)
{
    std::error_code error;
    if (!fs::is_regular_file(path, error))
    {
        return false;
    }
    if (fs::file_size(path, error) != file.size || error)
    {
        return false;
    }
    return Sha256File(path) == file.sha256;
}

void WriteBytes(const fs::path& path, const std::vector<unsigned char>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("Could not write downloaded file: " + WideToUtf8(path.wstring()));
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output)
    {
        throw std::runtime_error("Could not finish writing downloaded file: " + WideToUtf8(path.wstring()));
    }
}

[[nodiscard]] std::string JoinUrl(std::string baseUrl, const std::string& relativePath)
{
    if (!baseUrl.empty() && baseUrl.back() != '/')
    {
        baseUrl.push_back('/');
    }
    return baseUrl + relativePath;
}

void RemoveDirectoryIfExists(const fs::path& path)
{
    std::error_code error;
    if (fs::exists(path, error))
    {
        fs::remove_all(path, error);
        if (error)
        {
            throw std::runtime_error("Could not remove temporary update directory: " + WideToUtf8(path.wstring()));
        }
    }
}

void CleanExtraPayloadFiles(const fs::path& payloadDirectory, const std::set<std::string>& wantedFiles)
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
        if (relativeText.rfind(".update", 0) == 0)
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

[[nodiscard]] std::string UpdateRuntime(const fs::path& payloadDirectory, const std::string& manifestUrl)
{
    HttpClient http{};
    const std::vector<unsigned char> manifestBytes = http.Download(manifestUrl);
    const std::string manifestText(manifestBytes.begin(), manifestBytes.end());
    const Manifest manifest = ParseManifest(manifestText, manifestUrl);

    fs::create_directories(payloadDirectory);
    const fs::path stagingDirectory = payloadDirectory / L".update";
    RemoveDirectoryIfExists(stagingDirectory);
    fs::create_directories(stagingDirectory);

    std::set<std::string> wantedFiles;
    std::vector<std::pair<fs::path, fs::path>> stagedFiles;

    try
    {
        for (const ManifestFile& file : manifest.files)
        {
            const fs::path relativePath = SafePayloadRelativePath(file.path);
            const fs::path targetPath = payloadDirectory / relativePath;
            wantedFiles.insert(file.path);

            if (FileMatches(targetPath, file))
            {
                continue;
            }

            const fs::path stagedPath = stagingDirectory / relativePath;
            const std::vector<unsigned char> bytes = http.Download(JoinUrl(manifest.baseUrl, file.path));
            WriteBytes(stagedPath, bytes);
            if (!FileMatches(stagedPath, file))
            {
                throw std::runtime_error("Downloaded file failed validation: " + file.path);
            }
            stagedFiles.emplace_back(stagedPath, targetPath);
        }

        for (const auto& [stagedPath, targetPath] : stagedFiles)
        {
            fs::create_directories(targetPath.parent_path());
            std::error_code error;
            fs::copy_file(stagedPath, targetPath, fs::copy_options::overwrite_existing, error);
            if (error)
            {
                throw std::runtime_error("Could not install updated runtime file: " + WideToUtf8(targetPath.wstring()));
            }
        }

        CleanExtraPayloadFiles(payloadDirectory, wantedFiles);
    }
    catch (...)
    {
        RemoveDirectoryIfExists(stagingDirectory);
        throw;
    }

    RemoveDirectoryIfExists(stagingDirectory);
    (void)SafePayloadRelativePath(manifest.entrypoint);
    return manifest.entrypoint;
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
    std::string manifestUrl = DON_CRAFT_DEFAULT_UPDATE_MANIFEST_URL;
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
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--doncraft-no-update")
        {
            options.skipUpdate = true;
            continue;
        }
        if (argument == L"--doncraft-manifest-url" && index + 1 < argumentCount)
        {
            ++index;
            options.manifestUrl = WideToUtf8(arguments[index]);
            continue;
        }
        constexpr std::wstring_view prefix = L"--doncraft-manifest-url=";
        if (argument.rfind(prefix, 0) == 0)
        {
            options.manifestUrl = WideToUtf8(argument.substr(prefix.size()));
            continue;
        }
        options.gameArguments.emplace_back(argument);
    }

    LocalFree(arguments);
    return options;
}

[[nodiscard]] DWORD LaunchGameAndWait(const fs::path& executable, const std::vector<std::wstring>& arguments)
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

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode))
    {
        exitCode = 1;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return exitCode;
}

void ShowErrorMessage(const std::string& message)
{
    const std::wstring wideMessage = Utf8ToWide(message);
    MessageBoxW(nullptr, wideMessage.c_str(), kLauncherTitle, MB_OK | MB_ICONERROR);
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        const LauncherOptions options = ParseLauncherOptions();
        const fs::path launcherDirectory = ModuleDirectory();
        const fs::path payloadDirectory = launcherDirectory / kPayloadDirectoryName;

        std::string entrypoint = kDefaultEntrypoint;
        std::string updateError;
        if (!options.skipUpdate)
        {
            try
            {
                entrypoint = UpdateRuntime(payloadDirectory, options.manifestUrl);
            }
            catch (const std::exception& error)
            {
                updateError = error.what();
            }
        }

        const fs::path executable = payloadDirectory / SafePayloadRelativePath(entrypoint);
        if (!fs::exists(executable))
        {
            std::string message = "DonCraft could not start because no local runtime is installed.";
            if (!updateError.empty())
            {
                message += "\n\nUpdate failed: " + updateError;
            }
            ShowErrorMessage(message);
            return 1;
        }

        return static_cast<int>(LaunchGameAndWait(executable, options.gameArguments));
    }
    catch (const std::exception& error)
    {
        ShowErrorMessage(error.what());
        return 1;
    }
}
