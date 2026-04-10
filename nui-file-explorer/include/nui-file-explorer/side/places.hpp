#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <filesystem>
#include <functional>
#include <memory>

namespace NuiFileExplorer
{
    class ISideModel;

    class Places
    {
      public:
        Places(ISideModel& model, std::function<void(std::filesystem::path const&)> onNavigate);
        ~Places();

        Places(Places const&) = delete;
        Places& operator=(Places const&) = delete;
        Places(Places&&);
        Places& operator=(Places&&);

        Nui::ElementRenderer operator()();

        void reloadDefaultPlaces();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
