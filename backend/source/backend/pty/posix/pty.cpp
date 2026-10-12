#include <backend/pty/posix/pty.hpp>

#include <log/log.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#ifdef __APPLE__
#    include <util.h>
#else
#    include <pty.h>
#    include <utmp.h>
#endif
#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace PTY
{
    namespace
    {
        /**
         * @brief Input for the pty master, written in order without blocking the caller.
         *
         * The master is non-blocking, so a write can take a part of the data or nothing while the pty
         * input buffer is full. The rest waits here until the master is writable again.
         */
        class InputQueue : public std::enable_shared_from_this<InputQueue>
        {
          public:
            /**
             * @brief Writes through a duplicate of the master, the reading stream owns the master itself.
             * @param executor Runs the waits for the master to become writable.
             * @param descriptor The duplicated master, owned by the queue from here on.
             */
            InputQueue(boost::asio::any_io_executor executor, int descriptor)
                : mutex_{}
                , descriptor_{std::move(executor), descriptor}
                , pending_{}
                , writtenOffset_{0u}
                , isWaitingForWritable_{false}
            {}

            /**
             * @brief Appends @p data behind the pending input and writes as much as the pty takes now.
             * @param data The input for the pty.
             * @return false if the pty is closed and the data was dropped.
             */
            bool enqueue(std::string_view data)
            {
                std::scoped_lock lock{mutex_};
                if (!descriptor_.is_open())
                    return false;
                pending_.append(data);
                if (!isWaitingForWritable_)
                    flush();
                return true;
            }

            /**
             * @brief Drops the pending input and closes the duplicated master.
             */
            void close()
            {
                std::scoped_lock lock{mutex_};
                closeDescriptor();
            }

          private:
            /**
             * @brief Writes pending input until it is gone or the pty takes no more. Requires mutex_.
             */
            void flush()
            {
                while (writtenOffset_ < pending_.size())
                {
                    const auto written = ::write(
                        descriptor_.native_handle(), pending_.data() + writtenOffset_, pending_.size() - writtenOffset_
                    );
                    if (written > 0)
                        writtenOffset_ += static_cast<std::size_t>(written);
                    else if (written == -1 && errno == EINTR)
                        continue;
                    else if (written == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        // Erasing the front of a large paste on every wakeup would be quadratic.
                        if (writtenOffset_ * 2u >= pending_.size())
                        {
                            pending_.erase(0, writtenOffset_);
                            writtenOffset_ = 0u;
                        }
                        waitUntilWritable();
                        return;
                    }
                    else
                    {
                        Log::warn(
                            "Dropping {} byte(s) of pty input: {}",
                            pending_.size() - writtenOffset_,
                            std::strerror(errno)
                        );
                        closeDescriptor();
                        return;
                    }
                }
                pending_.clear();
                writtenOffset_ = 0u;
            }

            /**
             * @brief Continues the flush once the master is writable. Requires mutex_.
             */
            void waitUntilWritable()
            {
                isWaitingForWritable_ = true;
                descriptor_.async_wait(
                    boost::asio::posix::stream_descriptor::wait_write,
                    [weak = weak_from_this()](boost::system::error_code const& error)
                    {
                        const auto self = weak.lock();
                        if (!self)
                            return;
                        std::scoped_lock lock{self->mutex_};
                        self->isWaitingForWritable_ = false;
                        if (error || !self->descriptor_.is_open())
                            return;
                        self->flush();
                    }
                );
            }

            /**
             * @brief Drops the pending input and closes the duplicate. Requires mutex_.
             */
            void closeDescriptor()
            {
                pending_.clear();
                writtenOffset_ = 0u;
                boost::system::error_code ignored;
                descriptor_.close(ignored);
            }

          private:
            std::mutex mutex_;
            boost::asio::posix::stream_descriptor descriptor_;
            std::string pending_;
            /**
             * @brief The front of pending_ that is already written.
             */
            std::size_t writtenOffset_;
            bool isWaitingForWritable_;
        };
    }

    struct PseudoTerminal::Implementation
    {
      public:
        boost::asio::any_io_executor executor;
        int master;
        int slave;
        std::string name;

        std::atomic_bool isReading;
        std::function<void(std::string_view)> onStdout;
        std::function<void(std::string_view)> onStderr;
        std::unique_ptr<boost::asio::posix::stream_descriptor> stream;
        std::vector<char> buffer;
        std::shared_ptr<InputQueue> input;

        void closeInput()
        {
            if (input)
                input->close();
        }

        void stopReading()
        {
            if (isReading)
            {
                // Keeping the duplicate open would keep the pty alive after the master is closed.
                closeInput();
                stream.reset();
                isReading = false;
                onStdout = {};
                onStderr = {};
                master = 0;
            }
        }

        void asyncRead()
        {
            stream->async_wait(
                boost::asio::posix::stream_descriptor::wait_read,
                [this](boost::system::error_code ec)
                {
                    if (ec)
                    {
                        stopReading();
                        return;
                    }

                    stream->async_read_some(
                        boost::asio::buffer(buffer),
                        [this](boost::system::error_code ec, std::size_t bytesRead)
                        {
                            if (ec)
                            {
                                stopReading();
                                return;
                            }

                            if (bytesRead == 0)
                            {
                                stopReading();
                                return;
                            }

                            std::string_view data{buffer.data(), bytesRead};

                            if (onStdout)
                                onStdout(data);

                            asyncRead();
                        }
                    );
                }
            );
        }

        Implementation(boost::asio::any_io_executor executor)
            : executor{std::move(executor)}
            , master{0}
            , slave{0}
            , name{}
            , isReading{false}
            , onStdout{}
            , onStderr{}
            , stream{}
            , buffer(4096)
            , input{}
        {}
    };

    boost::system::error_code PseudoTerminal::LauncherInit::on_exec_setup(
        boost::process::v2::posix::default_launcher&,
        const boost::process::v2::filesystem::path&,
        const char* const*
    )
    {
        // master is auto closed by boost::process
        if (login_tty(terminal_->impl_->slave) != 0)
        {
            return boost::system::error_code{errno, boost::system::system_category()};
        }
        return {};
    }

    boost::system::error_code PseudoTerminal::LauncherInit::on_setup(
        boost::process::v2::posix::default_launcher& launcher,
        const boost::process::v2::filesystem::path&,
        const char* const*
    )
    {
        // preserve fd for child:
        launcher.fd_whitelist.push_back(terminal_->impl_->slave);

        return {};
    }

    void PseudoTerminal::LauncherInit::on_error(
        boost::process::v2::posix::default_launcher&,
        const boost::process::v2::filesystem::path&,
        const char* const*,
        const boost::system::error_code& error
    )
    {
        Log::error("Failed to launch the process on the pty: {}", error.message());

        terminal_->impl_->closeInput();
        close(terminal_->impl_->master);
        close(terminal_->impl_->slave);
    }

    void PseudoTerminal::LauncherInit::on_success(
        boost::process::v2::posix::default_launcher&,
        const boost::process::v2::filesystem::path&,
        const char* const*
    )
    {
        close(terminal_->impl_->slave);
    }

    std::optional<PseudoTerminal>
    createPseudoTerminal(boost::asio::any_io_executor executor, Persistence::Termios const& termy)
    {
        auto term = PseudoTerminal{std::move(executor)};

        Persistence::Termios saneTty{
            .inputFlags = Persistence::Termios::InputFlags{}.saneDefaults(),
            .outputFlags = Persistence::Termios::OutputFlags{}.saneDefaults(),
            .controlFlags = Persistence::Termios::ControlFlags{}.saneDefaults(),
            .localFlags = Persistence::Termios::LocalFlags{}.saneDefaults(),
            .cc = Persistence::Termios::CC{},
            .iSpeed = 0,
            .oSpeed = 0,
        };

        termios ttyOptions{
            .c_iflag = termy.inputFlags.assemble(),
            .c_oflag = termy.outputFlags.assemble(),
            .c_cflag = termy.controlFlags.assemble(),
            .c_lflag = termy.localFlags.assemble(),
#ifdef __linux__
            .c_line = 0,
#endif
            .c_cc = {},
            .c_ispeed = termy.iSpeed ? *termy.iSpeed : 0,
            .c_ospeed = termy.oSpeed ? *termy.oSpeed : 0,
        };

        if (termy.cc)
        {
            std::vector<unsigned char> cc = termy.cc->assemble();
            for (size_t i = 0; i < cc.size(); ++i)
            {
                ttyOptions.c_cc[i] = cc[i];
            }
        }
        else
        {
            std::vector<unsigned char> cc = saneTty.cc->assemble();
            for (size_t i = 0; i < cc.size(); ++i)
            {
                ttyOptions.c_cc[i] = cc[i];
            }
        }

        winsize initialSize{
            .ws_row = 30,
            .ws_col = 80,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };

        char name[1024]{'\0'};
        const auto e = openpty(&term.impl_->master, &term.impl_->slave, &name[0], &ttyOptions, &initialSize);

        if (e < 0)
        {
            Log::error("Failed to open pty: {}", strerror(errno));
            return std::nullopt;
        }
        else
        {
            term.impl_->name = name;
            Log::info("Opened pty: {}", name);
        }

        // Writes must never block the caller, whether or not reading has started yet.
        const auto flags = fcntl(term.impl_->master, F_GETFL);
        const auto inputDescriptor = fcntl(term.impl_->master, F_DUPFD_CLOEXEC, 0);
        if (flags == -1 || fcntl(term.impl_->master, F_SETFL, flags | O_NONBLOCK) == -1 || inputDescriptor == -1)
        {
            Log::error("Failed to prepare the pty for input: {}", strerror(errno));
            if (inputDescriptor != -1)
                close(inputDescriptor);
            close(term.impl_->master);
            close(term.impl_->slave);
            term.impl_->master = 0;
            return std::nullopt;
        }
        term.impl_->input = std::make_shared<InputQueue>(term.impl_->executor, inputDescriptor);

        return std::optional<PseudoTerminal>(std::move(term));
    }

    PseudoTerminal::PseudoTerminal(boost::asio::any_io_executor executor)
        : impl_{std::make_unique<Implementation>(std::move(executor))}
    {}
    PseudoTerminal::PseudoTerminal(PseudoTerminal&&) = default;
    PseudoTerminal& PseudoTerminal::operator=(PseudoTerminal&&) = default;
    PseudoTerminal::~PseudoTerminal()
    {
        if (!moveDetector_.wasMoved())
        {
            if (impl_->isReading)
                stopReading();

            impl_->closeInput();

            if (impl_->master != 0)
                close(impl_->master);
        }
    }

    void PseudoTerminal::startReading(
        std::function<void(std::string_view)> onStdout,
        std::function<void(std::string_view)> onStderr
    )
    {
        if (impl_->master == 0)
            return;

        impl_->onStdout = onStdout;
        impl_->onStderr = onStderr;

        impl_->isReading = true;

        impl_->stream = std::make_unique<boost::asio::posix::stream_descriptor>(impl_->executor, impl_->master);

        impl_->asyncRead();
    }
    void PseudoTerminal::stopReading()
    {
        impl_->stopReading();
    }

    bool PseudoTerminal::write(std::string_view data)
    {
        if (!impl_->input)
            return false;
        return impl_->input->enqueue(data);
    }

    void PseudoTerminal::resize(short width, short height)
    {
        winsize size{
            .ws_row = static_cast<unsigned short>(height),
            .ws_col = static_cast<unsigned short>(width),
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        ioctl(impl_->master, TIOCSWINSZ, &size);
    }

    std::vector<TerminalProcess> PseudoTerminal::listProcessesUnderPty()
    {
        return listProcessesOnTerminal(impl_->name);
    }

    PseudoTerminal::LauncherInit PseudoTerminal::makeProcessLauncherInit()
    {
        return PseudoTerminal::LauncherInit{this};
    }
}