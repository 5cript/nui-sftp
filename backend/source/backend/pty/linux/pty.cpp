#include <backend/pty/linux/pty.hpp>

#include <log/log.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <pty.h>
#include <utmp.h>

#include <filesystem>
#include <atomic>
#include <algorithm>
#include <fstream>

namespace PTY
{
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

        void stopReading()
        {
            if (isReading)
            {
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
        {}
    };

    boost::system::error_code PseudoTerminal::LauncherInit::on_exec_setup(
        boost::process::v2::posix::default_launcher&,
        const std::filesystem::path&,
        const char* const*
    )
    {
        // master is auto closed by boost::process
        if (!login_tty(terminal_->impl_->slave))
        {
            return boost::system::error_code{errno, boost::system::system_category()};
        }
        return {};
    }

    boost::system::error_code PseudoTerminal::LauncherInit::on_setup(
        boost::process::v2::posix::default_launcher& launcher,
        const std::filesystem::path&,
        const char* const*
    )
    {
        // preserve fd for child:
        launcher.fd_whitelist.push_back(terminal_->impl_->slave);

        return {};
    }

    void PseudoTerminal::LauncherInit::on_error(
        boost::process::v2::posix::default_launcher&,
        const std::filesystem::path&,
        const char* const*
    )
    {
        Log::error("Failed to create pty");

        close(terminal_->impl_->master);
        close(terminal_->impl_->slave);
    }

    void PseudoTerminal::LauncherInit::on_success(
        boost::process::v2::posix::default_launcher&,
        const std::filesystem::path&,
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
            .c_line = 0,
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
        return ::write(impl_->master, data.data(), data.size()) == static_cast<ssize_t>(data.size());
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

    std::vector<PseudoTerminal::PtyProcess> PseudoTerminal::listProcessesUnderPty()
    {
        std::vector<PtyProcess> processes;
        for (auto const& entry : std::filesystem::directory_iterator("/proc"))
        {
            try
            {
                if (entry.is_directory())
                {
                    const auto id = entry.path().filename().string();
                    if (!std::all_of(
                            id.begin(),
                            id.end(),
                            [](char c)
                            {
                                return std::isdigit(c);
                            }
                        ))
                    {
                        continue;
                    }

                    const auto fdPath = entry.path() / "fd" / "0";
                    if (!std::filesystem::exists(fdPath) || !std::filesystem::is_symlink(fdPath))
                    {
                        continue;
                    }

                    std::error_code ec;
                    const auto target = std::filesystem::read_symlink(fdPath, ec);
                    if (ec)
                        continue;

                    if (target == impl_->name)
                    {
                        std::ifstream cmdlineFile{entry.path() / "cmdline"};
                        if (!cmdlineFile)
                            continue;
                        std::string content;
                        std::getline(cmdlineFile, content, '\0');

                        processes.push_back(
                            PtyProcess{
                                .pid = std::stoi(id),
                                .cmdline = content,
                            }
                        );
                    }
                }
            }
            catch (...)
            {
                // probably a perm error
                continue;
            }
        }

        std::sort(
            processes.begin(),
            processes.end(),
            [](PtyProcess const& a, PtyProcess const& b)
            {
                return a.pid < b.pid;
            }
        );
        return processes;
    }

    PseudoTerminal::LauncherInit PseudoTerminal::makeProcessLauncherInit()
    {
        return PseudoTerminal::LauncherInit{this};
    }
}