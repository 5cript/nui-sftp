#pragma once

#include <utility/traits_and_concepts/unique_ptr.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <optional>

namespace Persistence
{
    struct Reference
    {
        std::string ref;
        explicit Reference(std::string ref)
            : ref{std::move(ref)}
        {}
        explicit Reference(char const* ref)
            : ref{ref}
        {}
        operator std::string() const
        {
            return ref;
        }
    };
    void to_json(nlohmann::json& obj, Reference const& ref);
    void from_json(nlohmann::json const& obj, Reference& ref);

    template <typename T>
    class ReferenceAndImpl
    {
      public:
        T const& value() const
        {
            return value_;
        }
        T& value()
        {
            return value_;
        }
        std::string ref() const
        {
            return ref_->ref;
        }
        void ref(Reference ref)
        {
            ref_ = std::move(ref);
        }
        void ref(std::optional<Reference> ref)
        {
            ref_ = std::move(ref);
        }
        void ref(std::optional<std::string> ref)
        {
            if (ref)
                ref_ = Reference{std::move(*ref)};
            else
                ref_ = std::nullopt;
        }
        bool hasReference() const
        {
            return ref_.has_value();
        }
        T& operator*()
        {
            return value_;
        }
        T const& operator*() const
        {
            return value_;
        }
        T* operator->()
        {
            return &value_;
        }
        T const* operator->() const
        {
            return &value_;
        }
        template <typename MapType, typename MergerFunction>
        bool resolveWith(MapType const& map, MergerFunction const& merge)
        {
            // No need to resolve if there is no reference
            if (!hasReference())
                return true;

            auto iter = map.find(ref());
            if (iter == end(map))
                return false; // not found in map

            merge(value_, iter->second);
            return true;
        }
        template <typename MapType>
        bool resolveWith(MapType const& map)
        {
            return resolveWith(
                map,
                [](auto& value, auto const& other)
                {
                    useDefaultsFrom(value, other);
                }
            );
        }

        ReferenceAndImpl() = default;
        ReferenceAndImpl(Reference ref)
            : ref_{std::move(ref)}
            , value_{}
        {}
        ReferenceAndImpl(T t)
            : ref_{std::nullopt}
            , value_{std::move(t)}
        {}
        ReferenceAndImpl(ReferenceAndImpl const& other)
            : ref_{other.ref_}
            , value_{other.value_}
        {}
        ReferenceAndImpl(ReferenceAndImpl&& other)
            : ref_{std::exchange(other.ref_, std::nullopt)}
            , value_{std::move(other.value_)}
        {}
        ReferenceAndImpl& operator=(ReferenceAndImpl const& other)
        {
            ref_ = other.ref_;
            value_ = other.value_;
            return *this;
        }
        ReferenceAndImpl& operator=(ReferenceAndImpl&& other)
        {
            ref_ = std::exchange(other.ref_, std::nullopt);
            value_ = std::move(other.value_);
            return *this;
        }
        ReferenceAndImpl& operator=(Reference ref)
        {
            ref_ = std::move(ref);
            value_ = std::nullopt;
            return *this;
        }
        ReferenceAndImpl& operator=(T t)
        {
            ref_ = std::nullopt;
            value_ = std::move(t);
            return *this;
        }
        virtual ~ReferenceAndImpl() = default;

      protected:
        std::optional<Reference> ref_;
        T value_;
    };

    template <typename T>
    class ReferenceAndImpl<std::unique_ptr<T>>
    {
      public:
        T const& value() const
        {
            return *value_;
        }
        T& value()
        {
            return *value_;
        }
        std::string ref() const
        {
            return ref_->ref;
        }
        void ref(Reference ref)
        {
            ref_ = std::move(ref);
        }
        bool hasReference() const
        {
            return ref_.has_value();
        }
        T& operator*()
        {
            return *value_;
        }
        T const& operator*() const
        {
            return *value_;
        }
        T* operator->()
        {
            return value_.get();
        }
        T const* operator->() const
        {
            return value_.get();
        }

        ReferenceAndImpl() = default;
        ReferenceAndImpl(Reference ref)
            : ref_{std::move(ref)}
            , value_{std::make_unique<T>()}
        {}
        ReferenceAndImpl(T t)
            : ref_{std::nullopt}
            , value_{std::make_unique<T>(std::move(t))}
        {}
        ReferenceAndImpl(ReferenceAndImpl const& other)
            : ref_{other.ref_}
            , value_{std::make_unique<T>(*other.value_)}
        {}
        ReferenceAndImpl(ReferenceAndImpl&& other)
            : ref_{std::exchange(other.ref_, std::nullopt)}
            , value_{std::exchange(other.value_, nullptr)}
        {}
        ReferenceAndImpl& operator=(ReferenceAndImpl const& other)
        {
            ref_ = other.ref_;
            value_ = std::make_unique<T>(*other.value_);
            return *this;
        }
        ReferenceAndImpl& operator=(ReferenceAndImpl&& other)
        {
            ref_ = std::exchange(other.ref_, std::nullopt);
            value_ = std::exchange(other.value_, nullptr);
            return *this;
        }
        ReferenceAndImpl& operator=(Reference ref)
        {
            ref_ = std::move(ref);
            value_ = std::make_unique<T>();
            return *this;
        }
        ReferenceAndImpl& operator=(T t)
        {
            ref_ = std::nullopt;
            value_ = std::make_unique<T>(std::move(t));
            return *this;
        }
        ReferenceAndImpl& operator=(std::unique_ptr<T> t)
        {
            ref_ = std::nullopt;
            value_ = std::move(t);
            return *this;
        }
        virtual ~ReferenceAndImpl() = default;

      protected:
        std::optional<Reference> ref_;
        std::unique_ptr<T> value_;
    };

    template <typename T>
    using Referenceable = ReferenceAndImpl<T>;

    template <typename T>
    void to_json(nlohmann::json& obj, Referenceable<T> const& ref)
    {
        if (obj.is_null())
            obj = nlohmann::json::object();

        if (ref.hasReference())
            obj["$ref"] = ref.ref();

        to_json(obj, ref.value());
    }
    template <Utility::UniquePtrType T>
    void from_json(nlohmann::json const& obj, Referenceable<T>& ref)
    {
        if (obj.contains("$ref"))
        {
            ref.ref(Reference{obj["$ref"].get<std::string>()});
        }

        from_json(obj, ref.value());
    }
    template <typename T>
    requires(!Utility::IsUniquePtr_v<T>)
    void from_json(nlohmann::json const& obj, Referenceable<T>& ref)
    {
        if (obj.contains("$ref"))
        {
            ref.ref(Reference{obj["$ref"].get<std::string>()});
        }

        from_json(obj, ref.value());
    }
}