#include <ftp/protocol/features.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <algorithm>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        std::string_view trimmed(std::string_view text)
        {
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1u);
            while (!text.empty() && text.back() == ' ')
                text.remove_suffix(1u);
            return text;
        }

        std::string upperCase(std::string_view text)
        {
            return Utility::Algorithm::toUpperCase(std::string{text});
        }
    }

    std::expected<Features, RemoteIo::RemoteError> Features::fromReply(Reply const& reply)
    {
        if (reply.code == 500 || reply.code == 502)
            return Features{};
        if (reply.code != 211)
            return std::unexpected(replyError(reply, Verb::Features));

        Features features{};
        for (std::size_t index = 1u; index + 1u < reply.lines.size(); ++index)
        {
            std::string_view line = reply.lines[index];
            if (!line.starts_with(' '))
                continue;
            line = trimmed(line);
            if (line.empty())
                continue;
            const auto labelEnd = std::min(line.find(' '), line.size());
            features.features_.push_back(
                Feature{
                    .name = upperCase(line.substr(0u, labelEnd)),
                    .parameters = std::string{trimmed(line.substr(labelEnd))},
                }
            );
        }
        return features;
    }

    bool Features::has(std::string_view name) const
    {
        return parametersOf(name).has_value();
    }

    bool Features::has(std::string_view name, std::string_view parameter) const
    {
        const auto wantedName = upperCase(name);
        const auto wantedParameter = upperCase(parameter);
        return std::ranges::any_of(
            features_,
            [&](Feature const& feature)
            {
                if (feature.name != wantedName)
                    return false;
                const auto parameters = upperCase(feature.parameters);
                std::size_t start = 0u;
                while (start <= parameters.size())
                {
                    const auto end = std::min(parameters.find_first_of("; ", start), parameters.size());
                    if (std::string_view{parameters}.substr(start, end - start) == wantedParameter)
                        return true;
                    start = end + 1u;
                }
                return false;
            }
        );
    }

    std::optional<std::string_view> Features::parametersOf(std::string_view name) const
    {
        const auto wantedName = upperCase(name);
        const auto found = std::ranges::find(features_, wantedName, &Feature::name);
        if (found == features_.end())
            return std::nullopt;
        return std::string_view{found->parameters};
    }

    std::vector<Feature> const& Features::all() const
    {
        return features_;
    }
}
