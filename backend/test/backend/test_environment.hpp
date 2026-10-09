#pragma once

#include <backend/process/environment.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace Test
{
    namespace
    {
        /**
         *  @brief Sets or removes a variable of this process and puts the previous state back on destruction.
         */
        class ScopedVariable
        {
          public:
            ScopedVariable(std::string name, std::optional<std::string> const& value)
                : name_{std::move(name)}
                , previous_{}
            {
                if (char const* const current = std::getenv(name_.c_str()); current != nullptr)
                    previous_ = current;
                apply(value);
            }
            ~ScopedVariable()
            {
                apply(previous_);
            }
            ScopedVariable(ScopedVariable const&) = delete;
            ScopedVariable(ScopedVariable&&) = delete;
            ScopedVariable& operator=(ScopedVariable const&) = delete;
            ScopedVariable& operator=(ScopedVariable&&) = delete;

          private:
            void apply(std::optional<std::string> const& value)
            {
#ifdef _WIN32
                _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
                if (value)
                    setenv(name_.c_str(), value->c_str(), 1);
                else
                    unsetenv(name_.c_str());
#endif
            }

            std::string name_;
            std::optional<std::string> previous_;
        };
    }

    TEST(EnvironmentTests, VariablesChangedByTheAppImageLauncherGetTheirHostValueBack)
    {
        const ScopedVariable changed{"NUI_SFTP_TEST_CHANGED", "/appimage/usr/lib:/host/lib"};
        const ScopedVariable original{"NUI_SFTP_APPIMAGE_ORIGINAL_NUI_SFTP_TEST_CHANGED", "/host/lib"};

        Environment environment;
        environment.loadFromCurrent();

        EXPECT_EQ(environment.environment().at("NUI_SFTP_TEST_CHANGED"), "/host/lib");
        EXPECT_FALSE(environment.environment().contains("NUI_SFTP_APPIMAGE_ORIGINAL_NUI_SFTP_TEST_CHANGED"));
    }

    TEST(EnvironmentTests, VariablesAddedByTheAppImageLauncherAreRemoved)
    {
        const ScopedVariable added{"NUI_SFTP_TEST_ADDED", "/appimage/usr/lib"};
        const ScopedVariable marker{"NUI_SFTP_APPIMAGE_UNSET_NUI_SFTP_TEST_ADDED", "1"};

        Environment environment;
        environment.loadFromCurrent();

        EXPECT_FALSE(environment.environment().contains("NUI_SFTP_TEST_ADDED"));
        EXPECT_FALSE(environment.environment().contains("NUI_SFTP_APPIMAGE_UNSET_NUI_SFTP_TEST_ADDED"));
    }

    TEST(EnvironmentTests, VariablesWithoutRecordedHostValueAreKept)
    {
        const ScopedVariable untouched{"NUI_SFTP_TEST_UNTOUCHED", "value"};

        Environment environment;
        environment.loadFromCurrent();

        EXPECT_EQ(environment.environment().at("NUI_SFTP_TEST_UNTOUCHED"), "value");
    }
}
