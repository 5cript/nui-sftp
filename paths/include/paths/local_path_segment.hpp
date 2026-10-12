#pragma once

#include <paths/path_segment.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace Paths
{
    /**
     * @brief Naming rules of the local filesystem.
     */
    enum class LocalPlatform : std::uint8_t
    {
        Posix,
        Windows,
    };

    /**
     * @brief The platform this binary runs on.
     */
    constexpr LocalPlatform compiledLocalPlatform()
    {
#ifdef _WIN32
        return LocalPlatform::Windows;
#else
        return LocalPlatform::Posix;
#endif
    }

    /**
     * @brief The local platform paths default to. The frontend runs in WebAssembly and sets the backend's here.
     */
    inline LocalPlatform& defaultLocalPlatform()
    {
        static LocalPlatform platform = compiledLocalPlatform();
        return platform;
    }

    /**
     * @brief A @ref PathSegment that can be created on the local filesystem of @p platform.
     *
     * Windows additionally refuses '\\', ':', '<', '>', '"', '|', '?', '*', control characters, device names
     * like CON or LPT1 (also with an extension) and a trailing dot or space. A remote name has to pass here
     * before it becomes part of a local path.
     */
    class LocalPathSegment
    {
      public:
        static std::expected<LocalPathSegment, PathError>
        make(PathSegment const& segment, LocalPlatform platform = defaultLocalPlatform())
        {
            if (platform == LocalPlatform::Windows)
            {
                if (auto reason = windowsRejection(segment.view()); reason)
                    return std::unexpected(PathError{.reason = *reason, .input = segment.value()});
            }
            return LocalPathSegment{segment};
        }

        static std::expected<LocalPathSegment, PathError>
        make(std::string_view utf8, LocalPlatform platform = defaultLocalPlatform())
        {
            return PathSegment::make(utf8).and_then(
                [platform](PathSegment const& segment)
                {
                    return make(segment, platform);
                }
            );
        }

        PathSegment const& segment() const noexcept
        {
            return segment_;
        }

        std::string const& value() const noexcept
        {
            return segment_.value();
        }

        auto operator<=>(LocalPathSegment const&) const = default;

      private:
        explicit LocalPathSegment(PathSegment segment)
            : segment_{std::move(segment)}
        {}

        static std::optional<PathRejection> windowsRejection(std::string_view name)
        {
            for (const auto character : name)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (byte < 0x20u || std::string_view{"\\:<>\"|?*"}.find(character) != std::string_view::npos)
                    return PathRejection::ReservedCharacter;
            }
            if (name.back() == '.' || name.back() == ' ')
                return PathRejection::TrailingDotOrSpace;

            // Device names count with any extension: "nul.txt" is the NUL device too.
            const auto stem = name.substr(0, name.find('.'));
            std::string upper{stem};
            std::transform(
                upper.begin(),
                upper.end(),
                upper.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::toupper(character));
                }
            );
            constexpr std::array<std::string_view, 4> devices{"CON", "PRN", "AUX", "NUL"};
            if (std::ranges::find(devices, upper) != devices.end())
                return PathRejection::ReservedDeviceName;
            if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '1' &&
                upper[3] <= '9')
                return PathRejection::ReservedDeviceName;
            return std::nullopt;
        }

        PathSegment segment_;
    };
}
