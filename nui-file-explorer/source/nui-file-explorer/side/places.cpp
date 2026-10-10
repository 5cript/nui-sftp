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
        Nui::Observed<Texts> const* texts;
        std::function<void(std::filesystem::path const&)> onNavigate;

        Nui::Observed<std::vector<IPlacesProvider::PlaceEntry>> defaultPlaces{};
        Nui::Observed<std::vector<IPlacesProvider::PlaceEntry>> drives{};
        std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> favorites{};
        /**
         * @brief Expires with this, so answers to place requests arriving afterwards are dropped.
         */
        std::shared_ptr<int> lifetime{std::make_shared<int>(0)};

        Implementation(
            ISideModel& mdl,
            Nui::Observed<Texts> const& txts,
            std::function<void(std::filesystem::path const&)> nav
        )
            : model{&mdl}
            , texts{&txts}
            , onNavigate{std::move(nav)}
        {
            requestDefaultPlaces();
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

        void requestDefaultPlaces()
        {
            if (auto* prov = model->placesProvider(); prov)
            {
                prov->requestDefaultPlaces(
                    [this, alive = std::weak_ptr{lifetime}](std::vector<IPlacesProvider::PlaceEntry> places)
                    {
                        if (alive.expired())
                            return;
                        defaultPlaces.value() = std::move(places);
                        defaultPlaces.modifyNow();
                    }
                );
            }
        }

        auto text(std::string Texts::Places::* member) const
        {
            return Nui::observe(*texts).generate(
                [this, member]()
                {
                    return texts->value().places.*member;
                }
            );
        }
    };

    Places::Places(
        ISideModel& model,
        Nui::Observed<Texts> const& texts,
        std::function<void(std::filesystem::path const&)> onNavigate
    )
        : impl_{std::make_unique<Implementation>(model, texts, std::move(onNavigate))}
    {}

    void Places::reloadDefaultPlaces()
    {
        impl_->requestDefaultPlaces();
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
                            div{class_ = "nui-file-grid-places-section-header"}(impl_->text(&Texts::Places::places))
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
                            div{class_ = "nui-file-grid-places-section-header"}(impl_->text(&Texts::Places::favorites))
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
                    div{class_ = "nui-file-grid-places-section-header"}(impl_->text(&Texts::Places::devices)),
                    div{
                        class_ = "nui-file-grid-places-item",
                        onClick = [this]() {
                            impl_->onNavigate("/");
                        }
                    }(
                        span{class_ = "nui-file-grid-places-item-icon"}(Ui5Icons::home()),
                        span{}(impl_->text(&Texts::Places::root))
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
                            div{class_ = "nui-file-grid-places-section-header"}(impl_->text(&Texts::Places::drives))
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
