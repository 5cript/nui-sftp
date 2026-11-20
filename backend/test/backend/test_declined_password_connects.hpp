#pragma once

#include "test_session_manager_passwords.hpp"

#include <array>
#include <chrono>
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
    }

    /**
     * @brief SessionManager::noticingDeclinedPasswords, which tells the connect RPC to answer "cancelled".
     */
    class DeclinedPasswordConnectTests : public SessionManagerPasswordTests
    {
      protected:
        /**
         * @brief Whether a connect that asks for a password through the libssh callback and then fails was noticed as
         *        one whose password was declined.
         */
        bool declinedDuringConnect()
        {
            auto declined = std::make_shared<bool>(false);
            auto completed = std::make_shared<std::promise<bool>>();
            auto future = completed->get_future();
            manager_->addSession(
                manager_->noticingDeclinedPasswords(
                    [manager = manager_.get()](std::function<void()>) -> std::expected<SessionConnection, std::string>
                    {
                        std::pair<SessionManager*, std::string> userData{manager, "password"};
                        std::array<char, 64> buffer{};
                        askPassDefault("Password:", buffer.data(), buffer.size(), 0, 0, &userData);
                        return std::unexpected("asked");
                    },
                    declined
                ),
                {},
                Log::Toast::Skip,
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
}
