#include <frontend/components/code_editor.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <nui/frontend/val.hpp>

#include <utility>

namespace Components
{
    namespace
    {
        Nui::val codeEditorUtility()
        {
            return Nui::val::global("codeEditor");
        }

        bool isSet(Nui::val const& value)
        {
            return !value.isUndefined() && !value.isNull();
        }
    }

    struct CodeEditor::Implementation
    {
        Settings settings;
        Nui::val editor{Nui::val::undefined()};
        Nui::val onChangeDisposable{Nui::val::undefined()};

        explicit Implementation(Settings settings)
            : settings{std::move(settings)}
        {}

        ~Implementation()
        {
            dispose();
        }

        Implementation(Implementation const&) = delete;
        Implementation(Implementation&&) = delete;
        Implementation& operator=(Implementation const&) = delete;
        Implementation& operator=(Implementation&&) = delete;

        /**
         * @brief Disposes the change subscription first, so monaco drops its reference to the bound functor.
         */
        void dispose()
        {
            if (isSet(onChangeDisposable))
            {
                onChangeDisposable.call<void>("dispose");
                onChangeDisposable = Nui::val::undefined();
            }
            if (isSet(editor))
            {
                settings.initialValue = editor.call<std::string>("getValue");
                codeEditorUtility().call<void>("dispose", editor);
                editor = Nui::val::undefined();
            }
        }

        void create(Nui::val host)
        {
            dispose();

            auto options = Nui::val::object();
            options.set("value", settings.initialValue);
            options.set("language", settings.language);
            options.set("readOnly", settings.readOnly);
            editor = codeEditorUtility().call<Nui::val>("create", host, options);

            onChangeDisposable = editor.call<Nui::val>(
                "onDidChangeModelContent",
                Nui::bind(
                    [this](Nui::val)
                    {
                        if (settings.onChange)
                            settings.onChange(editor.call<std::string>("getValue"));
                    },
                    std::placeholders::_1
                )
            );

            if (settings.focusOnCreate)
                editor.call<void>("focus");
        }
    };

    CodeEditor::CodeEditor(Settings settings)
        : impl_{std::make_unique<Implementation>(std::move(settings))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(CodeEditor);

    Nui::ElementRenderer CodeEditor::operator()() const
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

        return div{
            class_ = "code-editor",
            reference.onMaterialize(
                [implementation = impl_.get()](Nui::val element)
                {
                    implementation->create(std::move(element));
                }
            ),
        }();
    }

    std::string CodeEditor::value() const
    {
        if (!isSet(impl_->editor))
            return impl_->settings.initialValue;
        return impl_->editor.call<std::string>("getValue");
    }

    void CodeEditor::setValue(std::string const& value)
    {
        if (!isSet(impl_->editor))
        {
            impl_->settings.initialValue = value;
            if (impl_->settings.onChange)
                impl_->settings.onChange(value);
            return;
        }
        impl_->editor.call<void>("setValue", value);
    }

    void CodeEditor::setLanguage(std::string const& language)
    {
        impl_->settings.language = language;
        if (isSet(impl_->editor))
            codeEditorUtility().call<void>("setLanguage", impl_->editor, language);
    }

    void CodeEditor::focus()
    {
        if (isSet(impl_->editor))
            impl_->editor.call<void>("focus");
    }
}
