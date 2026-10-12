#pragma once

#ifndef _WIN32

#    include <backend/process/boost_process.hpp>
#    include <backend/pty/posix/pty.hpp>
#    include <persistence/state/termios.hpp>

#    include <boost/asio/io_context.hpp>
#    include <fmt/format.h>
#    include <gtest/gtest.h>

#    include <chrono>
#    include <cstddef>
#    include <filesystem>
#    include <fstream>
#    include <functional>
#    include <iterator>
#    include <optional>
#    include <string>
#    include <string_view>
#    include <thread>
#    include <vector>

extern std::filesystem::path programDirectory;

namespace Test
{
    namespace
    {
        namespace bp2 = boost::process::v2;

        /**
         * @brief Runs a sh script on a pty that stores what it reads from its stdin in a file.
         *
         * The script sleeps before it reads, so the input piles up beyond what the pty buffers.
         */
        class PseudoTerminalCapture
        {
          public:
            /**
             * @brief Starts the script, which reads @p byteCount bytes of raw input into @p file.
             * @param file Where the script stores the input.
             * @param byteCount How many bytes the script reads.
             */
            PseudoTerminalCapture(std::filesystem::path const& file, std::size_t byteCount)
            {
                pseudoTerminal_ =
                    PTY::createPseudoTerminal(context_.get_executor(), Persistence::Termios::saneDefaults());
                if (!pseudoTerminal_)
                    return;

                // Raw without IEXTEN passes printable input unchanged and without a line length limit.
                const auto script = fmt::format(
                    "stty raw -echo -iexten; echo nui-ready; sleep 1; head -c {} > '{}'; echo nui-done",
                    byteCount,
                    file.string()
                );
                bp2::posix::default_launcher launcher;
                process_.emplace(launcher(
                    context_.get_executor(),
                    bp2::filesystem::path{"/bin/sh"},
                    std::vector<std::string>{"-c", script},
                    pseudoTerminal_->makeProcessLauncherInit()
                ));

                pseudoTerminal_->startReading(
                    [this](std::string_view data)
                    {
                        output_ += data;
                    },
                    [](std::string_view) {}
                );
            }

            ~PseudoTerminalCapture()
            {
                if (!process_)
                    return;
                if (process_->running())
                {
                    boost::system::error_code terminateError;
                    process_->terminate(terminateError);
                }
            }

            PseudoTerminalCapture(PseudoTerminalCapture const&) = delete;
            PseudoTerminalCapture& operator=(PseudoTerminalCapture const&) = delete;

            bool started() const
            {
                return process_.has_value();
            }

            PTY::PseudoTerminal& pseudoTerminal()
            {
                return *pseudoTerminal_;
            }

            /**
             * @brief Pumps the pty until the script printed @p marker or the timeout passes.
             * @param marker The text to wait for.
             */
            bool waitForOutput(std::string_view marker, std::chrono::milliseconds timeout = std::chrono::seconds{20})
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (output_.find(marker) == std::string::npos)
                {
                    if (std::chrono::steady_clock::now() > deadline)
                        return false;
                    if (context_.stopped())
                        context_.restart();
                    context_.run_for(std::chrono::milliseconds{10});
                }
                return true;
            }

          private:
            boost::asio::io_context context_{};
            std::optional<PTY::PseudoTerminal> pseudoTerminal_{};
            std::optional<bp2::process> process_{};
            std::string output_{};
        };

        std::filesystem::path pseudoTerminalCaptureFile(std::string const& name)
        {
            const auto directory = programDirectory / "temp" / "pseudo_terminal";
            std::filesystem::create_directories(directory);
            const auto file = directory / name;
            std::filesystem::remove(file);
            return file;
        }

        std::string readWholeFile(std::filesystem::path const& file)
        {
            std::ifstream stream{file, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }

        /**
         * @brief A fixed length record of printable characters that names its writer and position.
         */
        std::string makeRecord(char writer, std::size_t index, std::size_t length)
        {
            auto record = fmt::format("{}{:06}", writer, index);
            record.resize(length, writer);
            return record;
        }
    }

    TEST(PseudoTerminalTests, InputLargerThanThePtyBufferArrivesCompleteWhileTheProcessIsNotReading)
    {
        std::string input{};
        for (std::size_t index = 0u; input.size() < 256u * 1024u; ++index)
            input += makeRecord('a', index, 64u);

        const auto file = pseudoTerminalCaptureFile("large_input.bin");
        PseudoTerminalCapture capture{file, input.size()};
        ASSERT_TRUE(capture.started());
        ASSERT_TRUE(capture.waitForOutput("nui-ready"));

        EXPECT_TRUE(capture.pseudoTerminal().write(input));

        ASSERT_TRUE(capture.waitForOutput("nui-done"));
        const auto received = readWholeFile(file);
        ASSERT_EQ(received.size(), input.size());
        EXPECT_TRUE(received == input) << "The input arrived reordered or altered.";
    }

    TEST(PseudoTerminalTests, ConcurrentWritesStayWholeAndInOrder)
    {
        constexpr std::size_t recordLength = 64u;
        constexpr std::size_t recordsPerWriter = 2000u;
        constexpr std::string_view writers = "ab";

        const auto file = pseudoTerminalCaptureFile("concurrent_input.bin");
        PseudoTerminalCapture capture{file, writers.size() * recordsPerWriter * recordLength};
        ASSERT_TRUE(capture.started());
        ASSERT_TRUE(capture.waitForOutput("nui-ready"));

        std::vector<std::thread> threads{};
        for (const auto writer : writers)
        {
            threads.emplace_back(
                [&capture, writer]()
                {
                    for (std::size_t index = 0u; index < recordsPerWriter; ++index)
                        capture.pseudoTerminal().write(makeRecord(writer, index, recordLength));
                }
            );
        }
        for (auto& thread : threads)
            thread.join();

        ASSERT_TRUE(capture.waitForOutput("nui-done"));
        const auto received = readWholeFile(file);
        ASSERT_EQ(received.size(), writers.size() * recordsPerWriter * recordLength);

        std::vector<std::size_t> nextIndex(writers.size(), 0u);
        for (std::size_t offset = 0u; offset < received.size(); offset += recordLength)
        {
            const auto record = received.substr(offset, recordLength);
            const auto writerPosition = writers.find(record.front());
            ASSERT_NE(writerPosition, std::string_view::npos) << "Unknown record at " << offset;
            ASSERT_EQ(record, makeRecord(record.front(), nextIndex[writerPosition], recordLength))
                << "Record at " << offset << " is torn or out of order.";
            ++nextIndex[writerPosition];
        }
    }
}

#endif
