#include <frontend/file_explorer/file_explorer_texts.hpp>
#include <utility/language.hpp>

#include <fmt/format.h>

NuiFileExplorer::Texts fileExplorerTexts()
{
    return {
        .toolbar =
            {
                .newMenu = language->get("fileExplorer", "toolbar", "newMenu"),
                .sortMenu = language->get("fileExplorer", "toolbar", "sortMenu"),
                .viewMenu = language->get("fileExplorer", "toolbar", "viewMenu"),
                .searchPlaceholder = language->get("fileExplorer", "toolbar", "searchPlaceholder"),
            },
        .newMenu =
            {
                .file = language->get("fileExplorer", "newMenu", "file"),
                .folder = language->get("fileExplorer", "newMenu", "folder"),
            },
        .sortMenu =
            {
                .nameAscending = language->get("fileExplorer", "sortMenu", "nameAscending"),
                .nameDescending = language->get("fileExplorer", "sortMenu", "nameDescending"),
                .sizeAscending = language->get("fileExplorer", "sortMenu", "sizeAscending"),
                .sizeDescending = language->get("fileExplorer", "sortMenu", "sizeDescending"),
                .infoAscending = language->get("fileExplorer", "sortMenu", "infoAscending"),
                .infoDescending = language->get("fileExplorer", "sortMenu", "infoDescending"),
                .modificationTimeAscending = language->get("fileExplorer", "sortMenu", "modificationTimeAscending"),
                .modificationTimeDescending = language->get("fileExplorer", "sortMenu", "modificationTimeDescending"),
            },
        .viewMenu =
            {
                .icons = language->get("fileExplorer", "viewMenu", "icons"),
                .table = language->get("fileExplorer", "viewMenu", "table"),
            },
        .columns =
            {
                .name = language->get("fileExplorer", "columns", "name"),
                .size = language->get("fileExplorer", "columns", "size"),
                .info = language->get("fileExplorer", "columns", "info"),
                .lastModified = language->get("fileExplorer", "columns", "lastModified"),
            },
        .places =
            {
                .places = language->get("fileExplorer", "places", "places"),
                .favorites = language->get("fileExplorer", "places", "favorites"),
                .devices = language->get("fileExplorer", "places", "devices"),
                .drives = language->get("fileExplorer", "places", "drives"),
                .root = language->get("fileExplorer", "places", "root"),
            },
        .contextMenu =
            {
                .synchronize = language->get("fileExplorer", "contextMenu", "synchronize"),
                .synchronizeHint = language->get("fileExplorer", "contextMenu", "synchronizeHint"),
            },
    };
}

std::string fileExplorerErrorText(NuiFileExplorer::Error const& error)
{
    using enum NuiFileExplorer::ErrorCode;
    switch (error.code)
    {
        case ExternalDropOnLocalSide:
            return language->get("sessionFrontend", "dropNotImplementedText");
        case DropOnSameSide:
            return language->get("fileExplorer", "errors", "dropOnSameSide");
        case DropDataUnreadable:
            return fmt::format(fmt::runtime(language->get("fileExplorer", "errors", "dropDataUnreadable")), error.detail);
    }
    return error.detail;
}
