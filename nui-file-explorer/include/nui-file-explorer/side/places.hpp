#pragma once

#include <nui-file-explorer/texts.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/event_system/observed_value.hpp>

#include <filesystem>
#include <functional>
#include <memory>

namespace NuiFileExplorer
{
    class ISideModel;

    class Places
    {
      public:
        /**
         * @param texts Owned by the side, which outlives the places panel.
         */
        Places(
            ISideModel& model,
            Nui::Observed<Texts> const& texts,
            std::function<void(std::filesystem::path const&)> onNavigate
        );
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
