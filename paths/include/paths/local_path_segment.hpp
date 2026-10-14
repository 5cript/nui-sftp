#pragma once

#include <paths/path_segment.hpp>
#include <paths/utf8.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
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
     * @brief The local platform paths default to. The frontend runs in WebAssembly and sets the backend's here
     *        before it reads any local path.
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
     * like CON, CONIN$, COM0 or LPT¹ (also with an extension or spaces before it), a trailing dot or space and
     * names longer than 255 UTF-16 code units. A remote name has to pass here before it becomes part of a local
     * path.
     */
    class LocalPathSegment
    {
      public:
        static std::expected<LocalPathSegment, PathError>
        make(PathSegment const& segment, LocalPlatform platform = defaultLocalPlatform())
        {
            if (platform == LocalPlatform::Windows)
            {
                if (const auto reason = windowsRejection(segment.view()); reason)
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

        /**
         * @brief Why @p utf8 cannot be a local name on @p platform, nothing when it can.
         */
        static std::optional<PathRejection>
        rejectionOf(std::string_view utf8, LocalPlatform platform = defaultLocalPlatform())
        {
            if (const auto rejection = PathSegment::rejectionOf(utf8); rejection)
                return rejection;
            if (platform == LocalPlatform::Windows)
                return windowsRejection(utf8);
            return std::nullopt;
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

        /**
         * @brief The Windows rules for a valid @ref PathSegment.
         */
        static std::optional<PathRejection> windowsRejection(std::string_view name)
        {
            constexpr std::size_t maximumNameLength = 255u;
            const auto reserved = [](char character)
            {
                return static_cast<unsigned char>(character) < 0x20u ||
                    std::string_view{"\\:<>\"|?*"}.find(character) != std::string_view::npos;
            };
            if (std::ranges::any_of(name, reserved))
                return PathRejection::ReservedCharacter;
            if (name.back() == '.' || name.back() == ' ')
                return PathRejection::TrailingDotOrSpace;
            if (isDeviceName(name))
                return PathRejection::ReservedDeviceName;
            if (utf16Length(name) > maximumNameLength)
                return PathRejection::NameTooLong;
            return std::nullopt;
        }

        /**
         * @brief True for a Windows device name. It counts with any extension and with spaces before it:
         *        "nul.txt" and "CON .log" are devices too.
         */
        static bool isDeviceName(std::string_view name)
        {
            auto stem = name.substr(0, name.find('.'));
            while (!stem.empty() && stem.back() == ' ')
                stem.remove_suffix(1);

            std::string upper{stem};
            std::ranges::transform(
                upper,
                upper.begin(),
                [](char character)
                {
                    return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
                                                                 : character;
                }
            );
            constexpr std::array<std::string_view, 6> devices{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
            if (std::ranges::find(devices, upper) != devices.end())
                return true;
            if (!upper.starts_with("COM") && !upper.starts_with("LPT"))
                return false;

            const auto number = std::string_view{upper}.substr(3);
            constexpr std::array<std::string_view, 3> superscripts{"\xC2\xB9", "\xC2\xB2", "\xC2\xB3"};
            return (number.size() == 1u && number[0] >= '0' && number[0] <= '9') ||
                std::ranges::find(superscripts, number) != superscripts.end();
        }

        PathSegment segment_;
    };
}
