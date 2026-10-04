#pragma once

#include <nui/utility/move_detector.hpp>
#include <persistence/state/termios.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <backend/process/boost_process.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <filesystem>

namespace PTY
{
    class PseudoTerminal
    {
      public:
        struct LauncherInit
        {
          public:
            boost::system::error_code on_exec_setup(
                boost::process::v2::posix::default_launcher& launcher,
                const boost::process::v2::filesystem::path& executable,
                const char* const* cmdLine);

            boost::system::error_code on_setup(
                boost::process::v2::posix::default_launcher& launcher,
                const boost::process::v2::filesystem::path& executable,
                const char* const* cmdLine);

            void on_error(
                boost::process::v2::posix::default_launcher& launcher,
                const boost::process::v2::filesystem::path& executable,
                const char* const* cmdLine,
                const boost::system::error_code& error);

            void on_success(
                boost::process::v2::posix::default_launcher& launcher,
                const boost::process::v2::filesystem::path& executable,
                const char* const* cmdLine);

            LauncherInit(PseudoTerminal* terminal)
                : terminal_{terminal}
            {}

          private:
            PseudoTerminal* terminal_;
        };

      public:
        friend std::optional<PseudoTerminal>
        createPseudoTerminal(boost::asio::any_io_executor executor, Persistence::Termios const& termios);

        ~PseudoTerminal();
        PseudoTerminal(PseudoTerminal const&) = delete;
        PseudoTerminal(PseudoTerminal&&);
        PseudoTerminal& operator=(PseudoTerminal const&) = delete;
        PseudoTerminal& operator=(PseudoTerminal&&);

        LauncherInit makeProcessLauncherInit();

        void resize(short width, short height);

        struct PtyProcess
        {
            int pid;
            std::string cmdline;
        };
        std::vector<PtyProcess> listProcessesUnderPty();

        void
        startReading(std::function<void(std::string_view)> onStdout, std::function<void(std::string_view)> onStderr);
        void stopReading();

        bool write(std::string_view data);

      private:
        PseudoTerminal(boost::asio::any_io_executor executor);

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;

        Nui::MoveDetector moveDetector_;
    };

    std::optional<PseudoTerminal>
    createPseudoTerminal(boost::asio::any_io_executor executor, Persistence::Termios const& termios);
}