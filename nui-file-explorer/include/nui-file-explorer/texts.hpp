#pragma once

#include <string>

namespace NuiFileExplorer
{
    /**
     * @brief Every text the file explorer renders itself. The defaults are English; pass translations through
     * SideSettings::texts or FileGrid::texts to localize.
     */
    struct Texts
    {
        struct Toolbar
        {
            std::string newMenu{"New"};
            std::string sortMenu{"Sort"};
            std::string viewMenu{"View"};
            std::string searchPlaceholder{"Search"};
        };

        struct NewMenu
        {
            std::string file{"File"};
            std::string folder{"Folder"};
        };

        struct SortMenu
        {
            std::string nameAscending{"Name Ascending"};
            std::string nameDescending{"Name Descending"};
            std::string sizeAscending{"Size Ascending"};
            std::string sizeDescending{"Size Descending"};
            std::string infoAscending{"Info Ascending"};
            std::string infoDescending{"Info Descending"};
            std::string modificationTimeAscending{"Modification Time Ascending"};
            std::string modificationTimeDescending{"Modification Time Descending"};
        };

        struct ViewMenu
        {
            std::string icons{"Icons"};
            std::string table{"Table"};
        };

        struct Columns
        {
            std::string name{"Name"};
            std::string size{"Size"};
            std::string info{"Info"};
            std::string lastModified{"Last Modified"};
        };

        struct Places
        {
            std::string places{"Places"};
            std::string favorites{"Favorites"};
            std::string devices{"Devices"};
            std::string drives{"Drives"};
            std::string root{"Root"};
        };

        struct ContextMenu
        {
            std::string synchronize{"Synchronize..."};
            std::string synchronizeHint{"Select exactly one directory on each side to synchronize."};
        };

        Toolbar toolbar{};
        NewMenu newMenu{};
        SortMenu sortMenu{};
        ViewMenu viewMenu{};
        Columns columns{};
        Places places{};
        ContextMenu contextMenu{};
    };
}
