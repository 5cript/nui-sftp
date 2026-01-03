#include <frontend/session_components/session_options.hpp>
#include <frontend/components/ui5/suggestion-item.hpp>
#include <frontend/events/frontend_events.hpp>
#include <log/log.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <fmt/format.h>

#include <ui5/components/button.hpp>
#include <ui5/components/input.hpp>
#include <ui5/components/select.hpp>

struct SessionOptions::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    std::string persistenceSessionName;
    std::string sessionLayoutId;
    ConfirmDialog* confirmDialog;

    std::string selectedLayout;
    Nui::Observed<std::vector<std::string>> layoutNames;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::string persistenceSessionName,
        std::string sessionLayoutId,
        ConfirmDialog* confirmDialog
    )
        : stateHolder{stateHolder}
        , events{events}
        , persistenceSessionName{std::move(persistenceSessionName)}
        , sessionLayoutId{std::move(sessionLayoutId)}
        , confirmDialog{confirmDialog}
        , selectedLayout{}
    {}
};

void SessionOptions::loadLayoutNames()
{
    impl_->stateHolder->load(
        [this](bool success, Persistence::StateHolder&)
        {
            if (!success)
            {
                Log::error("Failed to load state holder");
                return;
            }
            impl_->layoutNames.value().clear();

            if (const auto iter = impl_->stateHolder->stateCache().sessions.find(impl_->persistenceSessionName);
                iter != end(impl_->stateHolder->stateCache().sessions) && iter->second.layouts)
            {
                for (const auto& [name, session] : *iter->second.layouts)
                {
                    impl_->layoutNames.value().push_back(name);
                }
            }

            impl_->layoutNames.modifyNow();
        }
    );
}

SessionOptions::SessionOptions(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    std::string persistenceSessionName,
    std::string sessionLayoutId,
    ConfirmDialog* confirmDialog
)
    : impl_{std::make_unique<Implementation>(
          stateHolder,
          events,
          std::move(persistenceSessionName),
          std::move(sessionLayoutId),
          confirmDialog
      )}
{
    loadLayoutNames();
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(SessionOptions);

Nui::ElementRenderer SessionOptions::operator()()
{
    using Nui::Elements::div; // because of the global div.
    using namespace Nui::Attributes;

    return div{
        style = "width: 100%; height: auto; display: block; padding: 10px",
    }();

    // clang-format off
    // return div{
    //     style = "width: 100%; height: auto; display: block; padding: 10px",
    // }(
    //     div{
    //         style = "display: flex; flex-direction: row; gap: 10px; align-items: center",
    //     }(
    //         ui5::input{
    //             "value"_prop = impl_->selectedLayout,
    //             "input"_event = [this](Nui::val event) {
    //                 impl_->selectedLayout = event["target"]["value"].as<std::string>();
    //             },
    //             "close"_event = [this](Nui::val event) {
    //                 impl_->selectedLayout = event["target"]["value"].as<std::string>();
    //             },
    //             "change"_event = [this](Nui::val event) {
    //                 impl_->selectedLayout = event["target"]["value"].as<std::string>();
    //             },
    //             "placeholder"_prop = "Layout Name",
    //             "showSuggestions"_prop = true,
    //         }(
    //             Nui::range(impl_->layoutNames),
    //             [](long long, auto const& name) -> Nui::ElementRenderer {
    //                 return ui5::suggestion_item{
    //                     "text"_prop = name,
    //                 }();
    //             }
    //         ),
    //         ui5::button{
    //             "click"_event = [this](Nui::val) {
    //                 Log::info("Save layout clicked");
    //                 impl_->stateHolder->load([this](bool success, Persistence::StateHolder& stateHolder) {
    //                     if (!success)
    //                     {
    //                         Log::error("Failed to load state holder");
    //                         impl_->confirmDialog->open({
    //                             .state = ConfirmDialog::State::Negative,
    //                             .headerText = "Failed to save layout",
    //                             .text = "Failed to load state holder",
    //                         });
    //                         return;
    //                     }

    //                     auto layout = Nui::val::global("contentPanelManager").call<Nui::val>("getPanelLayout", impl_->sessionLayoutId);
    //                     if (layout.isUndefined())
    //                     {
    //                         Log::error("Failed to get layout");
    //                         impl_->confirmDialog->open({
    //                             .state = ConfirmDialog::State::Negative,
    //                             .headerText = "Failed to save layout",
    //                             .text = "Failed to get layout",
    //                         });
    //                         return;
    //                     }

    //                     auto& cache = stateHolder.stateCache();

    //                     if (auto iter = cache.sessions.find(impl_->persistenceSessionName); iter != end(cache.sessions))
    //                     {
    //                         if (!iter->second.layouts)
    //                             iter->second.layouts = decltype(iter->second.layouts)::value_type{};
    //                         iter->second.layouts->emplace(impl_->selectedLayout, nlohmann::json::parse(Nui::JSON::stringify(layout)));

    //                         stateHolder.save([this]() {
    //                             Log::info("Layout saved");
    //                             impl_->confirmDialog->open({
    //                                 .state = ConfirmDialog::State::Positive,
    //                                 .headerText = "Layout saved",
    //                                 .text = "Layout was saved successfully",
    //                             });
    //                             impl_->events->onLayoutsChanged.modifyNow();
    //                             loadLayoutNames();
    //                         });
    //                     }
    //                     else {
    //                         Log::error("No such persisted session config '{}'", impl_->persistenceSessionName);
    //                         impl_->confirmDialog->open({
    //                             .state = ConfirmDialog::State::Negative,
    //                             .headerText = "Failed to save layout",
    //                             .text = "No such persisted session config",
    //                         });
    //                     }
    //                 });
    //             }
    //         }(
    //             "Save Layout"
    //         ),
    //         ui5::button{
    //             "click"_event = [this](Nui::val) {
    //                 Log::info("Delete layout clicked");
    //                 impl_->stateHolder->load([this](bool success, Persistence::StateHolder& stateHolder) {
    //                     if (!success)
    //                     {
    //                         Log::error("Failed to load state holder");
    //                         impl_->confirmDialog->open({.state = ConfirmDialog::State::Negative, .headerText = "Failed to delete layout", .text = "Failed to load state holder",});
    //                         return;
    //                     }

    //                     auto& layouts = stateHolder.stateCache().sessions.at(impl_->persistenceSessionName).layouts;
    //                     if (!layouts) {
    //                         Log::error("No such layout");
    //                         impl_->confirmDialog->open({
    //                             .state = ConfirmDialog::State::Negative,
    //                             .headerText = "Failed to delete layout",
    //                             .text = "No such layout",
    //                         });
    //                     }
    //                     auto result = layouts->erase(impl_->selectedLayout);
    //                     if (result == 0) {
    //                         Log::error("Failed to delete layout '{}'", impl_->selectedLayout);
    //                         impl_->confirmDialog->open({
    //                             .state = ConfirmDialog::State::Negative,
    //                             .headerText = "Failed to delete layout",
    //                             .text = fmt::format("Failed to delete layout '{}'", impl_->selectedLayout),
    //                         });
    //                         return;
    //                     }

    //                     stateHolder.save([this]() {
    //                         Log::info("Layout '{}' deleted", impl_->selectedLayout);
    //                         impl_->confirmDialog->open({
    //                             .state = ConfirmDialog::State::Positive,
    //                             .headerText = "Layout deleted",
    //                             .text = fmt::format("Layout '{}' was deleted successfully", impl_->selectedLayout),
    //                         });
    //                         impl_->events->onLayoutsChanged.modifyNow();
    //                         loadLayoutNames();
    //                     });
    //                 });
    //             }
    //         }(
    //             "Delete Layout"
    //         )
    //     )
    // );
    // clang-format on
}