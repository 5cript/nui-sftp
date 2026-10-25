#include "libssh_client.hpp"

#include <paths/native.hpp>

#include <fmt/format.h>

#include <fcntl.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <memory>
#include <set>
#include <vector>

namespace Benchmark
{
    namespace
    {
        /** @brief Bytes kept in flight per file, like the window OpenSSH's sftp keeps by default. */
        constexpr std::size_t bytesInFlight = 8u * 1024u * 1024u;

        struct FileCloser
        {
            void operator()(sftp_file file) const noexcept
            {
                sftp_close(file);
            }
        };
        using UniqueFile = std::unique_ptr<sftp_file_struct, FileCloser>;

        /**
         * @brief The requests of one file in flight, freed when they are not waited for.
         */
        class Requests
        {
          public:
            Requests() = default;
            ~Requests()
            {
                for (auto& request : requests_)
                    sftp_aio_free(request);
            }
            Requests(Requests const&) = delete;
            Requests& operator=(Requests const&) = delete;
            Requests(Requests&&) = delete;
            Requests& operator=(Requests&&) = delete;

            void push(sftp_aio request)
            {
                requests_.push_back(request);
            }

            /** @brief The oldest request, owned by the caller now. Wait functions free it. */
            sftp_aio pop()
            {
                const auto request = requests_.front();
                requests_.pop_front();
                return request;
            }

            bool empty() const noexcept
            {
                return requests_.empty();
            }

          private:
            std::deque<sftp_aio> requests_{};
        };

        std::size_t depthFor(std::size_t requestLength)
        {
            return std::max<std::size_t>(4u, bytesInFlight / std::max<std::size_t>(requestLength, 1u));
        }

        std::string joined(std::string const& root, std::string const& relative)
        {
            return relative.empty() ? root : fmt::format("{}/{}", root, relative);
        }

        /**
         * @brief The directory part of the relative remote path @p relative, empty for a file at the root.
         */
        std::string remoteParent(std::string const& relative)
        {
            const auto slash = relative.rfind('/');
            return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
        }
    }

    LibsshClient::LibsshClient(Connection& connection)
        : connection_{connection}
    {}

    std::string LibsshClient::name() const
    {
        return "libssh sftp_aio";
    }

    std::expected<void, std::string> LibsshClient::transfer(Transfer const& transfer)
    {
        return connection_.sftp
            ->performPromise(
                [this, &transfer]()
                {
                    return transferInStrand(transfer);
                }
            )
            .get();
    }

    std::expected<void, std::string> LibsshClient::transferInStrand(Transfer const& transfer)
    {
        sftp_ = *connection_.sftp;
        auto const* const limits = sftp_limits(sftp_);
        if (limits == nullptr)
            return std::unexpected<std::string>{"sftp_limits failed"};
        readLength_ = static_cast<std::size_t>(limits->max_read_length);
        writeLength_ = static_cast<std::size_t>(limits->max_write_length);

        const auto size = transfer.workload->fileSize;
        if (transfer.workload->isSingleFile())
        {
            return transfer.direction == Direction::Download
                ? downloadFile(transfer.remotePath, transfer.localPath, size)
                : uploadFile(transfer.localPath, transfer.remotePath, size);
        }

        if (transfer.direction == Direction::Upload)
        {
            if (const auto created = createRemoteDirectories(transfer.remotePath); !created)
                return created;
        }
        std::set<std::string> createdDirectories;
        for (auto const& relative : transfer.files)
        {
            const auto remotePath = joined(transfer.remotePath, relative);
            const auto localPath = transfer.localPath / relative;
            const auto parent = remoteParent(relative);
            if (transfer.direction == Direction::Download)
            {
                std::filesystem::create_directories(localPath.parent_path());
                if (const auto downloaded = downloadFile(remotePath, localPath, size); !downloaded)
                    return downloaded;
                continue;
            }
            if (!parent.empty() && createdDirectories.insert(parent).second)
            {
                if (const auto created = createRemoteDirectories(joined(transfer.remotePath, parent)); !created)
                    return created;
            }
            if (const auto uploaded = uploadFile(localPath, remotePath, size); !uploaded)
                return uploaded;
        }
        return {};
    }

    std::expected<void, std::string>
    LibsshClient::downloadFile(std::string const& remotePath, std::filesystem::path const& localPath, std::uint64_t size)
    {
        const UniqueFile file{sftp_open(sftp_, remotePath.c_str(), O_RDONLY, 0)};
        if (!file)
            return std::unexpected{fmt::format("opening {} failed: {}", remotePath, sftp_get_error(sftp_))};
        std::ofstream local{localPath, std::ios::binary | std::ios::trunc};
        if (!local)
            return std::unexpected{fmt::format("creating {} failed", Paths::nativeToUtf8(localPath))};

        std::vector<char> buffer(readLength_);
        Requests requests;
        std::uint64_t requested = 0;
        std::uint64_t received = 0;
        const auto depth = depthFor(readLength_);
        while (received < size)
        {
            while (requested < size && requested - received < depth * readLength_)
            {
                const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(readLength_, size - requested));
                sftp_aio request{nullptr};
                if (sftp_aio_begin_read(file.get(), length, &request) < 0)
                    return std::unexpected{fmt::format("reading {} failed", remotePath)};
                requests.push(request);
                requested += length;
            }
            auto request = requests.pop();
            const auto bytes = sftp_aio_wait_read(&request, buffer.data(), buffer.size());
            if (bytes <= 0)
                return std::unexpected{fmt::format("reading {} failed at {}", remotePath, received)};
            local.write(buffer.data(), static_cast<std::streamsize>(bytes));
            received += static_cast<std::uint64_t>(bytes);
        }
        return {};
    }

    std::expected<void, std::string>
    LibsshClient::uploadFile(std::filesystem::path const& localPath, std::string const& remotePath, std::uint64_t size)
    {
        const UniqueFile file{sftp_open(sftp_, remotePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
        if (!file)
            return std::unexpected{fmt::format("creating {} failed: {}", remotePath, sftp_get_error(sftp_))};
        std::ifstream local{localPath, std::ios::binary};
        if (!local)
            return std::unexpected{fmt::format("opening {} failed", Paths::nativeToUtf8(localPath))};

        std::vector<char> buffer(writeLength_);
        Requests requests;
        std::uint64_t sent = 0;
        std::uint64_t acknowledged = 0;
        const auto depth = depthFor(writeLength_);
        while (acknowledged < size)
        {
            while (sent < size && sent - acknowledged < depth * writeLength_)
            {
                const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(writeLength_, size - sent));
                local.read(buffer.data(), static_cast<std::streamsize>(length));
                sftp_aio request{nullptr};
                if (sftp_aio_begin_write(file.get(), buffer.data(), length, &request) < 0)
                    return std::unexpected{fmt::format("writing {} failed", remotePath)};
                requests.push(request);
                sent += length;
            }
            auto request = requests.pop();
            const auto bytes = sftp_aio_wait_write(&request);
            if (bytes <= 0)
                return std::unexpected{fmt::format("writing {} failed at {}", remotePath, acknowledged)};
            acknowledged += static_cast<std::uint64_t>(bytes);
        }
        return {};
    }

    std::expected<void, std::string> LibsshClient::createRemoteDirectories(std::string const& remotePath)
    {
        if (sftp_mkdir(sftp_, remotePath.c_str(), 0755) < 0 && sftp_get_error(sftp_) != SSH_FX_FILE_ALREADY_EXISTS)
            return std::unexpected{fmt::format("creating {} failed: {}", remotePath, sftp_get_error(sftp_))};
        return {};
    }
}
