#pragma once

#include <paths/paths.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace FileExplorer
{
    /**
     * @brief The drop target that stands for the parent directory, the ".." entry of a listing.
     */
    inline constexpr std::string_view parentDirectoryTarget{".."};

    inline std::string const& pathText(Paths::RemotePath const& path)
    {
        return path.value();
    }

    inline std::string const& pathText(Paths::LocalPath const& path)
    {
        return path.toUtf8();
    }

    /**
     * @brief Parses the UTF-8 text of an absolute path of the side @p PathT names.
     */
    template <typename PathT>
    std::expected<PathT, Paths::PathError> parsePath(std::string_view utf8);

    template <>
    inline std::expected<Paths::RemotePath, Paths::PathError> parsePath<Paths::RemotePath>(std::string_view utf8)
    {
        return Paths::RemotePath::make(utf8);
    }

    template <>
    inline std::expected<Paths::LocalPath, Paths::PathError> parsePath<Paths::LocalPath>(std::string_view utf8)
    {
        return Paths::LocalPath::fromUtf8(utf8);
    }

    /**
     * @brief @p name directly below @p directory, refused when it is no single name the server allows.
     */
    inline std::expected<Paths::RemotePath, Paths::PathError>
    childOf(Paths::RemotePath const& directory, std::string_view name)
    {
        return Paths::PathSegment::make(name).transform(
            [&directory](Paths::PathSegment const& segment)
            {
                return directory / segment;
            }
        );
    }

    inline std::expected<Paths::RemotePath, Paths::PathError>
    childOf(Paths::RemotePath const& directory, Paths::PathSegment const& name)
    {
        return directory / name;
    }

    /**
     * @brief @p name directly below @p directory, refused when it is no single name the platform of @p directory
     *        allows.
     */
    inline std::expected<Paths::LocalPath, Paths::PathError>
    childOf(Paths::LocalPath const& directory, std::string_view name)
    {
        return Paths::LocalPathSegment::make(name, directory.platform())
            .transform(
                [&directory](Paths::LocalPathSegment const& segment)
                {
                    return directory / segment;
                }
            );
    }

    inline std::expected<Paths::LocalPath, Paths::PathError>
    childOf(Paths::LocalPath const& directory, Paths::PathSegment const& name)
    {
        return Paths::LocalPathSegment::make(name, directory.platform())
            .transform(
                [&directory](Paths::LocalPathSegment const& segment)
                {
                    return directory / segment;
                }
            );
    }

    /**
     * @brief The directory a drop or transfer goes into: @p directory itself without @p target, its parent for
     *        @ref parentDirectoryTarget, else the child named @p target.
     */
    template <typename PathT>
    std::expected<PathT, Paths::PathError>
    targetDirectoryOf(PathT const& directory, std::optional<std::string> const& target)
    {
        if (!target)
            return directory;
        if (*target == parentDirectoryTarget)
        {
            if (auto parent = directory.parent(); parent)
                return *std::move(parent);
            return std::unexpected(
                Paths::PathError{.reason = Paths::PathRejection::EscapesRoot, .input = std::string{*target}}
            );
        }
        return childOf(directory, *target);
    }

    /**
     * @brief The parent of the listed @p directory, nullopt for a root (like "/", "C:/" or "//server/share") and
     *        for text that is no absolute path of the side.
     */
    template <typename PathT>
    std::optional<std::string> parentOf(std::string_view directory)
    {
        const auto parsed = parsePath<PathT>(directory);
        if (!parsed)
            return std::nullopt;
        return parsed->parent().transform(
            [](PathT const& parent)
            {
                return pathText(parent);
            }
        );
    }

    /**
     * @brief The directory to list for @p path: the normalized absolute path of the side, which has no trailing
     *        separator except at a root. Text the side does not take as an absolute path is normalized lexically,
     *        without trailing separators, and the listing then reports why it fails.
     */
    template <typename PathT>
    std::string navigationTarget(std::string_view path)
    {
        if (const auto parsed = parsePath<PathT>(path); parsed)
            return pathText(*parsed);
        auto normal = std::filesystem::path{std::string{path}}.lexically_normal().generic_string();
        while (normal.size() > 1 && normal.back() == '/')
            normal.pop_back();
        return normal;
    }
}
