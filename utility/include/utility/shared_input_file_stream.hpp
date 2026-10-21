#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <istream>
#include <memory>
#include <streambuf>

namespace Utility
{
    /**
     * @brief Reads a file that others may rename, replace or delete while it is open: Windows refuses that for a file
     *        a std::filebuf holds, which opens it without FILE_SHARE_DELETE.
     */
    class SharedInputFileBuffer : public std::streambuf
    {
      public:
        SharedInputFileBuffer() = default;
        ~SharedInputFileBuffer() override = default;
        SharedInputFileBuffer(SharedInputFileBuffer const&) = delete;
        SharedInputFileBuffer& operator=(SharedInputFileBuffer const&) = delete;
        SharedInputFileBuffer(SharedInputFileBuffer&&) = delete;
        SharedInputFileBuffer& operator=(SharedInputFileBuffer&&) = delete;

        /**
         * @brief Opens @p path for reading in binary mode, closing the file open before.
         * @return Whether the file is open.
         */
        bool open(std::filesystem::path const& path);
        bool isOpen() const;
        void close();

      protected:
        int_type underflow() override;
        std::streamsize xsgetn(char_type* destination, std::streamsize count) override;
        pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override;
        pos_type seekpos(pos_type position, std::ios_base::openmode which) override;

      private:
        struct FileCloser
        {
            void operator()(std::FILE* file) const;
        };

      private:
        std::unique_ptr<std::FILE, FileCloser> file_{};
        std::array<char, 64 * 1024> buffer_{};
    };

    /**
     * @brief A std::ifstream in binary mode whose file others may rename, replace or delete while it is open, see
     *        SharedInputFileBuffer. Named like std::ifstream, so it can take its place.
     */
    class SharedInputFileStream : public std::istream
    {
      public:
        SharedInputFileStream();
        ~SharedInputFileStream() override = default;
        SharedInputFileStream(SharedInputFileStream const&) = delete;
        SharedInputFileStream& operator=(SharedInputFileStream const&) = delete;
        SharedInputFileStream(SharedInputFileStream&&) = delete;
        SharedInputFileStream& operator=(SharedInputFileStream&&) = delete;

        /**
         * @brief Opens @p path, the failbit is set when that fails.
         */
        void open(std::filesystem::path const& path);
        bool is_open() const;
        void close();

      private:
        SharedInputFileBuffer buffer_;
    };
}
