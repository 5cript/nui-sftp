#pragma once

#include <events/app_wide_events.hpp>
#include <utility/localized_message.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/event_system/listen.hpp>

#include <nlohmann/json.hpp>
#include <fmt/args.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

/**
 * @brief Formats a translation with arguments only known at runtime. A translation whose placeholders do not fit the
 * arguments comes back unformatted with the arguments appended, instead of throwing.
 */
inline std::string formatWithArguments(std::string const& pattern, std::vector<std::string> const& arguments)
{
    fmt::dynamic_format_arg_store<fmt::format_context> store{};
    for (auto const& argument : arguments)
        store.push_back(argument);
    try
    {
        return fmt::vformat(pattern, store);
    }
    catch (fmt::format_error const&)
    {
        if (arguments.empty())
            return pattern;
        return fmt::format("{}\n{}", pattern, fmt::join(arguments, "\n"));
    }
}

class LanguageProvider
{
  public:
    LanguageProvider(AppWideEvents* events, nlohmann::json languageFile)
        : events_{events}
    {
        std::unordered_map<std::string, std::string> lookupMap;
        auto extendTable =
            [&](this const auto& self, nlohmann::json const& json, std::string const& parentKey = "") -> void
        {
            for (auto const& [subkey, value] : json.items())
            {
                if (value.is_string())
                    lookupMap.insert({fmt::format("{}/{}", parentKey, subkey), value.get<std::string>()});
                else if (value.is_object())
                    self(value, fmt::format("{}/{}", parentKey, subkey));
            }
        };

        for (auto const& [key, _] : languageFile.items())
            languageKeys_.push_back(key);

        extendTable(languageFile, "");
        lookupMap_ = std::move(lookupMap);
    }

    template <typename... Args>
    auto getObserved(Args&&... args)
    {
        std::vector<std::pair<std::string, std::string>> lookupTable;
        std::transform(
            languageKeys_.begin(),
            languageKeys_.end(),
            std::back_inserter(lookupTable),
            [&](std::string const& languageKey)
            {
                return std::make_pair(languageKey, getByLang(languageKey, std::forward<Args>(args)...));
            }
        );
        return observe(events_->onLanguageChanged)
            .generate(
                std::function<std::string(std::string const&)>{
                    [lookupTable = std::move(lookupTable)](std::string const& languageKey) -> std::string
                    {
                        for (auto const& [key, value] : lookupTable)
                        {
                            if (key == languageKey)
                                return value;
                        }
                        return fmt::format("language {} does not exist", languageKey);
                    }
                }
            );
    }

    template <typename... Args>
    std::string get(Args&&... args)
    {
        return getByLang(events_->onLanguageChanged.value(), std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::optional<std::string> findByLang(std::string const& languageKey, Args&&... args)
    {
        auto iter =
            lookupMap_.find(fmt::format("/{}", fmt::join(std::initializer_list{languageKey.c_str(), args...}, "/")));
        if (iter != lookupMap_.end())
            return iter->second;
        return std::nullopt;
    }

    template <typename... Args>
    std::string getByLang(std::string const& languageKey, Args&&... args)
    {
        auto res = findByLang(languageKey, args...);
        if (!res)
        {
            auto fallback = findByLang("en_US", args...);
            if (!fallback)
                return fmt::format(
                    "No translation {} in {}.", fmt::join(std::initializer_list{args...}, "/"), languageKey
                );
            return *fallback;
        }
        return *res;
    }

    /**
     * @brief Translates a message built with Utility::localizedMessage, like the error texts of the backend. Falls
     * back to English, then to the key and its arguments. Text that is not a key, like a raw system error, has no
     * translation and comes back unchanged.
     */
    std::string translate(std::string_view message)
    {
        const auto parsed = Utility::parseLocalizedMessage(message);
        std::string path = parsed.key;
        std::replace(path.begin(), path.end(), '.', '/');

        auto translation = findPath(events_->onLanguageChanged.value(), path);
        if (!translation)
            translation = findPath("en_US", path);
        if (!translation)
        {
            if (parsed.arguments.empty())
                return parsed.key;
            return fmt::format("{}: {}", parsed.key, fmt::join(parsed.arguments, ", "));
        }
        return formatWithArguments(*translation, parsed.arguments);
    }

    auto listenToLanguageChange(std::function<void(std::string const&)> onChange) const
    {
        return Nui::smartListen(
            events_->onLanguageChanged,
            [onChange = std::move(onChange)](std::string const& newLang)
            {
                onChange(newLang);
            }
        );
    }

  private:
    std::optional<std::string> findPath(std::string const& languageKey, std::string const& path) const
    {
        auto iter = lookupMap_.find(fmt::format("/{}/{}", languageKey, path));
        if (iter != lookupMap_.end())
            return iter->second;
        return std::nullopt;
    }

  private:
    AppWideEvents* events_;
    std::unordered_map<std::string, std::string> lookupMap_;
    std::vector<std::string> languageKeys_{};
};

using LanguageObservedText = Nui::ObservedValueCombinatorWithGenerator<
    std::function<std::string(std::string const&)>,
    decltype(AppWideEvents::onLanguageChanged)>;

extern std::unique_ptr<LanguageProvider> language;