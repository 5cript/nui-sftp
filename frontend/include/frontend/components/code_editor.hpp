#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <string>

namespace Components
{
    /**
     * @brief A monaco code editor; the monaco instance lives from materialization to destruction.
     *
     * The JS side is static/source/code_editor.ts, exposed as globalThis.codeEditor.
     */
    class CodeEditor
    {
      public:
        struct Settings
        {
            /** @brief Monaco language id, e.g. "json" or "plaintext". */
            std::string language{"plaintext"};

            /** @brief Text the editor starts with. */
            std::string initialValue{};

            /** @brief Disallows editing when true. */
            bool readOnly{false};

            /** @brief Moves the keyboard focus into the editor once it is created. */
            bool focusOnCreate{false};

            /** @brief Called with the full text after every change, programmatic ones included. */
            std::function<void(std::string const&)> onChange{};
        };

        explicit CodeEditor(Settings settings);
        ROAR_PIMPL_SPECIAL_FUNCTIONS(CodeEditor);

        /**
         * @brief Renders the editor host; the monaco instance is created once it is in the DOM.
         */
        Nui::ElementRenderer operator()() const;

        /**
         * @brief The current text, or the initial value while the editor is not created yet.
         */
        std::string value() const;

        /**
         * @brief Replaces the whole text.
         */
        void setValue(std::string const& value);

        /**
         * @brief Switches the language of the current model.
         */
        void setLanguage(std::string const& language);

        /**
         * @brief Moves the keyboard focus into the editor.
         */
        void focus();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
