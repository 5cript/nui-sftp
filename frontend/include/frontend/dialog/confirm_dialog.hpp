#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <script-nui-components/dialog.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

namespace Persistence
{
    class StateHolder;
}

class ConfirmDialog
{
  public:
    ConfirmDialog(std::string id, Persistence::StateHolder& stateHolder);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ConfirmDialog);

    using Button = ScriptNuiComponents::Dialog::Button;

    friend auto operator|(Button lhs, Button rhs)
    {
        return static_cast<Button>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
    }

    enum class State
    {
        None,
        Positive,
        Critical,
        Negative,
        Information
    };

    Nui::ElementRenderer operator()();

    struct OpenOptions
    {
        ScriptNuiComponents::StyleVariant styleVariant = ScriptNuiComponents::StyleVariant::Regular;
        std::string headerText = "";
        std::string text = "";
        Button buttons = Button::Ok;
        std::optional<Button> focusButton = std::nullopt;
        /** @brief Other labels for the buttons, the usual ones of the current language otherwise. */
        std::optional<ScriptNuiComponents::Dialog::ButtonLabels> buttonLabels = std::nullopt;
        struct ListElement
        {
            std::string text;
            std::optional<std::string> description = std::nullopt;
            std::optional<std::string> additionalText = std::nullopt;
            std::optional<State> additionalState = std::nullopt;
        };
        std::vector<ListElement> listItems = {};
        std::optional<std::string> neverShowAgainId = std::nullopt;
        std::function<void(std::optional<Button> buttonPressed)> onClose = [](auto) {};
    };

    /**
     * @brief Shows the dialog. A dialog still on screen is replaced, its onClose runs first without a button, so
     *        every onClose runs exactly once.
     */
    void open(OpenOptions const& options);

  private:
    Nui::ElementRenderer dialogBody();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};