#include <ftp/protocol/features.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <ftp/protocol/reply_error.hpp>
#include <utility/algorithm/case_convert.hpp>
#include "text.hpp"

#include <algorithm>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        /**
         * @brief The parameters of a feature line, split at ';' and spaces, without empty ones.
         */
        std::vector<std::string_view> parameterList(std::string_view parameters)
        {
            std::vector<std::string_view> list{};
            std::size_t start = 0u;
            while (start < parameters.size())
            {
                const auto end = std::min(parameters.find_first_of("; ", start), parameters.size());
                if (end > start)
                    list.push_back(parameters.substr(start, end - start));
                start = end + 1u;
            }
            return list;
        }

        /**
         * @brief An error reply that leaves the connection open, the commands probe for themselves, RFC 2389 3.2.
         */
        bool isRefusal(Reply const& reply)
        {
            const auto replyClass = classifyReply(reply.code);
            return reply.code != 421 &&
                (replyClass == ReplyClass::TransientNegative || replyClass == ReplyClass::PermanentNegative);
        }
    }

    std::expected<Features, RemoteIo::RemoteError> Features::fromReply(Reply const& reply)
    {
        if (isRefusal(reply))
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
                return Detail::equalIgnoringCase(feature.name, name) &&
                    std::ranges::any_of(
                        parameterList(feature.parameters),
                        [parameter](std::string_view listed)
                        {
                            return Detail::equalIgnoringCase(listed, parameter);
                        }
                    );
            }
        );
    }

    std::optional<FactState> Features::machineListFact(std::string_view fact) const
    {
        const auto parameters = parametersOf("MLST");
        if (!parameters)
            return std::nullopt;
        for (const auto listed : parameterList(*parameters))
        {
            const auto enabled = listed.ends_with('*');
            if (Detail::equalIgnoringCase(enabled ? listed.substr(0u, listed.size() - 1u) : listed, fact))
                return enabled ? FactState::Enabled : FactState::Offered;
        }
        return std::nullopt;
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
