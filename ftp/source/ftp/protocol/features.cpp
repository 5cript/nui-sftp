#include <ftp/protocol/features.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <utility/algorithm/case_convert.hpp>
#include "text.hpp"

#include <algorithm>
#include <utility>

namespace FileTransferProtocol
{
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
            line = Detail::trimmedSpaces(line);
            if (line.empty())
                continue;
            const auto labelEnd = std::min(line.find(' '), line.size());
            features.features_.push_back(
                Feature{
                    .name = Utility::Algorithm::toUpperCase(std::string{line.substr(0u, labelEnd)}),
                    .parameters = std::string{Detail::trimmedSpaces(line.substr(labelEnd))},
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
        return std::ranges::any_of(
            features_,
            [&](Feature const& feature)
            {
                if (!Detail::equalIgnoringCase(feature.name, name))
                    return false;
                const std::string_view parameters{feature.parameters};
                std::size_t start = 0u;
                while (start <= parameters.size())
                {
                    const auto end = std::min(parameters.find_first_of("; ", start), parameters.size());
                    if (Detail::equalIgnoringCase(parameters.substr(start, end - start), parameter))
                        return true;
                    start = end + 1u;
                }
                return false;
            }
        );
    }

    std::optional<std::string_view> Features::parametersOf(std::string_view name) const
    {
        const auto found = std::ranges::find_if(
            features_,
            [name](Feature const& feature)
            {
                return Detail::equalIgnoringCase(feature.name, name);
            }
        );
        if (found == features_.end())
            return std::nullopt;
        return std::string_view{found->parameters};
    }

    std::vector<Feature> const& Features::all() const
    {
        return features_;
    }
}
