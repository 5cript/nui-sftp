#pragma once

#include <nui/window.hpp>
#include <nui/rpc.hpp>

#include <gtest/gtest.h>

#ifdef __APPLE__
#    include <CoreGraphics/CoreGraphics.h>
#endif

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
            constructed() = true;
            return env;
        }

        static bool& constructed()
        {
            static bool wasConstructed = false;
            return wasConstructed;
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
        /**
         * @brief Without a GUI session (ssh, CI agent) AppKit never finishes launching and the window blocks forever.
         */
        static bool canOpenWindows()
        {
#ifdef __APPLE__
            CFDictionaryRef session = CGSessionCopyCurrentDictionary();
            if (session == nullptr)
                return false;
            CFRelease(session);
#endif
            return true;
        }

        NuiEnv()
        {
            if (!canOpenWindows())
                return;
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
            if (NuiEnv::constructed())
                NuiEnv::instance().shutdown();
        }
    };
}