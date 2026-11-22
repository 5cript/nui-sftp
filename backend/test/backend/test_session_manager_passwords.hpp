#pragma once

#include <backend/ftp/ftp_engine_context.hpp>
#include <backend/password/password_provider.hpp>
#include <backend/session_manager.hpp>
#include <persistence/state_holder.hpp>
#include "nui_env.hpp"

#include <boost/asio/thread_pool.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Test
{
    namespace SessionManagerPasswordTesting
    {
        /**
         * @brief Answers every prompt with its answer, or throws without one.
         */
        class FixedProvider : public PasswordProvider
        {
          public:
            explicit FixedProvider(std::optional<std::string> answer)
                : answer_{std::move(answer)}
            {}

            void getPassword(
                std::string const& whatFor,
                std::string const& prompt,
                std::function<void(std::optional<std::string>)> const& onPasswordReady
            ) override
            {
                if (!answer_)
                    throw std::runtime_error{"the provider failed"};
                askedFor = whatFor;
                askedWith = prompt;
                onPasswordReady(answer_);
            }

            std::string askedFor{};
            std::string askedWith{};

          private:
            const std::optional<std::string> answer_;
        };

        struct CallbackResult
        {
            int returned{-1};
            std::string password{};
        };
    }

    /**
     * @brief askPassDefault, the libssh callback of the SessionManager, which runs on its strand during a connect.
     */
    class SessionManagerPasswordTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (!NuiEnv::instance().available())
                GTEST_SKIP() << "No display available";
            auto& env = NuiEnv::instance();
            manager_ = std::make_shared<SessionManager>(
                strandPool_.get_executor(), stateHolder_, *env.window, *env.hub, engine_
            );
        }

        void TearDown() override
        {
            if (manager_)
                manager_->shutdown();
            manager_.reset();
        }

        /**
         * @brief Calls askPassDefault on the strand of the manager, from within a connect that then fails.
         */
        SessionManagerPasswordTesting::CallbackResult askOnTheStrand(std::string whatFor, std::string const& prompt)
        {
            auto result = std::make_shared<SessionManagerPasswordTesting::CallbackResult>();
            auto completed = std::make_shared<std::promise<void>>();
            auto future = completed->get_future();
            manager_->addSession(
                [manager = manager_.get(), result, whatFor = std::move(whatFor), prompt](
                    std::function<void()>
                ) -> std::expected<SessionConnection, std::string>
                {
                    std::pair<SessionManager*, std::string> userData{manager, whatFor};
                    std::array<char, 64> buffer{};
                    result->returned = askPassDefault(prompt.c_str(), buffer.data(), buffer.size(), 0, 0, &userData);
                    result->password = buffer.data();
                    return std::unexpected("asked");
                },
                {},
                [completed](std::optional<Ids::SessionId> const&, std::optional<std::string> const&)
                {
                    completed->set_value();
                }
            );
            if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready)
                throw std::runtime_error{"the connect did not complete within 10 s"};
            return *result;
        }

        boost::asio::thread_pool strandPool_{2};
        Persistence::StateHolder stateHolder_{};
        FtpEngineContext engine_{1u};
        std::shared_ptr<SessionManager> manager_{};
    };

    TEST_F(SessionManagerPasswordTests, TheLibsshCallbackAsksTheProviders)
    {
        SessionManagerPasswordTesting::FixedProvider provider{"s3cret"};
        manager_->addPasswordProvider(0, &provider);
        const auto result = askOnTheStrand("keyPhrase", "Passphrase for key:");
        EXPECT_EQ(result.returned, 0);
        EXPECT_EQ(result.password, "s3cret");
        EXPECT_EQ(provider.askedFor, "keyPhrase");
        EXPECT_EQ(provider.askedWith, "Passphrase for key:");
    }

    TEST_F(SessionManagerPasswordTests, TheLibsshCallbackGivesNoPasswordWhenAProviderThrows)
    {
        SessionManagerPasswordTesting::FixedProvider provider{std::nullopt};
        manager_->addPasswordProvider(0, &provider);
        const auto result = askOnTheStrand("password", "Password:");
        EXPECT_EQ(result.returned, -1);
        EXPECT_TRUE(result.password.empty());
    }
}
