#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace df
{
class ByteWriter
{
public:
    template <typename T>
    void WritePod(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "ByteWriter only supports trivially copyable POD types.");

        const auto currentSize = bytes_.size();
        bytes_.resize(currentSize + sizeof(T));
        std::memcpy(bytes_.data() + currentSize, &value, sizeof(T));
    }

    void WriteBool(const bool value)
    {
        const std::uint8_t packed = value ? 1u : 0u;
        WritePod(packed);
    }

    void WriteString(const std::string_view value)
    {
        const auto size = static_cast<std::uint32_t>(value.size());
        WritePod(size);

        if (!value.empty())
        {
            WriteBytes(std::as_bytes(std::span(value.data(), value.size())));
        }
    }

    void WriteBytes(const std::span<const std::byte> bytes)
    {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] std::span<const std::byte> Span() const
    {
        return std::span<const std::byte>(bytes_.data(), bytes_.size());
    }

    [[nodiscard]] const std::vector<std::byte>& Data() const
    {
        return bytes_;
    }

    [[nodiscard]] std::vector<std::byte> TakeData()
    {
        return std::move(bytes_);
    }

private:
    std::vector<std::byte> bytes_;
};

class ByteReader
{
public:
    explicit ByteReader(const std::span<const std::byte> bytes)
        : bytes_(bytes)
    {
    }

    template <typename T>
    [[nodiscard]] T ReadPod()
    {
        static_assert(std::is_trivially_copyable_v<T>, "ByteReader only supports trivially copyable POD types.");
        Require(sizeof(T));

        T value{};
        std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    [[nodiscard]] bool ReadBool()
    {
        return ReadPod<std::uint8_t>() != 0u;
    }

    [[nodiscard]] std::string ReadString()
    {
        const auto length = ReadPod<std::uint32_t>();
        Require(length);

        std::string value(length, '\0');
        if (length > 0)
        {
            std::memcpy(value.data(), bytes_.data() + offset_, length);
            offset_ += length;
        }

        return value;
    }

    [[nodiscard]] std::span<const std::byte> ReadBytes(const std::size_t length)
    {
        Require(length);

        const auto result = bytes_.subspan(offset_, length);
        offset_ += length;
        return result;
    }

    [[nodiscard]] bool Empty() const
    {
        return offset_ >= bytes_.size();
    }

    [[nodiscard]] std::size_t RemainingBytes() const
    {
        return bytes_.size() - offset_;
    }

private:
    void Require(const std::size_t length) const
    {
        if (offset_ + length > bytes_.size())
        {
            throw std::runtime_error("ByteReader attempted to read past the end of the buffer.");
        }
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};
}
