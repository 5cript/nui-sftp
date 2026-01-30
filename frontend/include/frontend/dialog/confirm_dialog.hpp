#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

class ConfirmDialog
{
  public:
    ConfirmDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ConfirmDialog);

    enum class Button : unsigned
    {
        Unknown = 0b0000'0000,
        Cancel = 0b0000'0001,
        Ok = 0b0000'0010,
        Yes = 0b0000'0100,
        No = 0b0000'1000,
        All = 0b0001'0000,
        None = 0b0010'0000,
    };

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
        State state = State::Information;
        std::string headerText = "";
        std::string text = "";
        Button buttons = Button::Ok;
        std::optional<Button> focusButton = std::nullopt;
        struct ListElement
        {
            std::string text;
            std::optional<std::string> description = std::nullopt;
            std::optional<std::string> additionalText = std::nullopt;
            std::optional<State> additionalState = std::nullopt;
        };
        std::vector<ListElement> listItems = {};
        std::function<void(Button buttonPressed)> onClose = [](auto) {};
    };

    void open(OpenOptions const& options);

  private:
    void close(Button button);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};