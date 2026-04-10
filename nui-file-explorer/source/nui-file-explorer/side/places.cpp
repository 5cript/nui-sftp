#include <nui-file-explorer/side/places.hpp>
#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/places_provider_interface.hpp>
#include <nui-file-explorer/favorites_provider_interface.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/mouse_event.hpp>
#include <nui/event_system/observed_value.hpp>

#include <ui5-sap-icons/icons/bookmark.hpp>
#include <ui5-sap-icons/icons/home.hpp>
#include <ui5-sap-icons/icons/decline.hpp>

using namespace std::string_literals;

namespace NuiFileExplorer
{
    struct Places::Implementation
    {
        ISideModel* model;
        std::function<void(std::filesystem::path const&)> onNavigate;

        Nui::Observed<std::vector<IPlacesProvider::PlaceEntry>> defaultPlaces{};
        Nui::Observed<std::vector<IPlacesProvider::PlaceEntry>> drives{};
        std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites{};

        explicit Implementation(ISideModel& mdl, std::function<void(std::filesystem::path const&)> nav)
            : model{&mdl}
            , onNavigate{std::move(nav)}
        {
            if (auto* prov = model->placesProvider(); prov)
            {
                prov->requestDefaultPlaces(
                    [this](std::vector<IPlacesProvider::PlaceEntry> places)
                    {
                        defaultPlaces.value() = std::move(places);
                        defaultPlaces.modifyNow();
                    }
                );
            }
            if (auto* prov = model->drivesProvider(); prov)
            {
                prov->requestDrives(
                    [this](std::vector<IPlacesProvider::PlaceEntry> driveList)
                    {
                        drives.value() = std::move(driveList);
                        drives.modifyNow();
                    }
                );
            }
            if (auto* prov = model->favoritesProvider(); prov)
            {
                favorites = prov->favorites();
            }
        }
    };

    Places::Places(ISideModel& model, std::function<void(std::filesystem::path const&)> onNavigate)
        : impl_{std::make_unique<Implementation>(model, std::move(onNavigate))}
    {}

    void Places::reloadDefaultPlaces()
    {
        if (auto* prov = impl_->model->placesProvider(); prov)
        {
            prov->requestDefaultPlaces(
                [this](std::vector<IPlacesProvider::PlaceEntry> places)
                {
                    impl_->defaultPlaces.value() = std::move(places);
                    impl_->defaultPlaces.modifyNow();
                }
            );
        }
    }

    Places::~Places() = default;
    Places::Places(Places&&) = default;
    Places& Places::operator=(Places&&) = default;

    Nui::ElementRenderer Places::operator()()
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        auto* favProv = impl_->model->favoritesProvider();
        auto* placesProv = impl_->model->placesProvider();
        auto* drivesProv = impl_->model->drivesProvider();

        // clang-format off
        return div{
            class_ = "nui-file-grid-places"
        }(
            // --- Places section ---
            [this, placesProv]() -> Nui::ElementRenderer {
                if (!placesProv)
                    return Nui::nil();
                return div{}(
                    Nui::range(impl_->defaultPlaces)
                        .before(
                            div{class_ = "nui-file-grid-places-section-header"}("Places")
                        ),
                    [this](long long /*idx*/, IPlacesProvider::PlaceEntry const& entry) -> Nui::ElementRenderer {
                        return div{
                            class_ = "nui-file-grid-places-item",
                            onClick = [this, path = entry.path]() {
                                impl_->onNavigate(path);
                            }
                        }(
                            span{class_ = "nui-file-grid-places-item-icon"}(entry.icon),
                            span{}(entry.name)
                        );
                    }
                );
            }(),
            // --- Favorites section ---
            [this, favProv]() -> Nui::ElementRenderer {
                if (!favProv || !impl_->favorites)
                    return Nui::nil();
                return div{}(
                    Nui::range(*impl_->favorites)
                        .before(
                            div{class_ = "nui-file-grid-places-section-header"}("Favorites")
                        ),
                    [this, favProv](long long /*idx*/, std::filesystem::path const& fav) -> Nui::ElementRenderer {
                        return div{
                            class_ = "nui-file-grid-places-item",
                            onClick = [this, fav]() {
                                impl_->onNavigate(fav);
                            }
                        }(
                            span{class_ = "nui-file-grid-places-item-icon"}(Ui5Icons::bookmark()),
                            span{class_ = "nui-file-grid-places-item-label"}(fav.filename().generic_string()),
                            span{
                                class_ = "nui-file-grid-places-remove-btn",
                                onClick = [favProv, fav](Nui::WebApi::MouseEvent event) {
                                    Nui::WebApi::Console::log("Removing favorite: {}", fav.generic_string());
                                    event.stopPropagation();
                                    favProv->removeFavorite(fav);
                                }
                            }(Ui5Icons::decline())
                        );
                    }
                );
            }(),
            // --- Devices section ---
            [this]() -> Nui::ElementRenderer {
                if (!impl_->model->showRootEntry())
                    return Nui::nil();
                return div{}(
                    div{class_ = "nui-file-grid-places-section-header"}("Devices"),
                    div{
                        class_ = "nui-file-grid-places-item",
                        onClick = [this]() {
                            impl_->onNavigate("/");
                        }
                    }(
                        span{class_ = "nui-file-grid-places-item-icon"}(Ui5Icons::home()),
                        span{}("Root")
                    )
                );
            }(),
            // --- Drives section ---
            [this, drivesProv]() -> Nui::ElementRenderer {
                if (!drivesProv)
                    return Nui::nil();
                return div{}(
                    Nui::range(impl_->drives)
                        .before(
                            div{class_ = "nui-file-grid-places-section-header"}("Drives")
                        ),
                    [this](long long /*idx*/, IPlacesProvider::PlaceEntry const& entry) -> Nui::ElementRenderer {
                        return div{
                            class_ = "nui-file-grid-places-item",
                            onClick = [this, path = entry.path]() {
                                impl_->onNavigate(path);
                            }
                        }(
                            span{class_ = "nui-file-grid-places-item-icon"}(entry.icon),
                            span{}(entry.name)
                        );
                    }
                );
            }()
        );
        // clang-format on
    }
}
