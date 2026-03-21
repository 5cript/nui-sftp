#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>

class MultiInputDialog
{
  public:
    MultiInputDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(MultiInputDialog);

    Nui::ElementRenderer operator()();

    struct InputField
    {
        std::string key;
        std::string label;
        std::string placeholder;
        bool isPassword{false};
    };
    struct OpenOptions
    {
        std::string headerText{};
        std::vector<InputField> inputFields;
        std::function<void(std::optional<std::unordered_map<std::string, std::string>> const&)> onConfirm;
    };
    void open(OpenOptions const& options);

  private:
    Nui::ElementRenderer dialogBody();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};