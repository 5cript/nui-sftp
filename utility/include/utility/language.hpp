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
    Nui::
        ObservedValueCombinatorWithGenerator<std::function<std::string(std::string const&)>, Nui::Observed<std::string>>
        getObserved(Args&&... args)
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
        auto res = languageFile_.find(languageKey);
        if (res == languageFile_.end())
            return std::nullopt;

        nlohmann::json::const_iterator result = res;
        nlohmann::json::const_iterator end;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-value" // its the one element to be compared against end
        (((result, end = result->end(), result = result->find(args)) != end) && ...);
#pragma clang diagnostic pop
        if (result == end)
            return std::nullopt;
        return result->get<std::string>();
    }

    template <typename... Args>
    std::string getByLang(std::string const& languageKey, Args&&... args)
    {
        auto res = findByLang(languageKey, std::forward<Args>(args)...);
        if (!res)
        {
            auto fallback = findByLang("en_US", std::forward<Args>(args)...);
            if (!fallback)
                return fmt::format(
                    "No translation {} in {}.", fmt::join(std::vector<std::string>{args...}, "/"), languageKey
                );
            return *fallback;
        }
        return *res;
    }

  private:
    AppWideEvents* events_;
    nlohmann::json languageFile_;
    std::vector<std::string> languageKeys_{};
};

using LanguageObservedText = Nui::
    ObservedValueCombinatorWithGenerator<std::function<std::string(std::string const&)>, Nui::Observed<std::string>>;

extern std::unique_ptr<LanguageProvider> language;