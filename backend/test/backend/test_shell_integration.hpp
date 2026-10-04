#pragma once

#ifndef _WIN32

#    include "docker_shells.hpp"
#    include "osc_scanner.hpp"

#    include <backend/process/boost_process.hpp>
#    include <backend/pty/linux/pty.hpp>
#    include <persistence/state/termios.hpp>
#    include <utility/echo_suppressor.hpp>
#    include <utility/shell_integration.hpp>

#    include <boost/asio/io_context.hpp>
#    include <fmt/format.h>
#    include <gtest/gtest.h>
#    include <nlohmann/json.hpp>

#    include <algorithm>
#    include <cctype>
#    include <chrono>
#    include <cstdlib>
#    include <filesystem>
#    include <fstream>
#    include <functional>
#    include <optional>
#    include <string>
#    include <string_view>
#    include <unordered_map>
#    include <vector>

extern char** environ;
extern std::filesystem::path programDirectory;

namespace Test
{
    namespace bp2 = boost::process::v2;

    /**
     * @brief A real shell on the backend's own pty, driven the way the app drives it.
     */
    class ShellUnderTest
    {
      public:
        using Environment = std::unordered_map<bp2::environment::key, bp2::environment::value>;

        struct Options
        {
            bp2::filesystem::path executable{};
            std::vector<std::string> arguments{};
            Environment environment{};
            short columns{200};
        };

        explicit ShellUnderTest(Options options)
        {
            pty_ = PTY::createPseudoTerminal(context_.get_executor(), Persistence::Termios::saneDefaults());
            if (!pty_)
                return;
            pty_->resize(options.columns, 30);

            bp2::posix::default_launcher launcher;
            process_.emplace(launcher(
                context_.get_executor(),
                options.executable,
                options.arguments,
                bp2::process_environment{options.environment},
                pty_->makeProcessLauncherInit()
            ));

            pty_->startReading(
                [this](std::string_view data) {
                    output_ += data;
                    chunks_.emplace_back(data);
                    scanner_.feed(data);
                    answerDeviceAttributeQueries(data);
                },
                [](std::string_view) {}
            );
        }

        ~ShellUnderTest()
        {
            if (!process_)
                return;
            write("exit\n");
            waitUntil(
                [this]() {
                    return !process_->running();
                },
                std::chrono::seconds{2}
            );
        }

        ShellUnderTest(ShellUnderTest const&) = delete;
        ShellUnderTest& operator=(ShellUnderTest const&) = delete;

        bool started() const
        {
            return process_.has_value();
        }

        void write(std::string_view data)
        {
            pty_->write(data);
        }

        /**
         * @brief Pumps the pty until @p done holds or the timeout passes.
         */
        bool waitUntil(std::function<bool()> const& done, std::chrono::milliseconds timeout = std::chrono::seconds{10})
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!done())
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                if (context_.stopped())
                    context_.restart();
                context_.run_for(std::chrono::milliseconds{10});
            }
            return true;
        }

        std::string const& output() const
        {
            return output_;
        }

        /**
         * @brief The output exactly as the pty delivered it, one entry per read.
         */
        std::vector<std::string> const& chunks() const
        {
            return chunks_;
        }

        /**
         * @brief The commands the shell hooks reported so far, decoded the way TerminalChannel does.
         */
        std::vector<std::string> reportedCommands() const
        {
            std::vector<std::string> commands;
            for (auto const& payload : scanner_.payloads())
            {
                if (auto command = ShellIntegration::commandFromOscPayload(payload))
                    commands.push_back(std::move(*command));
            }
            return commands;
        }

        /**
         * @brief Pumps the pty until no output arrived for @p quiet, e.g. after a prompt redraw.
         */
        bool waitForQuiet(std::chrono::milliseconds quiet, std::chrono::milliseconds timeout = std::chrono::seconds{5})
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            auto lastSize = output_.size();
            auto lastChange = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (context_.stopped())
                    context_.restart();
                context_.run_for(std::chrono::milliseconds{10});
                if (output_.size() != lastSize)
                {
                    lastSize = output_.size();
                    lastChange = std::chrono::steady_clock::now();
                }
                else if (std::chrono::steady_clock::now() - lastChange >= quiet)
                    return true;
            }
            return false;
        }

        /**
         * @brief Saves the raw output and the payloads this double found, for the headless xterm
         *        test to replay through the real parser (static/test/osc_handler.test.mjs).
         */
        void dumpFixture(std::string const& name) const
        {
            const auto directory = programDirectory / "temp" / "shell_integration" / "fixtures";
            // Sessions of earlier runs, possibly under names that no longer exist, would be replayed too.
            static const bool cleared = std::filesystem::remove_all(directory) >= 0;
            static_cast<void>(cleared);
            std::filesystem::create_directories(directory);
            std::ofstream{directory / (name + ".bin"), std::ios::binary} << output_;
            std::ofstream{directory / (name + ".payloads.json")}
                << nlohmann::json(scanner_.payloads()).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }

      private:
        /**
         * @brief Answers "ESC [ c" like a terminal. fish 4 asks at startup and waits for the answer;
         *        xterm.js gives it in the app, so the test pty has to as well.
         */
        void answerDeviceAttributeQueries(std::string_view data)
        {
            queryScan_ += data;
            for (auto const query : {std::string_view{"\x1b[c"}, std::string_view{"\x1b[0c"}})
            {
                for (auto position = queryScan_.find(query); position != std::string::npos;
                     position = queryScan_.find(query, position + query.size()))
                {
                    pty_->write("\x1b[?62;22c");
                }
            }
            // A query split across reads still completes; a found one cannot fit in the kept tail.
            if (queryScan_.size() > 3)
                queryScan_.erase(0, queryScan_.size() - 3);
        }

        boost::asio::io_context context_{};
        std::optional<PTY::PseudoTerminal> pty_{};
        std::optional<bp2::process> process_{};
        std::string output_{};
        std::vector<std::string> chunks_{};
        OscScanner scanner_{ShellIntegration::oscCode};
        std::string queryScan_{};
    };


    constexpr std::string_view prompt = "[nui-prompt]$ ";

    using Settings = std::vector<std::pair<std::string, std::string>>;

    /**
     * @brief Starts the shell of a test, on the host or in a docker container.
     *
     * A shell missing on the host is skipped, the docker images cover every shell. Docker itself is
     * required: without it the docker configurations fail, loudly and each on its own.
     */
    class ShellLauncher
    {
      protected:
        struct Launch
        {
            std::string name;
            std::string shell;
            std::vector<std::string> arguments;
            /// Empty runs the shell on the host.
            std::string image;
            Settings settings{};
            short columns{200};
        };

        void launch(Launch const& launch)
        {
            Settings shellSettings{
                {"TERM", "xterm-256color"},
                {"LANG", "C.UTF-8"},
                {"LC_ALL", "C.UTF-8"},
                {"PS1", std::string{prompt}},
            };
            shellSettings.insert(shellSettings.end(), launch.settings.begin(), launch.settings.end());

            if (launch.image.empty())
                launchOnHost(launch, std::move(shellSettings));
            else
                launchInContainer(launch, std::move(shellSettings));
        }

        /**
         * @brief Ends the shell, keeping its output as a fixture for the headless xterm test.
         */
        void finish()
        {
            if (shell_)
            {
                auto const* const info = ::testing::UnitTest::GetInstance()->current_test_info();
                auto name = fmt::format("{}_{}", info->test_suite_name(), info->name());
                std::ranges::replace(name, '/', '_');
                shell_->dumpFixture(name);
                shell_.reset();
            }
            if (!containerName_.empty())
                Docker::removeContainer(containerName_);
        }

        std::optional<ShellUnderTest> shell_{};

      private:
        void launchOnHost(Launch const& launch, Settings shellSettings)
        {
            const auto executable = bp2::environment::find_executable(launch.shell);
            if (executable.empty())
                GTEST_SKIP() << launch.shell << " is not installed on the host; the docker configurations cover it";

            const auto home = programDirectory / "temp" / "shell_integration" / launch.name;
            std::filesystem::remove_all(home);
            std::filesystem::create_directories(home);

            ShellUnderTest::Environment environment{
                {"PATH", std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin"},
                {"HOME", home.string()},
                {"HISTFILE", (home / "history").string()},
            };
            for (auto const& [key, value] : shellSettings)
                environment.insert_or_assign(bp2::environment::key{key}, bp2::environment::value{value});

            shell_.emplace(ShellUnderTest::Options{
                .executable = executable,
                .arguments = launch.arguments,
                .environment = std::move(environment),
                .columns = launch.columns,
            });
            ASSERT_TRUE(shell_->started());
        }

        void launchInContainer(Launch const& launch, Settings shellSettings)
        {
            if (auto const& reason = Docker::unavailableReason())
                FAIL() << "Docker is required for the shell integration tests: " << *reason;

            containerName_ = Docker::uniqueContainerName();
            std::vector<std::string> arguments{"run", "--rm", "-it", "--network", "none", "--name", containerName_};
            shellSettings.emplace_back("HISTFILE", "/tmp/nui-history");
            for (auto const& [key, value] : shellSettings)
            {
                arguments.push_back("-e");
                arguments.push_back(fmt::format("{}={}", key, value));
            }
            // The marker tells the test that the docker client attached and switched our pty to raw
            // mode. What the test writes next is typeahead in the container's pty while the shell is
            // still starting, exactly what the app's first write meets.
            arguments.push_back(launch.image);
            arguments.insert(arguments.end(), {"sh", "-c", R"(printf "nui-ready\n"; sleep 0.3; exec "$0" "$@")"});
            arguments.push_back(launch.shell);
            arguments.insert(arguments.end(), launch.arguments.begin(), launch.arguments.end());

            // The docker client needs the host's environment (DOCKER_HOST, the config in HOME, ...).
            ShellUnderTest::Environment environment;
            for (auto** variable = environ; *variable != nullptr; ++variable)
            {
                const std::string_view entry{*variable};
                const auto separator = entry.find('=');
                if (separator != std::string_view::npos)
                {
                    environment.insert_or_assign(
                        bp2::environment::key{std::string{entry.substr(0, separator)}},
                        bp2::environment::value{std::string{entry.substr(separator + 1)}}
                    );
                }
            }

            shell_.emplace(ShellUnderTest::Options{
                .executable = bp2::environment::find_executable("docker"),
                .arguments = std::move(arguments),
                .environment = std::move(environment),
                .columns = launch.columns,
            });
            ASSERT_TRUE(shell_->started());
            ASSERT_TRUE(shell_->waitUntil(
                [this]() {
                    return shell_->output().find("nui-ready") != std::string::npos;
                },
                std::chrono::seconds{30}
            )) << "the container never started:\n"
               << shell_->output();
        }

        std::string containerName_{};
    };

    /**
     * @brief "bash:5.2" -> "Bash52", for test names.
     */
    inline std::string imageLabel(std::string_view image)
    {
        std::string label;
        for (const auto character : image)
        {
            if (std::isalnum(static_cast<unsigned char>(character)))
                label += label.empty() ? static_cast<char>(std::toupper(static_cast<unsigned char>(character))) : character;
        }
        return label;
    }

    const std::vector<std::string> bashArguments{"--norc", "--noprofile", "-i"};
    const std::vector<std::string> zshArguments{"-f", "-i"};
    const std::vector<std::string> fishArguments{"--no-config", "-i"};
    const Settings historySettings{{"PROMPT_COMMAND", "true"}, {"HISTCONTROL", "ignoreboth"}};

    struct ShellConfiguration
    {
        std::string name;
        std::string shell;
        std::vector<std::string> arguments;
        /// The bootstrap as the app picks it: the local one for a known shell, the remote one for ssh.
        std::string bootstrap;
        /// Shell settings that change how the hook sees history.
        Settings settings{};
        /// Empty runs the shell on the host.
        std::string image{};
    };

    inline void PrintTo(ShellConfiguration const& configuration, std::ostream* stream)
    {
        *stream << configuration.name;
    }

    inline std::string configurationName(::testing::TestParamInfo<ShellConfiguration> const& info)
    {
        return info.param.name;
    }

    inline std::vector<ShellConfiguration> captureConfigurations()
    {
        using ShellIntegration::ShellKind;
        const auto local = [](ShellKind kind) {
            return ShellIntegration::bootstrap(kind);
        };
        const auto remote = ShellIntegration::remoteBootstrap();

        std::vector<ShellConfiguration> configurations{
            {"HostBashLocal", "bash", bashArguments, local(ShellKind::Bash)},
            {"HostBashRemote", "bash", bashArguments, remote},
            {"HostBashWithPromptCommand", "bash", bashArguments, remote, {{"PROMPT_COMMAND", "true; :"}}},
            {"HostBashIgnoreBoth", "bash", bashArguments, remote, historySettings},
            {"HostZshLocal", "zsh", zshArguments, local(ShellKind::Zsh)},
            {"HostFishLocal", "fish", fishArguments, local(ShellKind::Fish)},
        };
        for (auto const image : Docker::bashImages)
        {
            const auto label = imageLabel(image);
            configurations.push_back({label + "Remote", "bash", bashArguments, remote, {}, std::string{image}});
            configurations.push_back({label + "IgnoreBoth", "bash", bashArguments, remote, historySettings, std::string{image}});
        }
        const std::string shells{Docker::shellsImage};
        configurations.push_back({"AlpineZshLocal", "zsh", zshArguments, local(ShellKind::Zsh), {}, shells});
        configurations.push_back({"AlpineZshRemote", "zsh", zshArguments, remote, {}, shells});
        configurations.push_back({"AlpineFishLocal", "fish", fishArguments, local(ShellKind::Fish), {}, shells});
        return configurations;
    }

    inline std::vector<ShellConfiguration> bashConfigurations()
    {
        const auto remote = ShellIntegration::remoteBootstrap();
        std::vector<ShellConfiguration> configurations{
            {"HostPlain", "bash", bashArguments, remote},
            {"HostIgnoreBoth", "bash", bashArguments, remote, historySettings},
        };
        for (auto const image : Docker::bashImages)
            configurations.push_back({imageLabel(image), "bash", bashArguments, remote, {}, std::string{image}});
        return configurations;
    }

    /**
     * @brief One shell session per test. Commands are synchronized through the hook itself: a no-op
     *        written after them reports only once everything before it ran.
     */
    class ShellIntegrationCaptureTests
        : public ::testing::TestWithParam<ShellConfiguration>
        , protected ShellLauncher
    {
      protected:
        void SetUp() override
        {
            launch({
                .name = GetParam().name,
                .shell = GetParam().shell,
                .arguments = GetParam().arguments,
                .image = GetParam().image,
                .settings = GetParam().settings,
            });
        }

        void TearDown() override
        {
            finish();
        }

        void install()
        {
            shell_->write(" " + GetParam().bootstrap + "\n");
        }

        /**
         * @brief Waits until everything written so far ran, returns what was reported since the
         *        last call.
         */
        std::vector<std::string> sync()
        {
            // true takes and ignores arguments in every shell under test.
            const auto marker = fmt::format("true nui-sync-{}", ++syncCount_);
            shell_->write(marker + "\n");
            const auto arrived = shell_->waitUntil([&]() {
                const auto reported = shell_->reportedCommands();
                return std::ranges::find(reported, marker) != reported.end();
            });
            EXPECT_TRUE(arrived) << "the shell never reported " << marker << ", output tail:\n"
                                 << tail(shell_->output());

            std::vector<std::string> reported;
            const auto all = shell_->reportedCommands();
            for (auto index = consumed_; index < all.size(); ++index)
            {
                if (!all[index].starts_with("true nui-sync-"))
                    reported.push_back(all[index]);
            }
            consumed_ = all.size();
            return reported;
        }

        std::vector<std::string> commandsFor(std::string_view keystrokes)
        {
            shell_->write(keystrokes);
            return sync();
        }

        static std::string tail(std::string const& text)
        {
            return text.size() > 1500 ? text.substr(text.size() - 1500) : text;
        }

        std::size_t syncCount_{0};
        std::size_t consumed_{0};
    };

    using Commands = std::vector<std::string>;

    TEST_P(ShellIntegrationCaptureTests, BootstrapIsNeverRecorded)
    {
        // Typed before the hook exists; must not resurface as the first report either.
        shell_->write("echo prior\n");
        install();
        EXPECT_EQ(sync(), Commands{});
    }

    TEST_P(ShellIntegrationCaptureTests, TypedCommandsAreRecordedVerbatim)
    {
        install();
        ASSERT_EQ(sync(), Commands{});

        const std::vector<std::string> corpus{
            "ls",
            "pwd",
            R"(echo "hello world")",
            R"(echo 'single')",
            R"(echo "it's")",
            "true; false",
            "echo abc | grep b",
            "true && echo ok",
            R"(echo a\b)",
            R"(echo a\\b)",
            R"(echo '\x41\\x42')",
            R"(printf "%s\n" x)",
            "echo \xC3\xBCmlaut",
            "echo \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
            "echo \xF0\x9F\x98\x80",
            "echo " + std::string(4000, 'a'),
        };
        for (auto const& command : corpus)
            EXPECT_EQ(commandsFor(command + "\n"), Commands{command}) << command.substr(0, 60);
    }

    TEST_P(ShellIntegrationCaptureTests, NothingIsRecordedForLinesThatRunNothing)
    {
        install();
        ASSERT_EQ(sync(), Commands{});

        EXPECT_EQ(commandsFor("\n"), Commands{}) << "bare Enter";
        EXPECT_EQ(commandsFor("   \n"), Commands{}) << "blank line";
        // Line editors flush typeahead on Ctrl-C, so the next line waits until the redraw is done.
        shell_->write("echo never\x03");
        shell_->waitForQuiet(std::chrono::milliseconds{400});
        EXPECT_EQ(sync(), Commands{}) << "line abandoned with Ctrl-C";
        EXPECT_EQ(commandsFor("echo never\x15"), Commands{}) << "line cleared with Ctrl-U";
    }

    TEST_P(ShellIntegrationCaptureTests, CommandWithALiteralNewlineIsRecordedWhole)
    {
        install();
        ASSERT_EQ(sync(), Commands{});

        EXPECT_EQ(commandsFor("echo \"first\nsecond\"\n"), Commands{"echo \"first\nsecond\""});
    }

    INSTANTIATE_TEST_SUITE_P(
        Shells,
        ShellIntegrationCaptureTests,
        ::testing::ValuesIn(captureConfigurations()),
        configurationName
    );

    /**
     * @brief Bash specific shell constructs and history settings.
     */
    class BashCaptureTests : public ShellIntegrationCaptureTests
    {};

    TEST_P(BashCaptureTests, ShellConstructsAreRecordedAsTyped)
    {
        // Defined before the hook, so the definitions themselves are not part of the test.
        shell_->write("alias ll='ls -l'\n");
        shell_->write("greet(){ echo hi; }\n");
        install();
        ASSERT_EQ(sync(), Commands{});

        for (auto const* command :
             {"ll", "greet", "for i in 1 2; do echo $i; done", "x=1 true", "echo $((6 * 7))", "echo $(echo nested)"})
        {
            EXPECT_EQ(commandsFor(std::string{command} + "\n"), Commands{command}) << command;
        }
    }

    TEST_P(BashCaptureTests, TabCompletionRecordsNothing)
    {
        shell_->write("_nui_complete(){ COMPREPLY=(alpha beta); }\n");
        shell_->write("complete -F _nui_complete nuicommand\n");
        install();
        ASSERT_EQ(sync(), Commands{});

        EXPECT_EQ(commandsFor("nuicommand \t\t\x15"), Commands{});
    }

    TEST_P(BashCaptureTests, HeredocIsRecordedWhole)
    {
        install();
        ASSERT_EQ(sync(), Commands{});

        // bash 3.2 keeps only the first line of a heredoc in its history, which is what the hook
        // reports from.
        const auto expected = GetParam().image == "bash:3.2" ? Commands{"cat <<EOF"} : Commands{"cat <<EOF\nhello\nEOF"};
        EXPECT_EQ(commandsFor("cat <<EOF\nhello\nEOF\n"), expected);
    }

    TEST_P(BashCaptureTests, LoopDoesNotForkPerIteration)
    {
        install();
        ASSERT_EQ(sync(), Commands{});

        // The DEBUG trap fires on every iteration; a fork each time would take tens of seconds.
        const auto loop = std::string{"for ((i = 0; i < 20000; ++i)); do :; done"};
        const auto start = std::chrono::steady_clock::now();
        EXPECT_EQ(commandsFor(loop + "\n"), Commands{loop});
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
    }

    TEST_P(BashCaptureTests, PromptCommandAddedAfterTheHookSwallowsNothing)
    {
        install();
        ASSERT_EQ(sync(), Commands{});

        // Like a tool sourced later in the session; its prompt command runs after the hook's.
        commandsFor("PROMPT_COMMAND=\"${PROMPT_COMMAND:+$PROMPT_COMMAND; }nui_late=1\"\n");
        EXPECT_EQ(commandsFor("echo first\n"), Commands{"echo first"});
        EXPECT_EQ(commandsFor("echo second\n"), Commands{"echo second"});
    }

    TEST_P(BashCaptureTests, ExistingDebugTrapKeepsRunning)
    {
        // Like the trap of bash-preexec, installed before the hook.
        shell_->write("nui_fires=0; trap 'nui_fires=$((nui_fires + 1))' DEBUG\n");
        install();
        ASSERT_EQ(sync(), Commands{});

        const auto command = std::string{"nui_fires=0; true; echo fires-$nui_fires-"};
        EXPECT_EQ(commandsFor(command + "\n"), Commands{command});
        const auto& output = shell_->output();
        const auto shown = output.rfind("fires-");
        ASSERT_NE(shown, std::string::npos) << tail(output);
        EXPECT_NE(output.substr(shown, 8), "fires-0-") << tail(output);
    }

    INSTANTIATE_TEST_SUITE_P(Bash, BashCaptureTests, ::testing::ValuesIn(bashConfigurations()), configurationName);

    TEST(BashHistoryLimitationTests, RepeatIgnoredByTheShellIsNotReportedAgain)
    {
        // Documented quiet side: the hook reports from history, and ignoredups keeps a repeat out.
        const auto executable = bp2::environment::find_executable("bash");
        if (executable.empty())
            GTEST_SKIP() << "bash is not installed";

        const auto home = programDirectory / "temp" / "shell_integration" / "IgnoreDups";
        std::filesystem::create_directories(home);
        ShellUnderTest shell{{
            .executable = executable,
            .arguments = bashArguments,
            .environment =
                {
                    {"PATH", std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin"},
                    {"HOME", home.string()},
                    {"TERM", "xterm-256color"},
                    {"PS1", std::string{prompt}},
                    {"HISTFILE", (home / "history").string()},
                    {"HISTCONTROL", "ignoredups"},
                },
        }};
        ASSERT_TRUE(shell.started());
        shell.write(" " + ShellIntegration::remoteBootstrap() + "\n");
        shell.write("echo again\necho again\n: nui-done\n");
        ASSERT_TRUE(shell.waitUntil([&]() {
            return !shell.reportedCommands().empty() && shell.reportedCommands().back() == ": nui-done";
        }));
        EXPECT_EQ(shell.reportedCommands(), (Commands{"echo again", ": nui-done"}));
    }

    struct EchoConfiguration
    {
        std::string name;
        std::string shell;
        std::vector<std::string> arguments;
        std::string image;
        ShellIntegration::ShellKind kind;
        std::string bootstrap;
        /// Prints nui-marker-42 without containing it literally.
        std::string markerCommand;
        short columns;
        /// Whether the line editor echoes typed text verbatim (no syntax highlighting in between).
        bool plainEcho;
    };

    inline void PrintTo(EchoConfiguration const& configuration, std::ostream* stream)
    {
        *stream << configuration.name;
    }

    inline std::vector<EchoConfiguration> echoConfigurations()
    {
        const auto remote = ShellIntegration::remoteBootstrap();
        const std::string arithmetic = "echo nui-marker-$((6 * 7))";
        std::vector<EchoConfiguration> configurations;
        for (short const columns : {40, 80, 200})
        {
            configurations.push_back(
                {fmt::format("HostBashColumns{}", columns),
                 "bash",
                 bashArguments,
                 "",
                 ShellIntegration::ShellKind::Unknown,
                 remote,
                 arithmetic,
                 columns,
                 true}
            );
        }
        for (auto const image : Docker::bashImages)
        {
            for (short const columns : {40, 200})
            {
                configurations.push_back(
                    {fmt::format("{}Columns{}", imageLabel(image), columns),
                     "bash",
                     bashArguments,
                     std::string{image},
                     ShellIntegration::ShellKind::Unknown,
                     remote,
                     arithmetic,
                     columns,
                     true}
                );
            }
        }
        const std::string shells{Docker::shellsImage};
        for (short const columns : {40, 200})
        {
            configurations.push_back(
                {fmt::format("AlpineZshColumns{}", columns),
                 "zsh",
                 zshArguments,
                 shells,
                 ShellIntegration::ShellKind::Zsh,
                 ShellIntegration::bootstrap(ShellIntegration::ShellKind::Zsh),
                 arithmetic,
                 columns,
                 true}
            );
            configurations.push_back(
                {fmt::format("AlpineFishColumns{}", columns),
                 "fish",
                 fishArguments,
                 shells,
                 ShellIntegration::ShellKind::Fish,
                 ShellIntegration::bootstrap(ShellIntegration::ShellKind::Fish),
                 "echo nui-marker-(math 6 x 7)",
                 columns,
                 false}
            );
        }
        return configurations;
    }

    /**
     * @brief The echo filter against the echo a real shell produces, at a given terminal width.
     */
    class BootstrapEchoTests
        : public ::testing::TestWithParam<EchoConfiguration>
        , protected ShellLauncher
    {
      protected:
        void SetUp() override
        {
            launch({
                .name = GetParam().name,
                .shell = GetParam().shell,
                .arguments = GetParam().arguments,
                .image = GetParam().image,
                .columns = GetParam().columns,
            });
        }

        void TearDown() override
        {
            finish();
        }

        std::string filterChunks(std::string const& armedLine, std::vector<std::string> const& chunks) const
        {
            Utility::EchoSuppressor suppressor;
            suppressor.arm(armedLine, ShellIntegration::echoCount(GetParam().kind));
            std::string filtered;
            for (auto const& chunk : chunks)
                filtered += suppressor.filter(chunk);
            return filtered;
        }

        /**
         * @brief What a terminal would show: escape sequences removed. fish reports the running
         *        command in OSC 133 and the window title; neither is ever on screen.
         */
        static std::string visibleText(std::string const& stream)
        {
            std::string visible;
            for (std::size_t index = 0; index < stream.size(); ++index)
            {
                if (stream[index] != '\x1b' || index + 1 >= stream.size())
                {
                    visible += stream[index];
                    continue;
                }
                const auto kind = stream[++index];
                if (kind == '[')
                {
                    while (index + 1 < stream.size() && !(stream[index + 1] >= 0x40 && stream[index + 1] <= 0x7e))
                        ++index;
                    ++index;
                }
                else if (kind == ']' || kind == 'P' || kind == '_' || kind == '^' || kind == 'X')
                {
                    while (index + 1 < stream.size() && stream[index + 1] != '\x07' && stream[index + 1] != '\x1b')
                        ++index;
                    ++index;
                    if (index < stream.size() && stream[index] == '\x1b')
                        ++index;
                }
            }
            return visible;
        }

        static std::vector<std::string> rechunk(std::string const& stream, std::size_t size)
        {
            std::vector<std::string> chunks;
            for (std::size_t index = 0; index < stream.size(); index += size)
                chunks.push_back(stream.substr(index, size));
            return chunks;
        }
    };

    TEST_P(BootstrapEchoTests, EchoIsInvisibleAndEverythingElseSurvives)
    {
        // Exactly what TerminalChannel::installShellIntegration writes, right after the channel opened,
        // while the shell is still starting up.
        const auto line = " " + GetParam().bootstrap;
        shell_->write(line + "\n");
        shell_->write(GetParam().markerCommand + "\n");
        ASSERT_TRUE(shell_->waitUntil([&]() {
            return shell_->output().find("nui-marker-42\r\n") != std::string::npos;
        })) << shell_->output();
        shell_->waitForQuiet(std::chrono::milliseconds{300});

        const auto check = [&](std::string const& filtered, std::string const& how) {
            const auto visible = visibleText(filtered);
            EXPECT_EQ(visible.find("__nui"), std::string::npos) << how << ":\n" << filtered;
            EXPECT_EQ(visible.find("x1b"), std::string::npos) << how << ":\n" << filtered;
            EXPECT_EQ(visible.find("[?2004"), std::string::npos) << how << ":\n" << filtered;
            EXPECT_NE(filtered.find("nui-marker-42\r\n"), std::string::npos) << how;
            if (GetParam().plainEcho)
                EXPECT_NE(filtered.find(GetParam().markerCommand), std::string::npos) << how << ":\n" << filtered;
        };

        check(filterChunks(line, shell_->chunks()), "as the pty delivered it");
        check(filterChunks(line, {shell_->output()}), "in one piece");
        check(filterChunks(line, rechunk(shell_->output(), 1)), "byte by byte");
        check(filterChunks(line, rechunk(shell_->output(), 3)), "three bytes at a time");
    }

    INSTANTIATE_TEST_SUITE_P(
        Shells,
        BootstrapEchoTests,
        ::testing::ValuesIn(echoConfigurations()),
        [](::testing::TestParamInfo<EchoConfiguration> const& info) {
            return info.param.name;
        }
    );
}

#endif
