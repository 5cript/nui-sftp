#pragma once

#include <nui-file-explorer/flavor.hpp>
#include <nui-file-explorer/side.hpp>
#include <nui/utility/move_detector.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/attributes/impl/attribute.hpp>

#include <memory>
#include <string>
#include <vector>

namespace NuiFileExplorer
{
    class FileGrid
    {
      public:
        FileGrid(
            SideSettings const& leftSettings,
            SideSettings const& rightSettings,
            std::unique_ptr<ISideModel> leftModel,
            std::unique_ptr<ISideModel> rightModel
        );
        FileGrid(SideSettings const& leftSettings, std::unique_ptr<ISideModel> leftModel);
        ~FileGrid();
        FileGrid(const FileGrid&) = delete;
        FileGrid& operator=(const FileGrid&) = delete;
        FileGrid(FileGrid&&);
        FileGrid& operator=(FileGrid&&);

        /**
         * @brief Use this to close all menus and deselect all items.
         */
        void onUneventfulClick();

        /**
         * @brief Closes all menus without deselecting items.
         */
        void closeMenus();

        Nui::ElementRenderer operator()(std::vector<Nui::Attribute>&& attributes = {});

        Side& leftSide();
        Side* rightSide();

        ISideModel& leftModel();
        ISideModel* rightModel();

        /**
         * @brief Only visually, does not change what the methods leftSide and rightSide return.
         */
        void swapSides(bool doSwap);

        /**
         * @brief Replaces the texts of both sides, for example after a language change. Syncing the change is
         * left to the caller.
         */
        void texts(Texts const& value);

      private:
        Nui::MoveDetector moveDetector_;
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}