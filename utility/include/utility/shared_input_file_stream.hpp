#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <istream>
#include <memory>
#include <streambuf>
#include <system_error>

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

        /**
         * @brief Why the last open or read failed, kept until the next open, empty when none failed. On Windows the
         *        system's own code comes in std::system_category(), an error of the C runtime as its errno value in
         *        std::generic_category(). Elsewhere it is always errno in std::generic_category().
         */
        std::error_code lastError() const;

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

        /**
         * @brief Reads up to @p count bytes into @p destination, noting why when that fails.
         * @return How many bytes were read.
         */
        std::size_t readInto(char* destination, std::size_t count);

      private:
        std::unique_ptr<std::FILE, FileCloser> file_{};
        std::array<char, 64 * 1024> buffer_{};
        std::error_code lastError_{};
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

        /**
         * @brief Why the last open or read failed, see SharedInputFileBuffer::lastError.
         */
        std::error_code lastError() const;

      private:
        SharedInputFileBuffer buffer_;
    };
}
