#include <ssh/key_file.hpp>
#include <paths/native.hpp>

#include <libssh/libssh.h>

#include <cctype>
#include <fstream>
#include <iterator>

namespace SecureShell
{
    namespace
    {
        std::optional<std::string> readKeyFile(std::filesystem::path const& keyFile)
        {
            std::ifstream stream{keyFile, std::ios::binary};
            if (!stream)
                return std::nullopt;
            std::string content{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
            if (stream.bad())
            {
                Detail::secureWipe(content);
                return std::nullopt;
            }
            return content;
        }

        bool isSpace(char character)
        {
            return std::isspace(static_cast<unsigned char>(character)) != 0;
        }

        std::string_view nextToken(std::string_view& text)
        {
            std::size_t start = 0;
            while (start < text.size() && isSpace(text[start]))
                ++start;
            std::size_t end = start;
            while (end < text.size() && !isSpace(text[end]))
                ++end;
            const auto token = text.substr(start, end - start);
            text.remove_prefix(end);
            return token;
        }
    }

    namespace Detail
    {
        void secureWipe(std::string& target) noexcept
        {
            volatile char* volatileData = target.data();
            for (std::size_t index = 0; index < target.size(); ++index)
                volatileData[index] = '\0';
            target.clear();
        }

        std::optional<std::pair<std::string, std::string>> splitPublicKeyLine(std::string_view text)
        {
            const auto type = nextToken(text);
            const auto blob = nextToken(text);
            if (type.empty() || blob.empty() || type.starts_with("-----") ||
                ssh_key_type_from_name(std::string{type}.c_str()) == SSH_KEYTYPE_UNKNOWN)
                return std::nullopt;
            return std::pair{std::string{type}, std::string{blob}};
        }
    }

    int importPrivateKeyFile(
        std::filesystem::path const& keyFile,
        char const* passphrase,
        ssh_auth_callback askPass,
        void* userData,
        ssh_key* key
    )
    {
        auto content = readKeyFile(keyFile);
        if (!content)
            return SSH_EOF;
        const auto result = ssh_pki_import_privkey_base64(content->c_str(), passphrase, askPass, userData, key);
        Detail::secureWipe(*content);
        return result;
    }

    int importPublicKeyFile(std::filesystem::path const& keyFile, ssh_key* key)
    {
        const auto content = readKeyFile(keyFile);
        if (!content)
            return SSH_EOF;
        if (const auto line = Detail::splitPublicKeyLine(*content); line)
            return ssh_pki_import_pubkey_base64(line->second.c_str(), ssh_key_type_from_name(line->first.c_str()), key);
        return ssh_pki_import_pubkey_file(Paths::nativeToUtf8(keyFile).c_str(), key);
    }
}
