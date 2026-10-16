#pragma once

#include <libssh/libssh.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace SecureShell
{
    namespace Detail
    {
        /**
         * @brief Overwrites @p target with zero bytes through a volatile pointer and clears it, so sensitive data
         *        does not linger in process memory.
         */
        void secureWipe(std::string& target) noexcept;

        /**
         * @brief The key type name and base64 blob of an OpenSSH public key line "type base64 [comment]", nothing
         *        for any other text.
         */
        std::optional<std::pair<std::string, std::string>> splitPublicKeyLine(std::string_view text);
    }

    /**
     * @brief Imports the private key file @p keyFile, read by its native path so any name works on Windows. Returns
     *        what ssh_pki_import_privkey_file would: SSH_EOF when the file cannot be read, SSH_ERROR when it holds no
     *        valid key or the passphrase is wrong.
     */
    int importPrivateKeyFile(
        std::filesystem::path const& keyFile,
        char const* passphrase,
        ssh_auth_callback askPass,
        void* userData,
        ssh_key* key
    );

    /**
     * @brief Imports the public key file @p keyFile, read by its native path. An OpenSSH public key line is imported
     *        from memory, any other format (like PEM) through libssh by the UTF-8 path. SSH_EOF when the file cannot
     *        be read.
     */
    int importPublicKeyFile(std::filesystem::path const& keyFile, ssh_key* key);
}
