#include <ftp/client/tls_record_trail.hpp>

#include <algorithm>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        /** @brief Content type, version and length. */
        constexpr std::size_t headerSize = 5u;
        constexpr unsigned char firstContentType = 20u;
        constexpr unsigned char lastContentType = 24u;
        constexpr unsigned char versionMajor = 3u;
        /** @brief The largest record TLS 1.2 allows, 2^14 + 2048. */
        constexpr std::size_t largestRecord = 18432u;

        std::size_t lengthOf(std::string_view header)
        {
            return (static_cast<std::size_t>(static_cast<unsigned char>(header[3])) << 8u) |
                static_cast<std::size_t>(static_cast<unsigned char>(header[4]));
        }
    }

    bool TlsRecordTrail::headerMayBeginRecord() const
    {
        if (header_.empty())
            return true;
        const auto contentType = static_cast<unsigned char>(header_[0]);
        if (contentType < firstContentType || contentType > lastContentType)
            return false;
        if (header_.size() > 1u && static_cast<unsigned char>(header_[1]) != versionMajor)
            return false;
        return header_.size() < headerSize || lengthOf(header_) <= largestRecord;
    }

    void TlsRecordTrail::feed(std::string_view bytes)
    {
        while (!bytes.empty() && !leftRecords_)
        {
            if (recordLeft_ > 0u)
            {
                const auto skipped = std::min(recordLeft_, bytes.size());
                recordLeft_ -= skipped;
                bytes.remove_prefix(skipped);
                continue;
            }
            const auto taken = std::min(headerSize - header_.size(), bytes.size());
            header_.append(bytes.substr(0u, taken));
            bytes.remove_prefix(taken);
            if (!headerMayBeginRecord())
            {
                leftRecords_ = true;
                outside_ = std::move(header_);
            }
            else if (header_.size() == headerSize)
            {
                recordLeft_ = lengthOf(header_);
                header_.clear();
            }
        }
        if (leftRecords_)
            outside_.append(bytes.substr(0u, keptBytes - std::min(keptBytes, outside_.size())));
    }

    std::string_view TlsRecordTrail::outsideRecords() const
    {
        return outside_;
    }
}
