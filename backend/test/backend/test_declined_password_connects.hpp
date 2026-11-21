#pragma once

#include "test_session_manager_passwords.hpp"

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
#include <vector>

namespace Test
{
    namespace DeclinedPasswordConnectTesting
    {
        /**
         * @brief Gives no password for any prompt, as a cancelled prompt does.
         */
        class DecliningProvider : public PasswordProvider
        {
          public:
            void getPassword(
                std::string const&,
                std::string const&,
                std::function<void(std::optional<std::string>)> const& onPasswordReady
            ) override
            {
                onPasswordReady(std::nullopt);
            }
        };

        /**
         * @brief Answers the prompts with its answers in their order, with no password once they ran out.
         */
        class ScriptedProvider : public PasswordProvider
        {
          public:
            explicit ScriptedProvider(std::vector<std::optional<std::string>> answers)
                : answers_{std::move(answers)}
            {}

            void getPassword(
                std::string const&,
                std::string const&,
                std::function<void(std::optional<std::string>)> const& onPasswordReady
            ) override
            {
                if (next_ >= answers_.size())
                    return onPasswordReady(std::nullopt);
                onPasswordReady(answers_[next_++]);
            }

          private:
            const std::vector<std::optional<std::string>> answers_;
            std::size_t next_{0u};
        };
    }

    /**
     * @brief SessionManager::noticingDeclinedPasswords, which tells the connect RPC to answer "cancelled".
     */
    class DeclinedPasswordConnectTests : public SessionManagerPasswordTests
    {
      protected:
        /**
         * @brief Whether a connect that asks for @p prompts passwords through the libssh callback and then fails was
         *        noticed as one whose password was declined.
         */
        bool declinedDuringConnect(std::size_t prompts = 1u)
        {
            auto declined = std::make_shared<bool>(false);
            auto completed = std::make_shared<std::promise<bool>>();
            auto future = completed->get_future();
            manager_->addSession(
                manager_->noticingDeclinedPasswords(
                    [manager = manager_.get(),
                        prompts](std::function<void()>) -> std::expected<SessionConnection, std::string>
                    {
                        std::pair<SessionManager*, std::string> userData{manager, "password"};
                        for (std::size_t asked = 0u; asked < prompts; ++asked)
                        {
                            std::array<char, 64> buffer{};
                            askPassDefault("Password:", buffer.data(), buffer.size(), 0, 0, &userData);
                        }
                        return std::unexpected("asked");
                    },
                    declined
                ),
                {},
                [completed, declined](std::optional<Ids::SessionId> const&, std::optional<std::string> const&)
                {
                    completed->set_value(*declined);
                }
            );
            if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready)
                throw std::runtime_error{"the connect did not complete within 10 s"};
            return future.get();
        }
    };

    TEST_F(DeclinedPasswordConnectTests, AConnectWhosePasswordPromptGotNoPasswordIsNoticed)
    {
        DeclinedPasswordConnectTesting::DecliningProvider provider{};
        manager_->addPasswordProvider(0, &provider);
        EXPECT_TRUE(declinedDuringConnect());
    }

    TEST_F(DeclinedPasswordConnectTests, AConnectWithAnAnsweredPromptIsNotDeclinedAfterADeclinedOne)
    {
        DeclinedPasswordConnectTesting::DecliningProvider declining{};
        manager_->addPasswordProvider(0, &declining);
        ASSERT_TRUE(declinedDuringConnect());

        SessionManagerPasswordTesting::FixedProvider answering{"s3cret"};
        manager_->addPasswordProvider(-1, &answering);
        EXPECT_FALSE(declinedDuringConnect());
    }

    TEST_F(DeclinedPasswordConnectTests, AnAnsweredPromptAfterADeclinedOneLeavesTheConnectAFailure)
    {
        DeclinedPasswordConnectTesting::ScriptedProvider provider{{std::nullopt, "s3cret"}};
        manager_->addPasswordProvider(0, &provider);
        EXPECT_FALSE(declinedDuringConnect(2u));
    }

    TEST_F(DeclinedPasswordConnectTests, ADeclinedPromptAfterAnAnsweredOneIsNoticed)
    {
        DeclinedPasswordConnectTesting::ScriptedProvider provider{{"s3cret", std::nullopt}};
        manager_->addPasswordProvider(0, &provider);
        EXPECT_TRUE(declinedDuringConnect(2u));
    }

    TEST_F(DeclinedPasswordConnectTests, AConnectWithoutAProviderToAskIsNoticed)
    {
        EXPECT_TRUE(declinedDuringConnect());
    }

    TEST_F(DeclinedPasswordConnectTests, AConnectWhoseProviderFailedIsNotDeclined)
    {
        SessionManagerPasswordTesting::FixedProvider failing{std::nullopt};
        manager_->addPasswordProvider(0, &failing);
        EXPECT_FALSE(declinedDuringConnect());
    }
}
