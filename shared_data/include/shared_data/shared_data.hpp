#pragma once

#include <nui/core.hpp>
#include <nlohmann/json.hpp>
#include <utility/describe.hpp>
#include <utility/visit_overloaded.hpp>
#include <utility/traits_and_concepts/optional.hpp>
#include <utility/traits_and_concepts/variant.hpp>
#include <utility/enum_string_convert.hpp>

#ifdef NUI_FRONTEND
#    include <nui/frontend/val.hpp>
#    include <nui/frontend/utility/val_conversion.hpp>
#endif

#ifdef NUI_BACKEND
#    include <boost/type_index.hpp>
#endif
#include <fmt/format.h>

namespace SharedData
{
    template <typename EnumT, typename EnumDescription = boost::describe::describe_enumerators<EnumT>>
    void to_json(nlohmann::json& j, EnumT const& e)
    {
        j = Utility::enumToString<EnumT>(e);
    }
    template <typename EnumT, typename EnumDescription = boost::describe::describe_enumerators<EnumT>>
    void from_json(nlohmann::json const& j, EnumT& e)
    {
        e = Utility::enumFromString<EnumT>(j.template get<std::string>());
    }

#ifdef NUI_FRONTEND
    template <typename EnumT, typename EnumDescription = boost::describe::describe_enumerators<EnumT>>
    void to_val(Nui::val& v, EnumT const& e)
    {
        v = Nui::val::u8string(Utility::enumToString<EnumT>(e).c_str());
    }
    template <typename EnumT, typename EnumDescription = boost::describe::describe_enumerators<EnumT>>
    void from_val(Nui::val const& v, EnumT& e)
    {
        e = Utility::enumFromString<EnumT>(v.template as<std::string>());
    }
#endif

    namespace Detail
    {
        template <typename ObjT, typename T>
        void from_json_impl(ObjT& obj, nlohmann::json const&, nlohmann::json const& j, T& value);

        template <typename ObjT, typename T>
        requires std::is_enum_v<T>
        void from_json_impl(ObjT& obj, nlohmann::json const&, nlohmann::json const& j, T& value);

        template <typename ObjT, typename T>
        void from_json_impl(ObjT&, nlohmann::json const&, nlohmann::json const& j, std::optional<T>& value);

        template <typename ObjT, typename... Ts>
        void from_json_impl(
            ObjT& obj,
            nlohmann::json const&,
            nlohmann::json const& j,
            std::variant<std::monostate, Ts...>& value
        );

        template <typename ObjT>
        void from_json_impl(ObjT& obj, nlohmann::json const&, nlohmann::json const& j, std::chrono::seconds& value);

        template <typename ObjT>
        void
        from_json_impl(ObjT& obj, nlohmann::json const&, nlohmann::json const& j, std::chrono::milliseconds& value);

        template <typename ObjT>
        void from_json_impl(ObjT& obj, nlohmann::json const&, nlohmann::json const& j, std::filesystem::perms& value);

        template <typename ObjT, typename T>
        void from_json_impl(ObjT&, nlohmann::json const&, nlohmann::json const& j, T& value)
        {
            j.get_to(value);
        }

        template <typename ObjT, typename T>
        requires std::is_enum_v<T>
        void from_json_impl(ObjT&, nlohmann::json const&, nlohmann::json const& j, T& value)
        {
            value = Utility::enumFromString<T>(j.get<std::string>());
        }

        template <typename ObjT>
        void from_json_impl(ObjT&, nlohmann::json const&, nlohmann::json const& j, std::chrono::seconds& value)
        {
            value = std::chrono::seconds{j.get<long long>()};
        }

        template <typename ObjT>
        void from_json_impl(ObjT&, nlohmann::json const&, nlohmann::json const& j, std::chrono::milliseconds& value)
        {
            value = std::chrono::milliseconds{j.get<long long>()};
        }

        template <typename ObjT>
        void from_json_impl(ObjT&, nlohmann::json const&, nlohmann::json const& j, std::filesystem::perms& value)
        {
            value = static_cast<std::filesystem::perms>(j.get<int>());
        }

        template <typename ObjT, typename T>
        void from_json_impl(ObjT&, nlohmann::json const&, nlohmann::json const& j, std::optional<T>& value)
        {
            if (!j.is_null())
                value = j.get<T>();
            else
                value = std::nullopt;
        }

        template <typename ObjT, typename... Ts>
        void from_json_impl(
            ObjT& obj,
            nlohmann::json const& jParent,
            nlohmann::json const& j,
            std::variant<std::monostate, Ts...>& value
        )
        {
            if (j.is_null())
            {
                value = std::monostate{};
                return;
            }

            // initalizes the variant based on the type info in ObjT
            obj.variantDecide(jParent);
            Utility::visitOverloaded(
                value,
                [&](std::monostate&)
                {
                    throw std::runtime_error("Cannot deserialize variant from monostate");
                },
                [&](auto& currentValue)
                {
                    using CurrentType = std::decay_t<decltype(currentValue)>;
                    currentValue = j.get<CurrentType>();
                }
            );
        }

        template <typename T>
        requires(!std::is_enum_v<T>)
        void to_json_impl(nlohmann::json& j, T const& obj);
        template <typename T>
        requires std::is_enum_v<T>
        void to_json_impl(nlohmann::json& j, T obj);
        template <typename T>
        void to_json_impl(nlohmann::json& j, std::optional<T> const& obj);
        template <typename... Ts>
        void to_json_impl(nlohmann::json& j, std::variant<std::monostate, Ts...> const& obj);
        void to_json_impl(nlohmann::json& j, std::chrono::seconds value);
        void to_json_impl(nlohmann::json& j, std::chrono::milliseconds value);
        void to_json_impl(nlohmann::json& j, std::filesystem::perms value);

        template <typename T>
        requires(!std::is_enum_v<T>)
        void to_json_impl(nlohmann::json& j, T const& obj)
        {
            j = obj;
        }
        template <typename T>
        requires std::is_enum_v<T>
        void to_json_impl(nlohmann::json& j, T obj)
        {
            j = Utility::enumToString<T>(obj);
        }
        inline void to_json_impl(nlohmann::json& j, std::chrono::seconds value)
        {
            j = value.count();
        }
        inline void to_json_impl(nlohmann::json& j, std::chrono::milliseconds value)
        {
            j = value.count();
        }
        inline void to_json_impl(nlohmann::json& j, std::filesystem::perms value)
        {
            j = static_cast<int>(value);
        }
        template <typename T>
        void to_json_impl(nlohmann::json& j, std::optional<T> const& obj)
        {
            if (obj)
                Detail::to_json_impl(j, *obj);
        }
        template <typename... Ts>
        void to_json_impl(nlohmann::json& j, std::variant<std::monostate, Ts...> const& obj)
        {
            Utility::visitOverloaded(
                obj,
                [&](std::monostate)
                {
                    j = nullptr;
                },
                [&](auto const& currentValue)
                {
                    Detail::to_json_impl(j, currentValue);
                }
            );
        }
    }

    template <typename T, typename ElementT>
    void invokeElementOptionalHandler(T&, nlohmann::json&, ElementT const&, char const*)
    {
        // default is do nothing: dont serialize
    }

    template <
        typename T,
        typename ElementT,
        typename Enable = std::void_t<decltype(std::declval<T>().handleOptionalMember(
            std::declval<T&>(),
            std::declval<nlohmann::json&>(),
            std::declval<ElementT const&>(),
            std::declval<char const*>()
        ))>>
    void
    invokeElementOptionalHandler(T& obj, nlohmann::json& objectJson, ElementT const& pointerToMember, char const* name)
    {
        obj.handleOptionalMember(obj, objectJson, pointerToMember, name);
    }

    template <
        typename T,
        class Bases = boost::describe::describe_bases<T, boost::describe::mod_any_access>,
        class Members = boost::describe::describe_members<T, boost::describe::mod_any_access>,
        class Enable = std::enable_if_t<!std::is_union_v<T>>>
    void to_json(nlohmann::json& j, T const& obj)
    {
        if (j.is_null())
            j = nlohmann::json::object();

        boost::mp11::mp_for_each<Bases>(
            [&](auto&& base)
            {
                using type = typename std::decay_t<decltype(base)>::type;
                to_json(j, static_cast<type const&>(obj));
            }
        );
        boost::mp11::mp_for_each<Members>(
            [&](auto&& memAccessor)
            {
                using memberType = std::decay_t<decltype(obj.*memAccessor.pointer)>;
                if constexpr (Utility::OptionalType<memberType>)
                {
                    if (obj.*memAccessor.pointer)
                        Detail::to_json_impl(j[memAccessor.name], obj.*memAccessor.pointer);
                    else
                        invokeElementOptionalHandler(obj, j, obj.*memAccessor.pointer, memAccessor.name);
                }
                else
                    Detail::to_json_impl(j[memAccessor.name], obj.*memAccessor.pointer);
            }
        );
    }

    template <typename T, typename ElementT, typename = void>
    struct CanCallHandleMissingElementOnObject : std::false_type
    {};

    template <typename T, typename ElementT>
    struct CanCallHandleMissingElementOnObject<
        T,
        ElementT,
        std::void_t<decltype(std::declval<T>()
                .handleMissingElement(std::declval<T&>(), std::declval<ElementT&>(), std::declval<char const*>()))>>
        : std::true_type
    {};

    template <typename T, typename ElementT>
    concept HasHandleMissingElementOnObject = CanCallHandleMissingElementOnObject<T, ElementT>::value;

    template <typename T, typename ElementT>
    void invokeElementMissingHandler(T&, ElementT&, char const* memberName)
    {
#ifdef NUI_BACKEND
        throw std::runtime_error(
            fmt::format(
                "Missing required field '{}' in JSON object on type '{}'.",
                memberName,
                boost::typeindex::type_id<T>().pretty_name()
            )
        );
#else
        throw std::runtime_error(fmt::format("Missing required field '{}' in JSON object.", memberName));
#endif
    }

    template <typename T, typename ElementT>
    requires(HasHandleMissingElementOnObject<T, ElementT>)
    void invokeElementMissingHandler(T& obj, ElementT& member, char const* memberName)
    {
        obj.handleMissingElement(obj, member, memberName);
    }

    template <
        typename T,
        class Bases = boost::describe::describe_bases<T, boost::describe::mod_any_access>,
        class Members = boost::describe::describe_members<T, boost::describe::mod_any_access>,
        class Enable = std::enable_if_t<!std::is_union_v<T>>>
    void from_json(nlohmann::json const& j, T& obj)
    {
        boost::mp11::mp_for_each<Bases>(
            [&](auto&& base)
            {
                using type = typename std::decay_t<decltype(base)>::type;
                from_json(j, static_cast<type&>(obj));
            }
        );
        boost::mp11::mp_for_each<Members>(
            [&](auto&& memAccessor)
            {
                using memberType = std::decay_t<decltype(obj.*memAccessor.pointer)>;
                if constexpr (Utility::OptionalType<memberType>)
                {
                    if (j.contains(memAccessor.name))
                        Detail::from_json_impl(obj, j, j.at(memAccessor.name), obj.*memAccessor.pointer);
                }
                else
                {
                    if (j.contains(memAccessor.name))
                        Detail::from_json_impl(obj, j, j.at(memAccessor.name), obj.*memAccessor.pointer);
                    else
                        invokeElementMissingHandler(obj, memAccessor.pointer, memAccessor.name);
                }
            }
        );
    }
}