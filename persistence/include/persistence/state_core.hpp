#pragma once

#include <persistence/reference.hpp>
#include <persistence/element_missing_handler.hpp>
#include <nui/core.hpp>
#include <nlohmann/json.hpp>
#include <utility/describe.hpp>
#include <utility/enum_string_convert.hpp>
#include <shared_data/shared_data.hpp>
#include <log/log.hpp>

#include <utility>
#include <optional>

#ifdef NUI_FRONTEND
#    include <nui/frontend/event_system/observed_value.hpp>
#endif

namespace Persistence
{
#ifdef NUI_FRONTEND
    template <typename T>
    using StateWrap = Nui::Observed<T>;

    template <typename T>
    void to_json(nlohmann::json& j, StateWrap<T> const& wrap)
    {
        j = to_json(wrap.value());
    }

    template <typename T>
    void from_json(nlohmann::json const& j, StateWrap<T>& wrap)
    {
        auto proxy = wrap.modify();
        *proxy = j.get<T>();
    }

    template <typename T>
    struct IsStateWrap : std::false_type
    {};

    template <typename T>
    struct IsStateWrap<StateWrap<T>> : std::true_type
    {};
#else
    template <typename T>
    using StateWrap = T;

    template <typename T>
    struct IsStateWrap : std::false_type
    {};
#endif
    template <typename T>
    T& unwrap(StateWrap<T>& wrap)
    {
#ifdef NUI_FRONTEND
        return wrap.value();
#else
        return wrap;
#endif
    }
    template <typename T>
    T const& unwrap(StateWrap<T> const& wrap)
    {
#ifdef NUI_FRONTEND
        return wrap.value();
#else
        return wrap;
#endif
    }

    template <typename T>
    constexpr bool isStateWrap = IsStateWrap<T>::value;

    using SharedData::from_json;
    using SharedData::to_json;

    template <typename T>
    void useDefaultsFrom(std::optional<T>& toFill, std::optional<T> const& defaultsFromThis);

    template <typename T>
    void useDefaultsFrom(std::optional<T>& toFill, T const& defaultsFromThis);

    template <
        typename T,
        typename Enable = std::void_t<decltype(std::declval<T>().useDefaultsFrom(std::declval<T const&>()))>>
    void useDefaultsFrom(T& toFill, T const& defaultsFromThis);

    template <typename T>
    void useDefaultsFrom(Referenceable<T>& toFill, T const& defaultsFromThis);

    template <
        typename T,
        class Bases = boost::describe::describe_bases<T, boost::describe::mod_any_access>,
        class Members = boost::describe::describe_members<T, boost::describe::mod_any_access>,
        class Enable = std::enable_if_t<!std::is_union_v<T>>>
    void useDefaultsFrom(T& toFill, T const& defaultsFromThis);

    template <typename T>
    concept CanCallUseDefaultsFrom = requires(T t, T const& other) {
        { useDefaultsFrom(t, other) } -> std::same_as<void>;
    };

    template <typename T, class Bases, class Members, class Enable>
    void useDefaultsFrom(T& toFill, T const& defaultsFromThis)
    {
        boost::mp11::mp_for_each<Bases>(
            [&](auto&& base)
            {
                using type = typename std::decay_t<decltype(base)>::type;
                useDefaultsFrom(static_cast<type&>(toFill), static_cast<type const&>(defaultsFromThis));
            }
        );
        boost::mp11::mp_for_each<Members>(
            [&](auto&& memAccessor)
            {
                if constexpr (CanCallUseDefaultsFrom<std::decay_t<decltype(toFill.*memAccessor.pointer)>>)
                {
                    Log::debug(
                        "Calling useDefaultsFrom for member {} of type {}.",
                        memAccessor.name,
                        typeid(decltype(toFill.*memAccessor.pointer)).name()
                    );
                    useDefaultsFrom(toFill.*memAccessor.pointer, defaultsFromThis.*memAccessor.pointer);
                }
                else
                {
                    Log::debug(
                        "Skipping useDefaultsFrom for member {} of type {}.",
                        memAccessor.name,
                        typeid(decltype(toFill.*memAccessor.pointer)).name()
                    );
                }
            }
        );
    }

    template <typename T>
    void useDefaultsFrom(std::optional<T>& toFill, std::optional<T> const& defaultsFromThis)
    {
        if (!toFill && defaultsFromThis)
            toFill = *defaultsFromThis;
    }

    template <typename T>
    void useDefaultsFrom(std::optional<T>& toFill, T const& defaultsFromThis)
    {
        if (!toFill)
            toFill = defaultsFromThis;
    }

    template <typename T, typename Enable>
    void useDefaultsFrom(T& toFill, T const& defaultsFromThis)
    {
        toFill.useDefaultsFrom(defaultsFromThis);
    }

    template <typename T>
    void useDefaultsFrom(ReferenceAndImpl<T>& toFill, T const& defaultsFromThis)
    {
        useDefaultsFrom(toFill.value(), defaultsFromThis);
    }

#ifdef NUI_FRONTEND
    template <typename EnumT, typename EnumDescription = boost::describe::describe_enumerators<EnumT>>
    void to_val(Nui::val& v, EnumT const& e)
    {
        SharedData::to_val<EnumT, EnumDescription>(v, e);
    }
    template <typename EnumT, typename EnumDescription = boost::describe::describe_enumerators<EnumT>>
    void from_val(Nui::val const& v, EnumT& e)
    {
        SharedData::from_val<EnumT, EnumDescription>(v, e);
    }
#endif
}

namespace Detail
{
    template <typename T>
    struct FromJsonUnwrapped
    {
        static void fromJson(nlohmann::json const& json, T& value, char const* name)
        {
            if (auto it = json.find(name); it != json.end())
                value = it->get<typename T::value_type>();
            else
                value = std::nullopt;
        }
    };

    template <typename T>
    struct ToJsonUnwrapped
    {
        static void toJson(nlohmann::json& json, T const& value, char const* name)
        {
            if (value)
                json[name] = *value;
        }
    };

#ifdef NUI_FRONTEND
    template <typename T>
    struct FromJsonUnwrapped<Persistence::StateWrap<T>>
    {
        static void fromJson(nlohmann::json const& json, Persistence::StateWrap<T>& value, char const* name)
        {
            const auto proxy = value.modify();
            if (auto it = json.find(name); it != json.end())
                Persistence::unwrap(value) = it->get<typename T::value_type>();
        }
    };

    template <typename T>
    struct ToJsonUnwrapped<Persistence::StateWrap<T>>
    {
        static void toJson(nlohmann::json& json, Persistence::StateWrap<T> const& value, char const* name)
        {
            if (auto const& v = Persistence::unwrap(value); v)
                json[name] = *v;
        }
    };
#endif
} // namespace Detail

#define TO_JSON_OPTIONAL_RENAME(JSON, CLASS, MEMBER, JSON_NAME) \
    Detail::ToJsonUnwrapped<std::decay_t<decltype(CLASS.MEMBER)>>::toJson(JSON, CLASS.MEMBER, JSON_NAME)

#define TO_JSON_OPTIONAL(JSON, CLASS, MEMBER) TO_JSON_OPTIONAL_RENAME(JSON, CLASS, MEMBER, #MEMBER)

#define FROM_JSON_OPTIONAL_RENAME(JSON, CLASS, MEMBER, JSON_NAME) \
    Detail::FromJsonUnwrapped<std::decay_t<decltype(CLASS.MEMBER)>>::fromJson(JSON, CLASS.MEMBER, JSON_NAME)

#define FROM_JSON_OPTIONAL(JSON, CLASS, MEMBER) FROM_JSON_OPTIONAL_RENAME(JSON, CLASS, MEMBER, #MEMBER)