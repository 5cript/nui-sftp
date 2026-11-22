#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/local_file_problem.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <remote_io/remote_error.hpp>

namespace RemoteIo
{
    template <typename EnumType>
    requires boost::describe::has_describe_enumerators<EnumType>::value
    void from_json(nlohmann::json const& json, EnumType& value)
    {
        SharedData::from_json(json, value);
    }
    template <typename EnumType>
    requires boost::describe::has_describe_enumerators<EnumType>::value
    void to_json(nlohmann::json& json, EnumType const& value)
    {
        SharedData::to_json(json, value);
    }
#ifdef __EMSCRIPTEN__
    template <typename EnumType>
    requires boost::describe::has_describe_enumerators<EnumType>::value
    void from_val(Nui::val const& val, EnumType& value)
    {
        SharedData::from_val(val, value);
    }
    template <typename EnumType>
    requires boost::describe::has_describe_enumerators<EnumType>::value
    void to_val(Nui::val& val, EnumType const& value)
    {
        SharedData::to_val(val, value);
    }
#endif

    BOOST_DESCRIBE_STRUCT(RemoteError, (), (kind, protocol, protocolCode, message, wrapperError))

    inline void from_json(nlohmann::json const& json, RemoteError& error)
    {
        SharedData::from_json(json, error);
    }
    inline void to_json(nlohmann::json& json, RemoteError const& error)
    {
        SharedData::to_json(json, error);
    }
}

namespace SharedData
{
    struct OperationError
    {
        OperationErrorType type;
        std::optional<RemoteIo::RemoteError> remoteError = std::nullopt;
        std::optional<std::string> extraInfo = std::nullopt;
        /** @brief Why the local file could not be read or written, for an error on the local side. */
        std::optional<LocalFileProblem> localProblem = std::nullopt;

        std::string toString() const
        {
            const auto enumString = boost::describe::enum_to_string(type, "INVALID_ENUM_VALUE");
            if (remoteError.has_value())
            {
                if (extraInfo)
                    return fmt::format("{}: {}. {}.", enumString, remoteError->toString(), *extraInfo);
                else
                    return fmt::format("{}: {}.", enumString, remoteError->toString());
            }
            if (extraInfo)
            {
                return fmt::format("{}: {}.", enumString, *extraInfo);
            }
            return enumString;
        }
    };
    BOOST_DESCRIBE_STRUCT(OperationError, (), (type, remoteError, extraInfo, localProblem))
}