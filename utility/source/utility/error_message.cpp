#include <utility/error_message.hpp>

#include <paths/native.hpp>
#include <paths/utf8.hpp>

#include <fmt/format.h>

#include <memory>
#include <string_view>

#ifdef _WIN32
#    include <windows.h>
#endif

namespace Utility
{
    namespace
    {
#ifdef _WIN32
        struct LocalFreeDeleter
        {
            void operator()(wchar_t* buffer) const noexcept
            {
                LocalFree(buffer);
            }
        };

        std::string systemMessage(int code)
        {
            wchar_t* buffer = nullptr;
            const auto length = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                static_cast<DWORD>(code),
                0,
                reinterpret_cast<LPWSTR>(&buffer),
                0,
                nullptr
            );
            const std::unique_ptr<wchar_t, LocalFreeDeleter> owned{buffer};
            if (length == 0 || !owned)
                return fmt::format("System error {}", code);
            std::wstring_view text{owned.get(), length};
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
                text.remove_suffix(1);
            return Paths::wideToUtf8(text);
        }
#endif
    }

    std::string errorMessage(std::error_code const& error)
    {
#ifdef _WIN32
        if (error.category() == std::system_category())
            return systemMessage(error.value());
#endif
        return Paths::sanitizedUtf8(error.message());
    }

    std::string errorMessage(std::filesystem::filesystem_error const& error)
    {
        const auto message = errorMessage(error.code());
        if (error.path1().empty())
            return message;
        if (error.path2().empty())
            return fmt::format("{}: '{}'", message, Paths::nativeToUtf8(error.path1()));
        return fmt::format(
            "{}: '{}', '{}'", message, Paths::nativeToUtf8(error.path1()), Paths::nativeToUtf8(error.path2())
        );
    }

    std::string errorMessage(std::exception const& error)
    {
        if (const auto* filesystemError = dynamic_cast<std::filesystem::filesystem_error const*>(&error))
            return errorMessage(*filesystemError);
        return Paths::sanitizedUtf8(error.what());
    }
}
