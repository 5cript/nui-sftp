#pragma once

#include <paths/utf8.hpp>

#include <nlohmann/json.hpp>

#include <utility>

namespace Utility
{
    /**
     * @brief @p json with every string and object key that is not valid UTF-8 written with U+FFFD for each invalid
     *        byte, so dump() never refuses it. Valid text stays as it is.
     */
    inline nlohmann::json withValidUtf8(nlohmann::json json)
    {
        if (json.is_string())
        {
            auto const& text = json.get_ref<std::string const&>();
            if (!Paths::isValidUtf8(text))
                json = Paths::sanitizedUtf8(text);
            return json;
        }
        if (json.is_array())
        {
            for (auto& element : json)
                element = withValidUtf8(std::move(element));
            return json;
        }
        if (json.is_object())
        {
            nlohmann::json result = nlohmann::json::object();
            for (auto& [key, value] : json.items())
                result[Paths::sanitizedUtf8(key)] = withValidUtf8(std::move(value));
            return result;
        }
        return json;
    }
}
