#pragma once

#include <nui/window.hpp>
#include <nui/rpc.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace Test
{

    // -------------------------------------------------------------------------
    // NuiEnv — lazy singleton holding a Window + RpcHub for tests that need them
    // -------------------------------------------------------------------------

    /**
     * @brief Holds the single Nui::Window and Nui::RpcHub used by all display-
     *        dependent tests.  Construction is attempted once; if it fails (e.g.
     *        no display is available) available() returns false and the tests that
     *        use this object skip themselves via GTEST_SKIP().
     */
    struct NuiEnv
    {
        std::unique_ptr<Nui::Window> window;
        std::unique_ptr<Nui::RpcHub> hub;

        static NuiEnv& instance()
        {
            static NuiEnv env;
            return env;
        }

        bool available() const
        {
            return window != nullptr;
        }

        void shutdown()
        {
            hub.reset();
            window.reset();
        }

      private:
        NuiEnv()
        {
            try
            {
                window = std::make_unique<Nui::Window>();
                hub = std::make_unique<Nui::RpcHub>(*window);
            }
            catch (...)
            {
                window.reset();
                hub.reset();
            }
        }
    };

    class NuiEnvGuard : public ::testing::Environment
    {
      public:
        void TearDown() override
        {
            NuiEnv::instance().shutdown();
        }
    };
}