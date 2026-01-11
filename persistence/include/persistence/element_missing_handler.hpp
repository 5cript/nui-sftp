#pragma once

#include <utility/describe.hpp>

#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <vector>
#include <string>
#include <type_traits>

namespace Persistence
{
    struct DefaultMissingMember
    {
        template <typename DerivedType, typename ElementT>
        void handleMissingElement(DerivedType& derived, ElementT& element, char const* name)
        {
            defaultedMissingMembers_.emplace_back(name);
            derived.*element = DerivedType{}.*element;
        }

        template <typename DerivedT, typename ElementT>
        void handleOptionalMember(DerivedT&, nlohmann::json&, ElementT const&, char const*)
        {
            // Do nothing by default:
            // json[name] = self.*element;
        }

        std::vector<std::string> const& defaultedMissingMembers() const
        {
            return defaultedMissingMembers_;
        }

        template <typename DerivedType>
        std::vector<std::string> collectMissingMembers(DerivedType& derived) const
        {
            std::vector<std::string> missingMembers;
            collectMissingMembers(missingMembers, derived, "");
            return missingMembers;
        }

        template <
            typename DerivedType,
            class Bases = boost::describe::describe_bases<DerivedType, boost::describe::mod_any_access>,
            class Members = boost::describe::describe_members<DerivedType, boost::describe::mod_any_access>>
        void collectMissingMembers(
            std::vector<std::string>& missingMembers,
            DerivedType& derived,
            std::string const& nameAccum
        ) const
        {
            boost::mp11::mp_for_each<Bases>(
                [&](auto&& base)
                {
                    using type = typename std::decay_t<decltype(base)>::type;
                    collectMissingMembers(nameAccum, missingMembers, static_cast<type&>(derived));
                }
            );

            auto formatter = [&]() -> std::function<std::string(std::string const&)>
            {
                if (nameAccum.empty())
                    return [&](std::string const& memberName)
                    {
                        return memberName;
                    };
                else
                    return [&](std::string const& memberName)
                    {
                        return fmt::format("{}.{}", nameAccum, memberName);
                    };
            }();

            auto missing = defaultedMissingMembers();
            std::transform(missing.begin(), missing.end(), std::back_inserter(missingMembers), formatter);

            boost::mp11::mp_for_each<Members>(
                [&](auto&& memAccessor)
                {
                    using memberType = std::decay_t<decltype(derived.*memAccessor.pointer)>;
                    if constexpr (std::is_base_of_v<DefaultMissingMember, memberType>)
                    {
                        (derived.*memAccessor.pointer)
                            .collectMissingMembers(
                                missingMembers,
                                derived.*memAccessor.pointer,
                                nameAccum.empty() ? memAccessor.name : fmt::format("{}.{}", nameAccum, memAccessor.name)
                            );
                    }
                }
            );
        }

      private:
        std::vector<std::string> defaultedMissingMembers_;
    };
}