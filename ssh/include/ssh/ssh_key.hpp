#pragma once

#include <libssh/libssh.h>

#include <utility>

namespace SecureShell
{
    struct SshKey
    {
      public:
        SshKey() noexcept
            : key_{nullptr}
        {}
        explicit SshKey(ssh_key key) noexcept
            : key_{key}
        {}
        ~SshKey()
        {
            dispose();
        }
        explicit operator ssh_key() const noexcept
        {
            return key_;
        }
        explicit operator bool() const noexcept
        {
            return key_ != nullptr;
        }
        ssh_key underlying() const noexcept
        {
            return key_;
        }
        ssh_key& underlyingReferenced() noexcept
        {
            return key_;
        }
        SshKey(SshKey const&) = delete;
        SshKey& operator=(SshKey const&) = delete;
        SshKey(SshKey&& other) noexcept
            : key_{std::exchange(other.key_, nullptr)}
        {}
        SshKey& operator=(SshKey&& other) noexcept
        {
            if (this != &other)
            {
                dispose();
                key_ = std::exchange(other.key_, nullptr);
            }
            return *this;
        }

      private:
        void dispose() noexcept
        {
            if (key_)
                ssh_key_free(key_);
        }

      private:
        ssh_key key_;
    };
}