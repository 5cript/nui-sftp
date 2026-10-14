#pragma once

#include <paths/local_path.hpp>
#include <paths/local_path_segment.hpp>
#include <paths/path_error.hpp>
#include <paths/path_segment.hpp>
#include <paths/relative_path.hpp>
#include <paths/remote_path.hpp>

#include <nlohmann/json.hpp>

#ifdef __EMSCRIPTEN__
#    include <nui/frontend/val.hpp>
#endif

#include <stdexcept>
#include <string>

namespace Paths
{
    namespace Detail
    {
        template <typename PathT>
        PathT orThrow(std::expected<PathT, PathError> result)
        {
            if (!result)
                throw std::invalid_argument{result.error().toString()};
            return std::move(result).value();
        }
    }

    // Paths travel as plain strings, invalid ones are refused when they are read.

    inline void to_json(nlohmann::json& json, RelativePath const& path)
    {
        json = path.value();
    }
    inline void from_json(nlohmann::json const& json, RelativePath& path)
    {
        path = Detail::orThrow(RelativePath::make(json.get<std::string>()));
    }

    inline void to_json(nlohmann::json& json, RemotePath const& path)
    {
        json = path.value();
    }
    inline void from_json(nlohmann::json const& json, RemotePath& path)
    {
        path = Detail::orThrow(RemotePath::make(json.get<std::string>()));
    }

    inline void to_json(nlohmann::json& json, LocalPath const& path)
    {
        json = path.toUtf8();
    }
    inline void from_json(nlohmann::json const& json, LocalPath& path)
    {
        path = Detail::orThrow(LocalPath::fromUtf8(json.get<std::string>()));
    }

#ifdef __EMSCRIPTEN__
    inline void to_val(Nui::val& value, PathSegment const& segment)
    {
        value = Nui::val::u8string(segment.value().c_str());
    }
    inline void from_val(Nui::val const& value, PathSegment& segment)
    {
        segment = Detail::orThrow(PathSegment::make(value.as<std::string>()));
    }
    inline void to_val(Nui::val& value, RelativePath const& path)
    {
        value = Nui::val::u8string(path.value().c_str());
    }
    inline void from_val(Nui::val const& value, RelativePath& path)
    {
        path = Detail::orThrow(RelativePath::make(value.as<std::string>()));
    }
    inline void to_val(Nui::val& value, RemotePath const& path)
    {
        value = Nui::val::u8string(path.value().c_str());
    }
    inline void from_val(Nui::val const& value, RemotePath& path)
    {
        path = Detail::orThrow(RemotePath::make(value.as<std::string>()));
    }
    inline void to_val(Nui::val& value, LocalPath const& path)
    {
        value = Nui::val::u8string(path.toUtf8().c_str());
    }
    inline void from_val(Nui::val const& value, LocalPath& path)
    {
        path = Detail::orThrow(LocalPath::fromUtf8(value.as<std::string>()));
    }
#endif
}

/**
 * @brief PathSegment has no unset state, so it is read through a serializer instead of from_json.
 */
template <>
struct nlohmann::adl_serializer<Paths::PathSegment>
{
    static Paths::PathSegment from_json(nlohmann::json const& json)
    {
        return Paths::Detail::orThrow(Paths::PathSegment::make(json.get<std::string>()));
    }
    static void to_json(nlohmann::json& json, Paths::PathSegment const& segment)
    {
        json = segment.value();
    }
};
