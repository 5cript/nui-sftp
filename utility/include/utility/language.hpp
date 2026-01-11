#pragma once

#include <events/app_wide_events.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>

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
        , languageFile_(std::move(languageFile))
    {
        for (auto const& [key, _] : languageFile_.items())
        {
            languageKeys_.push_back(key);
        }
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
                [lookupTable = std::move(lookupTable)](std::string const& languageKey) -> std::string
                {
                    for (auto const& [key, value] : lookupTable)
                    {
                        if (key == languageKey)
                            return value;
                    }
                    return fmt::format("language {} does not exist", languageKey);
                }
            );
    }

    template <typename... Args>
    std::string get(Args&&... args)
    {
        return getByLang(events_->onLanguageChanged.value(), std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::string getByLang(std::string const& languageKey, Args&&... args)
    {
        auto res = languageFile_.find(languageKey);
        if (res == languageFile_.end())
        {
            return fmt::format(
                "{} - language {} does not exist", fmt::join(std::vector<std::string>{args...}, "."), languageKey
            );
        }

        nlohmann::json::const_iterator result = res;
        nlohmann::json::const_iterator end;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-value" // its the one element to be compared against end
        (((result, end = result->end(), result = result->find(args)) != end) && ...);
#pragma clang diagnostic pop
        if (result == end)
            return fmt::format("{} - key does not exist", fmt::join(std::vector<std::string>{args...}, "."));
        return result->get<std::string>();
    }

  private:
    AppWideEvents* events_;
    nlohmann::json languageFile_;
    std::vector<std::string> languageKeys_{};
};

extern std::unique_ptr<LanguageProvider> language;