#pragma once

#include <events/app_wide_events.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/event_system/listen.hpp>

#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <string>
#include <vector>
#include <utility>

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
    AppWideEvents* events_;
    std::unordered_map<std::string, std::string> lookupMap_;
    std::vector<std::string> languageKeys_{};
};

using LanguageObservedText = Nui::ObservedValueCombinatorWithGenerator<
    std::function<std::string(std::string const&)>,
    decltype(AppWideEvents::onLanguageChanged)>;

extern std::unique_ptr<LanguageProvider> language;