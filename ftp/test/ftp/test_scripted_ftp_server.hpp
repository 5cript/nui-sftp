#pragma once

#include <ftp/support/scripted_ftp_server.hpp>
#include <ftp/support/test_ftp_client.hpp>
#include <ftp/support/test_pki.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol::Test
{
    class ScriptedFtpServerTests : public ::testing::Test
    {
      protected:
        void startServer(ScriptedFtpServer::Options options = {})
        {
            auto started = ScriptedFtpServer::start(std::move(options));
            ASSERT_TRUE(started.has_value()) << started.error();
            server_ = std::move(started).value();
        }

        void connectClient()
        {
            const auto connected = client_.connect(server_->address(), server_->port());
            ASSERT_TRUE(connected.has_value()) << connected.error();
        }

        void expectReply(std::string_view command, int code)
        {
            const auto reply = client_.command(command);
            ASSERT_TRUE(reply.has_value()) << reply.error();
            EXPECT_EQ(reply->code, code) << reply->text;
        }

        /**
         * @brief Checks the scripts after the client left.
         */
        void expectNoFailures()
        {
            EXPECT_TRUE(server_->waitUntilFinished());
            EXPECT_TRUE(server_->waitUntilDisconnected());
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
        }

        bool transcriptHas(TranscriptEntry::Kind kind, std::string_view detail = {}) const
        {
            return std::ranges::any_of(
                server_->transcript(),
                [&](TranscriptEntry const& entry)
                {
                    return entry.kind == kind && (detail.empty() || entry.detail == detail);
                }
            );
        }

      protected:
        std::unique_ptr<ScriptedFtpServer> server_{};
        TestFtpClient client_{};
    };

    TEST_F(ScriptedFtpServerTests, LoginScriptRuns)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login("alice", "secret").expect("QUIT").reply("221 Goodbye").closeControl();
        server_->addConnection(std::move(script));

        connectClient();
        const auto greeting = client_.readReply();
        ASSERT_TRUE(greeting.has_value()) << greeting.error();
        EXPECT_EQ(greeting->code, 220);
        expectReply("USER alice", 331);
        expectReply("PASS secret", 230);
        expectReply("QUIT", 221);

        expectNoFailures();
        EXPECT_EQ(server_->commands(), (std::vector<std::string>{"USER alice", "PASS secret", "QUIT"}));
    }

    TEST_F(ScriptedFtpServerTests, ScriptsRunInTheOrderOfTheConnections)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script first{};
        first.reply("220 First").expectDisconnect();
        Script second{};
        second.reply("220 Second").expectDisconnect();
        server_->addConnection(std::move(first));
        server_->addConnection(std::move(second));

        TestFtpClient other{};
        connectClient();
        const auto firstGreeting = client_.readReply();
        ASSERT_TRUE(firstGreeting.has_value()) << firstGreeting.error();
        ASSERT_TRUE(other.connect(server_->address(), server_->port()).has_value());
        const auto secondGreeting = other.readReply();
        ASSERT_TRUE(secondGreeting.has_value()) << secondGreeting.error();

        EXPECT_EQ(firstGreeting->text, "220 First");
        EXPECT_EQ(secondGreeting->text, "220 Second");
        client_.close();
        other.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, AnUnexpectedCommandFailsWithADiff)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login().expect("RETR", ".*\\.txt").reply("150 Here it comes");
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        expectReply("USER user", 331);
        expectReply("PASS password", 230);
        ASSERT_TRUE(client_.send("STOR upload.bin").has_value());

        EXPECT_TRUE(server_->waitUntilFinished());
        const auto failures = server_->failures();
        ASSERT_EQ(failures.size(), 1u);
        EXPECT_NE(failures.front().find("  = PASS password\n"), std::string::npos) << failures.front();
        EXPECT_NE(failures.front().find("  - command RETR .*\\.txt\n"), std::string::npos) << failures.front();
        EXPECT_NE(failures.front().find("  + command STOR upload.bin"), std::string::npos) << failures.front();
    }

    TEST_F(ScriptedFtpServerTests, AnArgumentMustMatchAsAWhole)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").expect("CWD", "/home");
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        ASSERT_TRUE(client_.send("CWD /home/user").has_value());

        EXPECT_TRUE(server_->waitUntilFinished());
        EXPECT_EQ(server_->failures().size(), 1u);
    }

    TEST_F(ScriptedFtpServerTests, ACommandBehindTheEndOfTheScriptFails)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").checkpoint("greeted");
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        EXPECT_TRUE(server_->waitForCheckpoint("greeted"));
        ASSERT_TRUE(client_.send("NOOP").has_value());
        client_.close();

        EXPECT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->failures().size(), 1u);
    }

    TEST_F(ScriptedFtpServerTests, AConnectionWithoutAScriptFails)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        connectClient();
        const auto reply = client_.readReply();
        EXPECT_FALSE(reply.has_value());
        EXPECT_EQ(server_->failures().size(), 1u);
    }

    TEST_F(ScriptedFtpServerTests, CheckpointsAreTakenOnce)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.checkpoint("reached").reply("220 Ready").expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        EXPECT_TRUE(server_->waitForCheckpoint("reached"));
        EXPECT_FALSE(server_->waitForCheckpoint("reached", std::chrono::milliseconds{0}));
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, AReleaseLetsTheScriptGoOn)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").awaitRelease("go").reply("200 Released").expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        const auto greeting = client_.readReply();
        ASSERT_TRUE(greeting.has_value()) << greeting.error();
        server_->release("go");
        const auto released = client_.readReply();
        ASSERT_TRUE(released.has_value()) << released.error();
        EXPECT_EQ(released->code, 200);
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, AServerStoppedWhileAScriptAwaitsAReleaseEndsCleanly)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").checkpoint("waiting").awaitRelease("never").expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(server_->waitForCheckpoint("waiting"));
        // The steps run on without a suspension, so the script waits for the release once the server thread ended.
        server_.reset();
        client_.close();
    }

    TEST_F(ScriptedFtpServerTests, RawBytesAndDribbledRepliesArriveAsSent)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.replyRaw("220-First\r\n").dribble("220 Second\r\n").expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        const auto reply = client_.readReply();
        ASSERT_TRUE(reply.has_value()) << reply.error();
        EXPECT_EQ(reply->text, "220-First\n220 Second");
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, RepliesEndEveryLineInCrLf)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220-First\n220 Second").expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        const auto reply = client_.readReply();
        ASSERT_TRUE(reply.has_value()) << reply.error();
        EXPECT_EQ(reply->text, "220-First\n220 Second");
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, ClosingTheControlConnectionEndsTheStream)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").closeControl();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        const auto reply = client_.readReply();
        ASSERT_FALSE(reply.has_value());
        EXPECT_NE(reply.error().find("End of file"), std::string::npos) << reply.error();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, ResettingTheControlConnectionIsNoCleanEnd)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").expect("NOOP").resetControl();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        const auto reply = client_.command("NOOP");
        ASSERT_FALSE(reply.has_value());
        EXPECT_EQ(reply.error().find("End of file"), std::string::npos) << reply.error();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, AStalledScriptNeverAnswersButRecordsCommands)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").stall();
        server_->addConnection(std::move(script));

        TestFtpClient impatient{std::chrono::milliseconds{50}};
        ASSERT_TRUE(impatient.connect(server_->address(), server_->port()).has_value());
        ASSERT_TRUE(impatient.readReply().has_value());
        EXPECT_FALSE(impatient.command("NOOP").has_value());
        impatient.close();
        expectNoFailures();
        EXPECT_EQ(server_->commands(), (std::vector<std::string>{"NOOP"}));
    }

    TEST_F(ScriptedFtpServerTests, AStepBehindAStallFailsTheScript)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").stall().reply("200 Never sent");
        server_->addConnection(std::move(script));

        TestFtpClient impatient{std::chrono::milliseconds{50}};
        ASSERT_TRUE(impatient.connect(server_->address(), server_->port()).has_value());
        ASSERT_TRUE(impatient.readReply().has_value());
        EXPECT_TRUE(server_->waitUntilFinished());
        EXPECT_FALSE(impatient.readReply().has_value());
        impatient.close();
        EXPECT_TRUE(server_->waitUntilDisconnected());
        const auto failures = server_->failures();
        ASSERT_EQ(failures.size(), 1u);
        EXPECT_NE(failures.front().find("  - stall() as the last step\n  + 1 steps behind it"), std::string::npos)
            << failures.front();
    }

    TEST_F(ScriptedFtpServerTests, Ipv6ControlConnection)
    {
        auto started = ScriptedFtpServer::start({.ipv6 = true});
        if (!started)
            GTEST_SKIP() << started.error();
        server_ = std::move(started).value();
        Script script{};
        script.reply("220 Ready").expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        EXPECT_TRUE(client_.readReply().has_value());
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, ExplicitTlsTurnsTheControlConnection)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready")
            .expect("AUTH", "TLS")
            .reply("234 Proceed")
            .startTls()
            .expect("USER")
            .reply("331 Password required")
            .expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        expectReply("AUTH TLS", 234);
        const auto pki = testPki();
        ASSERT_TRUE(pki.has_value()) << pki.error();
        const auto secured =
            client_.startTls({.authority = pki->get().authority, .hostName = "localhost"});
        ASSERT_TRUE(secured.has_value()) << secured.error();
        expectReply("USER user", 331);
        client_.close();
        expectNoFailures();
        EXPECT_TRUE(transcriptHas(TranscriptEntry::Kind::TlsEstablished, "control TLSv1.3"));
    }

    TEST_F(ScriptedFtpServerTests, ImplicitTlsStartsWithTheHandshake)
    {
        ASSERT_NO_FATAL_FAILURE(startServer({.implicitTls = true}));
        Script script{};
        script.reply("220 Ready").expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        const auto secured = client_.startTls();
        ASSERT_TRUE(secured.has_value()) << secured.error();
        const auto greeting = client_.readReply();
        ASSERT_TRUE(greeting.has_value()) << greeting.error();
        EXPECT_EQ(greeting->code, 220);
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, AHandshakeCanBeAnAlertInstead)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready")
            .expect("AUTH")
            .reply("234 Proceed")
            .startTls({.insteadOfHandshake = fatalAlert()})
            .stall();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        expectReply("AUTH TLS", 234);
        EXPECT_FALSE(client_.startTls().has_value());
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerTests, PlaintextCanFollowThe234)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready")
            .expect("AUTH")
            .replyRaw("234 Proceed\r\n")
            .startTls({.plaintextFirst = "230 Injected\r\n", .insteadOfHandshake = ""})
            .expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        expectReply("AUTH TLS", 234);
        const auto injected = client_.readReply();
        ASSERT_TRUE(injected.has_value()) << injected.error();
        EXPECT_EQ(injected->code, 230);
        client_.close();
        expectNoFailures();
    }

    class ScriptedFtpServerDataTests : public ScriptedFtpServerTests
    {
      protected:
        /**
         * @brief Logs in and asks for a data port with @p verb.
         */
        void openPassive(std::string_view verb, bool secured = false)
        {
            connectClient();
            ASSERT_TRUE(client_.readReply().has_value());
            expectReply("USER user", 331);
            expectReply("PASS password", 230);
            const auto reply = client_.command(verb);
            ASSERT_TRUE(reply.has_value()) << reply.error();
            const auto port = TestFtpClient::passivePort(*reply);
            ASSERT_TRUE(port.has_value()) << reply->text;
            const auto opened = client_.openData(server_->address(), *port, secured);
            ASSERT_TRUE(opened.has_value()) << opened.error();
        }
    };

    TEST_F(ScriptedFtpServerDataTests, DownloadsThroughEpsv)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("EPSV")
            .passive(DataPlan{}.send("file content", 5u))
            .expect("RETR", "file.txt")
            .reply("150 Here it comes")
            .beginData()
            .awaitData()
            .reply("226 Done")
            .expectDisconnect();
        server_->addConnection(std::move(script));

        openPassive("EPSV");
        expectReply("RETR file.txt", 150);
        const auto content = client_.readData();
        ASSERT_TRUE(content.has_value()) << content.error();
        EXPECT_EQ(*content, "file content");
        client_.closeData();
        const auto done = client_.readReply();
        ASSERT_TRUE(done.has_value()) << done.error();
        EXPECT_EQ(done->code, 226);
        client_.close();
        expectNoFailures();
        EXPECT_TRUE(transcriptHas(TranscriptEntry::Kind::DataConnected, "127.0.0.1"));
    }

    TEST_F(ScriptedFtpServerDataTests, BeginDataWithAPlanSendsWhatThatPlanSays)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("EPSV")
            .passive(DataPlan{}.send("planned at the EPSV"))
            .expect("RETR", "file.txt")
            .reply("150 Here it comes")
            .beginData(DataPlan{}.send("chosen at the RETR"))
            .awaitData()
            .reply("226 Done")
            .expectDisconnect();
        server_->addConnection(std::move(script));

        openPassive("EPSV");
        expectReply("RETR file.txt", 150);
        const auto content = client_.readData();
        ASSERT_TRUE(content.has_value()) << content.error();
        EXPECT_EQ(*content, "chosen at the RETR");
        client_.closeData();
        ASSERT_TRUE(client_.readReply().has_value());
        client_.close();
        expectNoFailures();
    }

    namespace
    {
        /**
         * @brief Logs in, echoes each other command in a 200, sends data for RETR and closes after QUIT.
         */
        class EchoResponder : public Responder
        {
          public:
            Script answer(std::string_view line) override
            {
                Script script{};
                if (line.starts_with("USER"))
                    return script.reply("331 Password required");
                if (line.starts_with("PASS"))
                    return script.reply("230 Logged in");
                if (line.starts_with("EPSV"))
                    return script.passive(DataPlan{});
                if (line.starts_with("RETR"))
                    return script.reply("150 Here it comes")
                        .beginData(DataPlan{}.send(std::string{line}))
                        .awaitData()
                        .reply("226 Done");
                if (line == "QUIT")
                    return script.reply("221 Goodbye.").closeControl();
                return script.reply(fmt::format("200 {}", line));
            }
        };
    }

    TEST_F(ScriptedFtpServerDataTests, AResponderAnswersEveryCommandUntilTheClientGoes)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").serve(std::make_shared<EchoResponder>());
        server_->addConnection(std::move(script));

        openPassive("EPSV");
        expectReply("RETR some file", 150);
        const auto content = client_.readData();
        ASSERT_TRUE(content.has_value()) << content.error();
        EXPECT_EQ(*content, "RETR some file");
        client_.closeData();
        ASSERT_TRUE(client_.readReply().has_value());
        const auto echoed = client_.command("NOOP");
        ASSERT_TRUE(echoed.has_value()) << echoed.error();
        EXPECT_EQ(echoed->text, "200 NOOP");
        expectReply("QUIT", 221);
        client_.close();
        expectNoFailures();
        EXPECT_EQ(
            server_->commands(),
            (std::vector<std::string>{"USER user", "PASS password", "EPSV", "RETR some file", "NOOP", "QUIT"})
        );
    }

    TEST_F(ScriptedFtpServerTests, StepsBehindServeFailTheScript)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.reply("220 Ready").serve(std::make_shared<EchoResponder>()).reply("200 Never");
        server_->addConnection(std::move(script));
        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        client_.close();
        EXPECT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->failures().size(), 1u);
    }

    TEST_F(ScriptedFtpServerDataTests, UploadsThroughPasv)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("PASV")
            .passive(DataPlan{}.expectUpload(1024u))
            .expect("STOR", "file.txt")
            .reply("150 Go ahead")
            .beginData()
            .awaitData()
            .reply("226 Done")
            .expectDisconnect();
        server_->addConnection(std::move(script));

        openPassive("PASV");
        expectReply("STOR file.txt", 150);
        ASSERT_TRUE(client_.writeData("uploaded content").has_value());
        client_.closeData();
        const auto done = client_.readReply();
        ASSERT_TRUE(done.has_value()) << done.error();
        EXPECT_EQ(done->code, 226);
        client_.close();
        expectNoFailures();
        EXPECT_TRUE(transcriptHas(TranscriptEntry::Kind::DataUploaded, "uploaded content"));
        EXPECT_TRUE(transcriptHas(TranscriptEntry::Kind::DataEnded, "end of stream"));
    }

    TEST_F(ScriptedFtpServerDataTests, AnAnnouncementCanLie)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("PASV")
            .passive(DataPlan{}.announce("203.0.113.7", 2121u))
            .expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        expectReply("USER user", 331);
        expectReply("PASS password", 230);
        const auto reply = client_.command("PASV");
        ASSERT_TRUE(reply.has_value()) << reply.error();
        EXPECT_EQ(reply->text, "227 Entering Passive Mode (203,0,113,7,8,73).");
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerDataTests, AnAnnouncementCanBeAnyReply)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("EPSV")
            .passive(DataPlan{}.announceAs("229 Port is {port}, or {high} and {low}"))
            .expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        expectReply("USER user", 331);
        expectReply("PASS password", 230);
        const auto reply = client_.command("EPSV");
        ASSERT_TRUE(reply.has_value()) << reply.error();
        EXPECT_TRUE(reply->text.starts_with("229 Port is ")) << reply->text;
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerDataTests, SendForeverEndsWhenTheScriptStopsIt)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("EPSV")
            .passive(DataPlan{}.sendForever())
            .expect("RETR")
            .reply("150 Here it comes")
            .beginData()
            .expect("ABOR")
            .stopData()
            .awaitData()
            .reply("426 Aborted")
            .expectDisconnect();
        server_->addConnection(std::move(script));

        openPassive("EPSV");
        expectReply("RETR file.txt", 150);
        expectReply("ABOR", 426);
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerDataTests, ADataResetIsNoCleanEnd)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("EPSV")
            .passive(DataPlan{}.send("partial").reset())
            .expect("RETR")
            .reply("150 Here it comes")
            .beginData()
            .awaitData()
            .reply("426 Broken")
            .expectDisconnect();
        server_->addConnection(std::move(script));

        openPassive("EPSV");
        expectReply("RETR file.txt", 150);
        EXPECT_FALSE(client_.readData().has_value());
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerDataTests, EveryDataPlanEndsWithItsControlConnection)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("EPSV")
            .passive(DataPlan{}.send("first"))
            .expect("EPSV")
            .passive(DataPlan{}.send("second"))
            .expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        ASSERT_TRUE(client_.readReply().has_value());
        expectReply("USER user", 331);
        expectReply("PASS password", 230);
        const auto first = client_.command("EPSV");
        ASSERT_TRUE(first.has_value()) << first.error();
        const auto firstPort = TestFtpClient::passivePort(*first);
        ASSERT_TRUE(firstPort.has_value()) << first->text;
        expectReply("EPSV", 229);
        client_.close();
        expectNoFailures();

        TestFtpClient probe{};
        EXPECT_FALSE(probe.connect(server_->address(), *firstPort).has_value());
    }

    TEST_F(ScriptedFtpServerDataTests, ADataPlanStoppedBeforeItBeganStartsNoTls)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login().expect("EPSV").passive(DataPlan{}.tls().send("never sent")).expectDisconnect();
        server_->addConnection(std::move(script));

        openPassive("EPSV");
        ASSERT_TRUE(server_->waitForTranscriptEntry(TranscriptEntry::Kind::DataConnected));
        client_.close();
        expectNoFailures();
        EXPECT_FALSE(transcriptHas(TranscriptEntry::Kind::TlsFailed));
    }

    TEST_F(ScriptedFtpServerDataTests, AStallLastsUntilTheClientLeaves)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        Script script{};
        script.login()
            .expect("EPSV")
            .passive(DataPlan{}.stallAfter("partial"))
            .expect("RETR")
            .reply("150 Here it comes")
            .beginData()
            .awaitData()
            .checkpoint("ended")
            .expectDisconnect();
        server_->addConnection(std::move(script));

        openPassive("EPSV");
        expectReply("RETR file.txt", 150);
        client_.closeData();
        EXPECT_TRUE(server_->waitForCheckpoint("ended"));
        client_.close();
        expectNoFailures();
    }

    class ScriptedFtpServerDataTlsTests : public ScriptedFtpServerDataTests
    {
      protected:
        Script securedScript(DataPlan plan)
        {
            Script script{};
            script.reply("220 Ready")
                .expect("AUTH", "TLS")
                .reply("234 Proceed")
                .startTls()
                .expect("USER")
                .reply("331 Password required")
                .expect("PASS")
                .reply("230 Logged in")
                .expect("EPSV")
                .passive(std::move(plan))
                .expect("RETR")
                .reply("150 Here it comes")
                .beginData()
                .awaitData()
                .reply("226 Done")
                .expectDisconnect();
            return script;
        }

        void login()
        {
            connectClient();
            ASSERT_TRUE(client_.readReply().has_value());
            expectReply("AUTH TLS", 234);
            ASSERT_TRUE(client_.startTls().has_value());
            expectReply("USER user", 331);
            expectReply("PASS password", 230);
        }

        void openSecuredData()
        {
            const auto reply = client_.command("EPSV");
            ASSERT_TRUE(reply.has_value()) << reply.error();
            const auto port = TestFtpClient::passivePort(*reply);
            ASSERT_TRUE(port.has_value()) << reply->text;
            // The handshake of the data connection needs the server, which runs the plan after the RETR.
            ASSERT_TRUE(client_.send("RETR file.txt").has_value());
            const auto preliminary = client_.readReply();
            ASSERT_TRUE(preliminary.has_value()) << preliminary.error();
            EXPECT_EQ(preliminary->code, 150);
            dataOpened_ = client_.openData(server_->address(), *port, true);
        }

      protected:
        std::expected<void, std::string> dataOpened_{};
    };

    TEST_F(ScriptedFtpServerDataTlsTests, ADataConnectionResumesTheControlSession)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        server_->addConnection(
            securedScript(DataPlan{}.tls({.requireResumption = true}).send("over tls"))
        );

        login();
        openSecuredData();
        ASSERT_TRUE(dataOpened_.has_value()) << dataOpened_.error();
        const auto content = client_.readData();
        ASSERT_TRUE(content.has_value()) << content.error();
        EXPECT_EQ(*content, "over tls");
        client_.closeData();
        EXPECT_TRUE(client_.readReply().has_value());
        client_.close();
        expectNoFailures();
        EXPECT_TRUE(transcriptHas(TranscriptEntry::Kind::TlsEstablished, "data TLSv1.3 resumed"));
    }

    TEST_F(ScriptedFtpServerDataTlsTests, ResumptionCanBeRefused)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        server_->addConnection(securedScript(DataPlan{}.tls({.refuseResumption = true}).send("over tls")));

        login();
        openSecuredData();
        ASSERT_TRUE(dataOpened_.has_value()) << dataOpened_.error();
        EXPECT_TRUE(client_.readData().has_value());
        client_.closeData();
        EXPECT_TRUE(client_.readReply().has_value());
        client_.close();
        expectNoFailures();
        EXPECT_TRUE(transcriptHas(TranscriptEntry::Kind::TlsEstablished, "data TLSv1.3"));
    }

    TEST_F(ScriptedFtpServerDataTlsTests, ADataConnectionCanPresentAnotherCertificate)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        const auto pki = testPki();
        ASSERT_TRUE(pki.has_value()) << pki.error();
        server_->addConnection(
            securedScript(DataPlan{}.tls({.certificate = pki->get().otherServer}).send("over tls"))
        );

        login();
        openSecuredData();
        ASSERT_TRUE(dataOpened_.has_value()) << dataOpened_.error();
        EXPECT_EQ(client_.dataCertificate(), pki->get().otherServer.certificate);
        EXPECT_TRUE(client_.readData().has_value());
        client_.closeData();
        EXPECT_TRUE(client_.readReply().has_value());
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerDataTlsTests, AClosedDataConnectionCanOmitCloseNotify)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        server_->addConnection(
            securedScript(DataPlan{}.tls({.closeWithoutCloseNotify = true}).send("over tls"))
        );

        login();
        openSecuredData();
        ASSERT_TRUE(dataOpened_.has_value()) << dataOpened_.error();
        const auto content = client_.readData();
        ASSERT_FALSE(content.has_value());
        EXPECT_NE(content.error().find("stream truncated"), std::string::npos) << content.error();
        client_.closeData();
        EXPECT_TRUE(client_.readReply().has_value());
        client_.close();
        expectNoFailures();
    }

    TEST_F(ScriptedFtpServerDataTlsTests, AnUnansweredCloseNotifyEndsThePlanInTime)
    {
        ASSERT_NO_FATAL_FAILURE(startServer());
        server_->addConnection(securedScript(DataPlan{}.tls().send("over tls")));

        login();
        openSecuredData();
        ASSERT_TRUE(dataOpened_.has_value()) << dataOpened_.error();
        EXPECT_TRUE(client_.readData().has_value());
        // The data connection stays open without the close_notify of the client.
        const auto done = client_.readReply();
        ASSERT_TRUE(done.has_value()) << done.error();
        EXPECT_EQ(done->code, 226);
        client_.closeData();
        client_.close();
        expectNoFailures();
    }

    struct BadCertificate
    {
        std::string name{};
        CertifiedKey TestPki::* certificate{nullptr};
    };

    class TestPkiTests : public ScriptedFtpServerTests, public ::testing::WithParamInterface<BadCertificate>
    {};

    TEST_P(TestPkiTests, AVerifyingClientRefusesTheCertificate)
    {
        const auto pki = testPki();
        ASSERT_TRUE(pki.has_value()) << pki.error();
        ASSERT_NO_FATAL_FAILURE(startServer({.implicitTls = true, .certificate = pki->get().*GetParam().certificate}));
        Script script{};
        script.expectDisconnect();
        server_->addConnection(std::move(script));

        connectClient();
        const auto secured = client_.startTls({.authority = pki->get().authority, .hostName = "localhost"});
        ASSERT_FALSE(secured.has_value());
        EXPECT_NE(secured.error().find("certificate verify failed"), std::string::npos) << secured.error();
        client_.close();
        expectNoFailures();
    }

    INSTANTIATE_TEST_SUITE_P(
        Certificates,
        TestPkiTests,
        ::testing::Values(
            BadCertificate{"SelfSigned", &TestPki::selfSigned},
            BadCertificate{"Expired", &TestPki::expired},
            BadCertificate{"WrongName", &TestPki::wrongName},
            BadCertificate{"OtherAuthority", &TestPki::otherAuthority}
        ),
        [](::testing::TestParamInfo<BadCertificate> const& info)
        {
            return info.param.name;
        }
    );

    TEST_F(ScriptedFtpServerTests, AVerifyingClientAcceptsTheServerCertificateOnEveryName)
    {
        const auto pki = testPki();
        ASSERT_TRUE(pki.has_value()) << pki.error();
        ASSERT_NO_FATAL_FAILURE(startServer({.implicitTls = true}));
        for (auto const* name : {"localhost", "127.0.0.1"})
        {
            Script script{};
            script.reply("220 Ready").expectDisconnect();
            server_->addConnection(std::move(script));

            TestFtpClient client{};
            ASSERT_TRUE(client.connect(server_->address(), server_->port()).has_value());
            const auto secured = client.startTls({.authority = pki->get().authority, .hostName = name});
            EXPECT_TRUE(secured.has_value()) << secured.error();
            EXPECT_TRUE(client.readReply().has_value());
        }
        expectNoFailures();
    }

    TEST(FtpScriptTests, CannedFeatRepliesEndWithTheirLastLine)
    {
        for (const auto server : {ServerLike::Vsftpd, ServerLike::Proftpd, ServerLike::PureFtpd, ServerLike::Iis})
        {
            Script script{};
            script.featLike(server);
            ASSERT_EQ(script.steps.size(), 2u);
            auto const& reply = std::get<Script::Send>(script.steps[1]);
            EXPECT_TRUE(reply.bytes.starts_with("211-"));
            EXPECT_NE(reply.bytes.rfind("\r\n211 "), std::string::npos);
        }
    }

    TEST(FtpScriptTests, CannedAbortSequencesExpectTheCommandFirst)
    {
        for (const auto replies :
             {AbortReplies::VsftpdLike,
              AbortReplies::ProftpdPlainLike,
              AbortReplies::ProftpdTlsLike,
              AbortReplies::PureFtpdLike,
              AbortReplies::PureFtpdFailedLike,
              AbortReplies::FinalOnly})
        {
            Script script{};
            script.abortSequence(replies);
            ASSERT_GE(script.steps.size(), 2u);
            EXPECT_EQ(std::get<Script::Expect>(script.steps[0]).verb, "ABOR");
        }
    }

    TEST(FtpScriptTests, ListingsEndEachEntryInCrLf)
    {
        const std::vector<ListingEntry> entries{{.name = "file.txt", .size = 12u}, {.name = "dir", .directory = true}};
        EXPECT_EQ(
            listing(ListingFormat::Unix, entries),
            "-rwxr-xr-x 1 ftp ftp         12 Oct 01 12:34 file.txt\r\n"
            "drwxr-xr-x 1 ftp ftp          0 Oct 01 12:34 dir\r\n"
        );
        EXPECT_EQ(
            listing(ListingFormat::Dos, entries),
            "10-01-26  12:34PM                   12 file.txt\r\n"
            "10-01-26  12:34PM       <DIR>          dir\r\n"
        );
        EXPECT_EQ(
            listing(ListingFormat::Mlsd, entries),
            "type=file;size=12;modify=20261001123400; file.txt\r\n"
            "type=dir;modify=20261001123400; dir\r\n"
        );
    }
}
