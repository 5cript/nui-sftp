#pragma once

#include <string>
#include <nlohmann/json.hpp>

#ifdef __EMSCRIPTEN__
#    include <nui/frontend/val.hpp>
#else
#    include <boost/uuid/uuid_generators.hpp>
#    include <boost/uuid/uuid.hpp>
#    include <boost/uuid/uuid_io.hpp>
#endif

namespace Ids
{
    class Id
    {
      public:
        friend Id generateId();

        Id(Id const&) = default;
        Id(Id&&) = default;
        Id& operator=(Id const&) = default;
        Id& operator=(Id&&) = default;
        ~Id() = default;

        std::string const& id() const
        {
            return id_;
        }

        auto operator*() const
        {
            return id_;
        }

        std::string value() const
        {
            return id_;
        }

        friend std::strong_ordering operator<=>(Id const& lhs, Id const& rhs) = default;

        bool isValid() const
        {
            return id_ != "INVALID_ID";
        }

        void invalidate()
        {
            id_ = "INVALID_ID";
        }

      protected:
        Id() = delete;
        explicit Id(std::string const& id)
            : id_{id}
        {}

      private:
        std::string id_;
    };

    struct IdHash
    {
        template <typename T>
        std::size_t operator()(T const& id) const
        {
            return std::hash<std::string>{}(id.value());
        }
    };

#ifdef __EMSCRIPTEN__
    inline Id generateId()
    {
        return Id{Nui::val::global("generateId")().as<std::string>()};
    }
#else
    inline Id generateId()
    {
        std::stringstream sstr;
        sstr << boost::uuids::random_generator()();
        return Id{sstr.str()};
    }
#endif
}

#ifdef __EMSCRIPTEN__
#    define VAL_CONVERTER(name) \
        inline void to_val(Nui::val& v, name const& id) \
        { \
            v = Nui::val::u8string(id.value().c_str()); \
        } \
        inline void from_val(Nui::val const& v, name& id) \
        { \
            id = make##name(v.template as<std::string>()); \
        }
#else
#    define VAL_CONVERTER(name)
#endif

#define DEFINE_ID_TYPE(name) \
    namespace Ids \
    { \
        class name : public Id \
        { \
          public: \
            friend name generate##name(); \
            friend name make##name(std::string const&); \
\
          public: \
            name() \
                : Id{"INVALID_ID"} \
            {} \
            name(Id id) \
                : Id{std::move(id)} \
            {} \
\
          private: \
            name(std::string const& str) \
                : Id{str} \
            {} \
        }; \
\
        inline name generate##name() \
        { \
            return name{generateId().value()}; \
        } \
\
        inline name make##name(std::string const& str) \
        { \
            return name{str}; \
        } \
        inline void to_json(nlohmann::json& j, name const& id) \
        { \
            j = id.value(); \
        } \
        inline void from_json(nlohmann::json const& j, name& id) \
        { \
            id = make##name(j.get<std::string>()); \
        } \
        VAL_CONVERTER(name) \
    }