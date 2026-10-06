#include <frontend/file_explorer/place_names.hpp>
#include <utility/language.hpp>

#include <ui5-sap-icons/icons/home.hpp>
#include <ui5-sap-icons/icons/desktop-mobile.hpp>
#include <ui5-sap-icons/icons/download.hpp>
#include <ui5-sap-icons/icons/documents.hpp>
#include <ui5-sap-icons/icons/picture.hpp>
#include <ui5-sap-icons/icons/video.hpp>
#include <ui5-sap-icons/icons/folder.hpp>

Nui::ElementRenderer iconForPlaceName(std::string const& name)
{
    if (name == "Home")
        return Ui5Icons::home();
    if (name == "Desktop")
        return Ui5Icons::desktop_mobile();
    if (name == "Downloads")
        return Ui5Icons::download();
    if (name == "Documents")
        return Ui5Icons::documents();
    if (name == "Pictures")
        return Ui5Icons::picture();
    if (name == "Videos")
        return Ui5Icons::video();
    return Ui5Icons::folder();
}

std::string placeDisplayName(std::string const& name)
{
    if (name == "Home")
        return language->get("places", "home");
    if (name == "Desktop")
        return language->get("places", "desktop");
    if (name == "Downloads")
        return language->get("places", "downloads");
    if (name == "Documents")
        return language->get("places", "documents");
    if (name == "Pictures")
        return language->get("places", "pictures");
    if (name == "Videos")
        return language->get("places", "videos");
    if (name == "Music")
        return language->get("places", "music");
    return name;
}
