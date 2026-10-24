#include <utility/shared_input_file_stream.hpp>

#ifdef _WIN32
#    include <fcntl.h>
#    include <io.h>
#    include <windows.h>
#else
#    include <sys/types.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Utility
{
    namespace
    {
#ifdef _WIN32
        struct HandleCloser
        {
            void operator()(HANDLE handle) const
            {
                ::CloseHandle(handle);
            }
        };

        /**
         * @brief A C runtime file descriptor that is closed unless it was released.
         */
        class OwnedDescriptor
        {
          public:
            explicit OwnedDescriptor(int descriptor)
                : descriptor_{descriptor}
            {}
            ~OwnedDescriptor()
            {
                if (descriptor_ != -1)
                    ::_close(descriptor_);
            }
            OwnedDescriptor(OwnedDescriptor const&) = delete;
            OwnedDescriptor& operator=(OwnedDescriptor const&) = delete;
            OwnedDescriptor(OwnedDescriptor&&) = delete;
            OwnedDescriptor& operator=(OwnedDescriptor&&) = delete;

            int get() const
            {
                return descriptor_;
            }

            int release()
            {
                return std::exchange(descriptor_, -1);
            }

          private:
            int descriptor_;
        };

        /**
         * @brief Forgets the errors noted so far, so the next failing call of the C runtime leaves only its own.
         */
        void clearSystemError()
        {
            errno = 0;
            _doserrno = 0;
        }

        /**
         * @brief The error the last failing call of the C runtime noted: the Windows code it got, if any, otherwise
         *        its errno value, and an I/O error when it noted neither.
         */
        std::error_code lastSystemError()
        {
            if (_doserrno != 0)
                return std::error_code{static_cast<int>(_doserrno), std::system_category()};
            if (errno != 0)
                return std::error_code{errno, std::generic_category()};
            return std::make_error_code(std::errc::io_error);
        }

        std::expected<std::FILE*, std::error_code> openShared(std::filesystem::path const& path)
        {
            auto handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>{::CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            )};
            if (handle.get() == INVALID_HANDLE_VALUE)
            {
                const std::error_code error{static_cast<int>(::GetLastError()), std::system_category()};
                std::ignore = handle.release();
                return std::unexpected(error);
            }
            clearSystemError();
            OwnedDescriptor descriptor{
                ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle.get()), _O_RDONLY | _O_BINARY)
            };
            if (descriptor.get() == -1)
                return std::unexpected(lastSystemError());
            // The descriptor owns the handle now.
            std::ignore = handle.release();
            clearSystemError();
            auto* file = ::_fdopen(descriptor.get(), "rb");
            if (file == nullptr)
                return std::unexpected(lastSystemError());
            std::ignore = descriptor.release();
            return file;
        }

        int seekFile(std::FILE* file, std::int64_t offset, int origin)
        {
            return ::_fseeki64(file, offset, origin);
        }

        std::int64_t tellFile(std::FILE* file)
        {
            return ::_ftelli64(file);
        }
#else
        void clearSystemError()
        {
            errno = 0;
        }

        /**
         * @brief The errno value the last failing call noted, an I/O error when it noted none.
         */
        std::error_code lastSystemError()
        {
            if (errno != 0)
                return std::error_code{errno, std::generic_category()};
            return std::make_error_code(std::errc::io_error);
        }

        std::expected<std::FILE*, std::error_code> openShared(std::filesystem::path const& path)
        {
            clearSystemError();
            auto* file = std::fopen(path.c_str(), "rb");
            if (file == nullptr)
                return std::unexpected(lastSystemError());
            return file;
        }

        int seekFile(std::FILE* file, std::int64_t offset, int origin)
        {
            return ::fseeko(file, static_cast<off_t>(offset), origin);
        }

        std::int64_t tellFile(std::FILE* file)
        {
            return static_cast<std::int64_t>(::ftello(file));
        }
#endif

        int originOf(std::ios_base::seekdir direction)
        {
            if (direction == std::ios_base::beg)
                return SEEK_SET;
            if (direction == std::ios_base::cur)
                return SEEK_CUR;
            return SEEK_END;
        }
    }

    void SharedInputFileBuffer::FileCloser::operator()(std::FILE* file) const
    {
        std::fclose(file);
    }

    bool SharedInputFileBuffer::open(std::filesystem::path const& path)
    {
        close();
        auto opened = openShared(path);
        lastError_ = opened ? std::error_code{} : opened.error();
        file_.reset(opened.value_or(nullptr));
        return isOpen();
    }

    bool SharedInputFileBuffer::isOpen() const
    {
        return file_ != nullptr;
    }

    void SharedInputFileBuffer::close()
    {
        file_.reset();
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }

    std::error_code SharedInputFileBuffer::lastError() const
    {
        return lastError_;
    }

    std::size_t SharedInputFileBuffer::readInto(char* destination, std::size_t count)
    {
        // The error indicator of the file is sticky, a later short read would take it for its own.
        std::clearerr(file_.get());
        clearSystemError();
        const auto read = std::fread(destination, 1, count, file_.get());
        if (read < count && std::ferror(file_.get()) != 0)
            lastError_ = lastSystemError();
        return read;
    }

    SharedInputFileBuffer::int_type SharedInputFileBuffer::underflow()
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        if (!file_)
            return traits_type::eof();
        const auto count = readInto(buffer_.data(), buffer_.size());
        if (count == 0)
            return traits_type::eof();
        setg(buffer_.data(), buffer_.data(), buffer_.data() + count);
        return traits_type::to_int_type(*gptr());
    }

    std::streamsize SharedInputFileBuffer::xsgetn(char_type* destination, std::streamsize count)
    {
        const auto buffered = std::min<std::streamsize>(egptr() - gptr(), count);
        if (buffered > 0)
        {
            std::memcpy(destination, gptr(), static_cast<std::size_t>(buffered));
            gbump(static_cast<int>(buffered));
        }
        if (buffered == count || !file_)
            return buffered;
        // The rest goes straight into the destination, the buffer would only copy it once more.
        return buffered +
            static_cast<std::streamsize>(readInto(destination + buffered, static_cast<std::size_t>(count - buffered)));
    }

    SharedInputFileBuffer::pos_type
    SharedInputFileBuffer::seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which)
    {
        if (!file_ || (which & std::ios_base::in) == 0)
            return pos_type(off_type(-1));
        // The file is ahead of the stream by what is buffered.
        if (direction == std::ios_base::cur)
            offset -= egptr() - gptr();
        setg(buffer_.data(), buffer_.data(), buffer_.data());
        if (seekFile(file_.get(), static_cast<std::int64_t>(offset), originOf(direction)) != 0)
            return pos_type(off_type(-1));
        return pos_type(static_cast<off_type>(tellFile(file_.get())));
    }

    SharedInputFileBuffer::pos_type SharedInputFileBuffer::seekpos(pos_type position, std::ios_base::openmode which)
    {
        return seekoff(off_type(position), std::ios_base::beg, which);
    }

    SharedInputFileStream::SharedInputFileStream()
        : std::istream{nullptr}
        , buffer_{}
    {
        rdbuf(&buffer_);
    }

    void SharedInputFileStream::open(std::filesystem::path const& path)
    {
        if (buffer_.open(path))
            clear();
        else
            setstate(std::ios_base::failbit);
    }

    bool SharedInputFileStream::is_open() const
    {
        return buffer_.isOpen();
    }

    void SharedInputFileStream::close()
    {
        buffer_.close();
    }

    std::error_code SharedInputFileStream::lastError() const
    {
        return buffer_.lastError();
    }
}
