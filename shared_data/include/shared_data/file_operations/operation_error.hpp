#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <ssh/sftp_error.hpp>

namespace SecureShell
{
    BOOST_DESCRIBE_ENUM(
        WrapperErrors,
        None,
        OwnerNull,
        SharedPtrDestroyed,
        ShortWrite,
        FileNull,
        TaskPushFailed,
        BufferUnavailable,
        UnexpectedEndOfFile,
        LocalIoFailure
    )
    BOOST_DESCRIBE_STRUCT(SftpError, (), (message, sshError, sftpError, wrapperError))

    inline void from_json(nlohmann::json const& json, WrapperErrors& error)
    {
        SharedData::from_json(json, error);
    }
    inline void to_json(nlohmann::json& json, WrapperErrors const& error)
    {
        SharedData::to_json(json, error);
    }
    inline void from_json(nlohmann::json const& json, SftpError& error)
    {
        SharedData::from_json(json, error);
    }
    inline void to_json(nlohmann::json& json, SftpError const& error)
    {
        SharedData::to_json(json, error);
    }
#ifdef __EMSCRIPTEN__
    inline void from_val(Nui::val const& val, WrapperErrors& error)
    {
        SharedData::from_val(val, error);
    }
    inline void to_val(Nui::val& val, WrapperErrors const& error)
    {
        SharedData::to_val(val, error);
    }
#endif
}

namespace SharedData
{
    struct OperationError
    {
        OperationErrorType type;
        std::optional<SecureShell::SftpError> sftpError = std::nullopt;
        std::optional<std::string> extraInfo = std::nullopt;

        std::string toString() const
        {
            const auto enumString = boost::describe::enum_to_string(type, "INVALID_ENUM_VALUE");
            if (sftpError.has_value())
            {
                if (extraInfo)
                    return fmt::format("{}: {}. {}.", enumString, sftpError->toString(), *extraInfo);
                else
                    return fmt::format("{}: {}.", enumString, sftpError->toString());
            }
            if (extraInfo)
            {
                return fmt::format("{}: {}.", enumString, *extraInfo);
            }
            return enumString;
        }
    };
    BOOST_DESCRIBE_STRUCT(OperationError, (), (type, sftpError))
}