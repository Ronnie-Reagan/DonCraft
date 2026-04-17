#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace df
{
inline constexpr float kPi = 3.14159265358979323846f;

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    auto operator+=(const Vec2& other) -> Vec2&
    {
        x += other.x;
        y += other.y;
        return *this;
    }

    auto operator-=(const Vec2& other) -> Vec2&
    {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    auto operator*=(const float scalar) -> Vec2&
    {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    auto operator/=(const float scalar) -> Vec2&
    {
        x /= scalar;
        y /= scalar;
        return *this;
    }
};

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    auto operator+=(const Vec3& other) -> Vec3&
    {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    auto operator-=(const Vec3& other) -> Vec3&
    {
        x -= other.x;
        y -= other.y;
        z -= other.z;
        return *this;
    }

    auto operator*=(const float scalar) -> Vec3&
    {
        x *= scalar;
        y *= scalar;
        z *= scalar;
        return *this;
    }

    auto operator/=(const float scalar) -> Vec3&
    {
        x /= scalar;
        y /= scalar;
        z /= scalar;
        return *this;
    }
};

struct Vec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Mat4
{
    std::array<float, 16> values{};

    [[nodiscard]] auto operator()(const int row, const int column) -> float&
    {
        return values[static_cast<std::size_t>(column) * 4u + static_cast<std::size_t>(row)];
    }

    [[nodiscard]] auto operator()(const int row, const int column) const -> float
    {
        return values[static_cast<std::size_t>(column) * 4u + static_cast<std::size_t>(row)];
    }
};

struct Int3
{
    int x = 0;
    int y = 0;
    int z = 0;
};

struct Ray
{
    Vec3 origin{};
    Vec3 direction{};
};

inline auto operator+(Vec2 lhs, const Vec2& rhs) -> Vec2
{
    lhs += rhs;
    return lhs;
}

inline auto operator-(Vec2 lhs, const Vec2& rhs) -> Vec2
{
    lhs -= rhs;
    return lhs;
}

inline auto operator*(Vec2 lhs, const float scalar) -> Vec2
{
    lhs *= scalar;
    return lhs;
}

inline auto operator*(const float scalar, Vec2 rhs) -> Vec2
{
    rhs *= scalar;
    return rhs;
}

inline auto operator/(Vec2 lhs, const float scalar) -> Vec2
{
    lhs /= scalar;
    return lhs;
}

inline auto operator+(Vec3 lhs, const Vec3& rhs) -> Vec3
{
    lhs += rhs;
    return lhs;
}

inline auto operator-(Vec3 lhs, const Vec3& rhs) -> Vec3
{
    lhs -= rhs;
    return lhs;
}

inline auto operator-(const Vec3 value) -> Vec3
{
    return {-value.x, -value.y, -value.z};
}

inline auto operator*(Vec3 lhs, const float scalar) -> Vec3
{
    lhs *= scalar;
    return lhs;
}

inline auto operator*(const float scalar, Vec3 rhs) -> Vec3
{
    rhs *= scalar;
    return rhs;
}

inline auto operator/(Vec3 lhs, const float scalar) -> Vec3
{
    lhs /= scalar;
    return lhs;
}

inline auto Dot(const Vec2& lhs, const Vec2& rhs) -> float
{
    return lhs.x * rhs.x + lhs.y * rhs.y;
}

inline auto Dot(const Vec3& lhs, const Vec3& rhs) -> float
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

inline auto Cross(const Vec3& lhs, const Vec3& rhs) -> Vec3
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

inline auto LengthSquared(const Vec2& value) -> float
{
    return Dot(value, value);
}

inline auto LengthSquared(const Vec3& value) -> float
{
    return Dot(value, value);
}

inline auto Length(const Vec2& value) -> float
{
    return std::sqrt(LengthSquared(value));
}

inline auto Length(const Vec3& value) -> float
{
    return std::sqrt(LengthSquared(value));
}

inline auto Normalize(const Vec3& value) -> Vec3
{
    const float length = Length(value);
    return length > 0.00001f ? value / length : Vec3{};
}

inline auto Normalize(const Vec2& value) -> Vec2
{
    const float length = Length(value);
    return length > 0.00001f ? value / length : Vec2{};
}

inline auto Clamp(const float value, const float minValue, const float maxValue) -> float
{
    return std::clamp(value, minValue, maxValue);
}

inline auto Lerp(const float a, const float b, const float t) -> float
{
    return a + (b - a) * t;
}

inline auto Lerp(const Vec3& a, const Vec3& b, const float t) -> Vec3
{
    return {
        Lerp(a.x, b.x, t),
        Lerp(a.y, b.y, t),
        Lerp(a.z, b.z, t),
    };
}

inline auto DegreesToRadians(const float degrees) -> float
{
    return degrees * (kPi / 180.0f);
}

inline auto IdentityMatrix() -> Mat4
{
    Mat4 matrix{};
    matrix(0, 0) = 1.0f;
    matrix(1, 1) = 1.0f;
    matrix(2, 2) = 1.0f;
    matrix(3, 3) = 1.0f;
    return matrix;
}

inline auto operator*(const Mat4& lhs, const Mat4& rhs) -> Mat4
{
    Mat4 result{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                value += lhs(row, k) * rhs(k, column);
            }
            result(row, column) = value;
        }
    }
    return result;
}

inline auto PerspectiveMatrix(const float verticalFovRadians, const float aspectRatio, const float nearPlane, const float farPlane) -> Mat4
{
    const float tanHalfFov = std::tan(verticalFovRadians * 0.5f);

    Mat4 matrix{};
    matrix(0, 0) = 1.0f / (aspectRatio * tanHalfFov);
    matrix(1, 1) = -1.0f / tanHalfFov;
    matrix(2, 2) = farPlane / (nearPlane - farPlane);
    matrix(2, 3) = (nearPlane * farPlane) / (nearPlane - farPlane);
    matrix(3, 2) = -1.0f;
    return matrix;
}

inline auto OrthographicMatrix(
    const float left,
    const float right,
    const float bottom,
    const float top,
    const float nearPlane,
    const float farPlane) -> Mat4
{
    Mat4 matrix = IdentityMatrix();
    matrix(0, 0) = 2.0f / (right - left);
    matrix(1, 1) = 2.0f / (top - bottom);
    matrix(2, 2) = 1.0f / (nearPlane - farPlane);
    matrix(0, 3) = -(right + left) / (right - left);
    matrix(1, 3) = -(top + bottom) / (top - bottom);
    matrix(2, 3) = nearPlane / (nearPlane - farPlane);
    return matrix;
}

inline auto LookAtMatrix(const Vec3& eye, const Vec3& target, const Vec3& up) -> Mat4
{
    const Vec3 forward = Normalize(target - eye);
    const Vec3 right = Normalize(Cross(forward, up));
    const Vec3 cameraUp = Cross(right, forward);

    Mat4 matrix = IdentityMatrix();
    matrix(0, 0) = right.x;
    matrix(0, 1) = right.y;
    matrix(0, 2) = right.z;
    matrix(0, 3) = -Dot(right, eye);
    matrix(1, 0) = cameraUp.x;
    matrix(1, 1) = cameraUp.y;
    matrix(1, 2) = cameraUp.z;
    matrix(1, 3) = -Dot(cameraUp, eye);
    matrix(2, 0) = -forward.x;
    matrix(2, 1) = -forward.y;
    matrix(2, 2) = -forward.z;
    matrix(2, 3) = Dot(forward, eye);
    return matrix;
}

inline auto TransformPoint(const Mat4& matrix, const Vec3& point) -> Vec3
{
    const float x =
        matrix(0, 0) * point.x +
        matrix(0, 1) * point.y +
        matrix(0, 2) * point.z +
        matrix(0, 3);
    const float y =
        matrix(1, 0) * point.x +
        matrix(1, 1) * point.y +
        matrix(1, 2) * point.z +
        matrix(1, 3);
    const float z =
        matrix(2, 0) * point.x +
        matrix(2, 1) * point.y +
        matrix(2, 2) * point.z +
        matrix(2, 3);
    const float w =
        matrix(3, 0) * point.x +
        matrix(3, 1) * point.y +
        matrix(3, 2) * point.z +
        matrix(3, 3);

    if (std::abs(w) <= 1.0e-6f)
    {
        return {x, y, z};
    }

    return {x / w, y / w, z / w};
}

inline auto MakeColor(const float red, const float green, const float blue, const float alpha = 1.0f) -> Vec4
{
    return {red, green, blue, alpha};
}
}
