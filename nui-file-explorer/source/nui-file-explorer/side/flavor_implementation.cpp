#include <nui-file-explorer/side/flavor_implementation.hpp>
#include <nui-file-explorer/side.hpp>
#include <nui-file-explorer/preprocessor.hpp>

#include <nui/frontend/api/json.hpp>

#include <nlohmann/json.hpp>

namespace NuiFileExplorer
{
    FlavorImplementation::FlavorImplementation(Side& side, Side& otherSide)
        : side_{&side}
        , otherSide_{&otherSide}
    {}
    SideImplementation& FlavorImplementation::impl() const
    {
        return *side_->impl_;
    }
    SideImplementation& FlavorImplementation::otherImpl() const
    {
        return *otherSide_->impl_;
    }
    void FlavorImplementation::onDrop(Nui::WebApi::DragEvent event, std::optional<Item> const& droppedOnItem)
    {
        event.stopPropagation();
        event.preventDefault();

        auto dataTransferOpt = event.dataTransfer();
        if (!dataTransferOpt.has_value())
        {
            Nui::WebApi::Console::log("No data transfer in drop event.");
            return;
        }

        const auto types = dataTransferOpt->types();
        for (auto const& type : types)
        {
            Nui::WebApi::Console::log("Data transfer type: " + type);
        }

        std::optional<std::string> subdir = std::nullopt;

        if (droppedOnItem && droppedOnItem->type == Item::Type::Directory)
            subdir = droppedOnItem->path.filename().string();

        const auto files = dataTransferOpt->files();
        if (files.length() > 0 && STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webview2"s)
        {
            nlohmann::json msg = {
                {"type", "filedrop"},
                {"isLeft", side_->model().isLeft()},
                {"dropMetadata", side_->model().dropMetadata()},
            };
            if (subdir)
                msg["subdir"] = *subdir;

            Nui::val::global("chrome")["webview"].call<void>(
                "postMessageWithAdditionalObjects", msg.dump(), files.val()
            );
            return;
        }

        auto data = dataTransferOpt->getData("application/json"s);
        if (data.empty())
            return;

        try
        {
            auto info = Nui::JSON::parse(dataTransferOpt->getData("application/json"s));
            if (side_->model().isLeft() != info["isLeft"].as<bool>())
            {
                // its a transfer!
                otherSide_->model().onTransfer(otherImpl().selectionManager.selectedItems(), subdir);
            }
            else
            {
                // TODO: else its a move within the same side.
                side_->model().onError("drag and drop on the same side is not implemented");
            }
        }
        catch (std::exception const& e)
        {
            side_->model().onError(std::string{"Failed to parse drag and drop data: "} + e.what());
        }
    }
}