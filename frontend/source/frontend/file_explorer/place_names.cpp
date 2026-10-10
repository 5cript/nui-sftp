#include <frontend/file_explorer/place_names.hpp>
#include <utility/language.hpp>

#include <ui5-sap-icons/icons/home.hpp>
#include <ui5-sap-icons/icons/desktop-mobile.hpp>
#include <ui5-sap-icons/icons/download.hpp>
#include <ui5-sap-icons/icons/documents.hpp>
#include <ui5-sap-icons/icons/picture.hpp>
#include <ui5-sap-icons/icons/video.hpp>
#include <ui5-sap-icons/icons/folder.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace
{
    struct PlaceKind
    {
        std::string_view kind;
        Nui::ElementRenderer (*icon)();
        /**
         * @brief Spelled out per kind, so language-check sees every key.
         */
        std::string (*displayName)();
    };

    const std::array<PlaceKind, 8> placeKinds{{
        {"home", &Ui5Icons::home, [] { return language->get("places", "home"); }},
        {"desktop", &Ui5Icons::desktop_mobile, [] { return language->get("places", "desktop"); }},
        {"downloads", &Ui5Icons::download, [] { return language->get("places", "downloads"); }},
        {"documents", &Ui5Icons::documents, [] { return language->get("places", "documents"); }},
        {"pictures", &Ui5Icons::picture, [] { return language->get("places", "pictures"); }},
        {"videos", &Ui5Icons::video, [] { return language->get("places", "videos"); }},
        {"movies", &Ui5Icons::video, [] { return language->get("places", "movies"); }},
        {"music", &Ui5Icons::folder, [] { return language->get("places", "music"); }},
    }};

    PlaceKind const* findPlaceKind(std::string const& kind)
    {
        const auto iter = std::ranges::find(placeKinds, kind, &PlaceKind::kind);
        return iter == placeKinds.end() ? nullptr : &*iter;
    }
}

Nui::ElementRenderer iconForPlaceKind(std::string const& kind)
{
    const auto* placeKind = findPlaceKind(kind);
    return placeKind ? placeKind->icon() : Ui5Icons::folder();
}

std::string placeDisplayName(std::string const& kind, std::string const& fallback)
{
    const auto* placeKind = findPlaceKind(kind);
    return placeKind ? placeKind->displayName() : fallback;
}
