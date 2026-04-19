#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <log/log.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/select.hpp>
#include <script-nui-components/slider.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>

std::string archiveCodecExtension(ArchiveCodec codec)
{
    switch (codec)
    {
        case ArchiveCodec::None:
            return {};
        case ArchiveCodec::Gzip:
            return ".gz";
        case ArchiveCodec::Bzip2:
            return ".bz2";
        case ArchiveCodec::Zstd:
            return ".zst";
        case ArchiveCodec::Xz:
            return ".xz";
    }
    return {};
}

ArchiveCodec archiveCodecFromExtension(std::string const& extension)
{
    if (extension == ".gz" || extension == "gz")
        return ArchiveCodec::Gzip;
    if (extension == ".bz2" || extension == "bz2")
        return ArchiveCodec::Bzip2;
    if (extension == ".zst" || extension == "zst")
        return ArchiveCodec::Zstd;
    if (extension == ".xz" || extension == "xz")
        return ArchiveCodec::Xz;
    return ArchiveCodec::None;
}

namespace
{
    /**
     * @brief The codec options presented in the dropdown, in display order. The
     * @c label field is what the user sees; the @c codec is returned back when the
     * user picks it.
     */
    struct CodecOption
    {
        std::string label;
        ArchiveCodec codec;
    };

    std::array<CodecOption, 5u> codecOptionsList()
    {
        return {{
            {".tar", ArchiveCodec::None},
            {".tar.gz", ArchiveCodec::Gzip},
            {".tar.bz2", ArchiveCodec::Bzip2},
            {".tar.zst", ArchiveCodec::Zstd},
            {".tar.xz", ArchiveCodec::Xz},
        }};
    }

    std::string codecLabel(ArchiveCodec codec)
    {
        for (auto const& option : codecOptionsList())
            if (option.codec == codec)
                return option.label;
        return ".tar.gz";
    }

    ArchiveCodec codecFromLabel(std::string const& label)
    {
        for (auto const& option : codecOptionsList())
            if (option.label == label)
                return option.codec;
        return ArchiveCodec::Gzip;
    }

    int clampLevel(int value)
    {
        return std::clamp(value, 1, 9);
    }
}

struct ArchiveTransferDialog::Implementation
{
    std::string id;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    Nui::Observed<std::string> fileStem{};
    Nui::Observed<std::string> codecLabelValue{};
    Nui::Observed<int> compressionLevel{5};
    std::vector<std::string> codecLabelOptions{};
    std::function<void(std::optional<ArchiveTransferResult> const&)> onConfirm;
    bool confirmOnClose{false};

    Implementation(std::string dialogId)
        : id{std::move(dialogId)}
    {
        codecLabelOptions.reserve(codecOptionsList().size());
        for (auto const& option : codecOptionsList())
            codecLabelOptions.push_back(option.label);
    }
};

ArchiveTransferDialog::ArchiveTransferDialog(std::string id)
    : impl_{std::make_unique<Implementation>(std::move(id))}
{
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, dialogBody());
}

Nui::ElementRenderer ArchiveTransferDialog::dialogBody()
{
    namespace Snc = ScriptNuiComponents;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Elements::label;

    // clang-format off
    return section{class_ = "archive-transfer-dialog"}(
        // Row 1 — file name (input + extension dropdown).
        label{class_ = "archive-transfer-dialog-label"}("File name"),
        div{class_ = "archive-transfer-dialog-filename"}(
            Snc::textInput({
                .value = impl_->fileStem,
                .attributes = {
                    id = fmt::format("{}-filename", impl_->id),
                    "keydown"_event = [this](Nui::WebApi::KeyboardEvent event){
                        dialogButtonContainerKeydown(event);
                        if (event.key() == "Enter") {
                            event.preventDefault();
                            impl_->confirmOnClose = true;
                            impl_->dialog->close();
                        }
                    }
                }
            }),
            div{class_ = "archive-transfer-dialog-codec"}(
                Snc::select(Snc::SelectOptions<decltype(impl_->codecLabelValue), std::vector<std::string>>{
                    .activeOption = impl_->codecLabelValue,
                    .options = impl_->codecLabelOptions,
                })
            )
        ),
        // Row 2 — compression level slider.
        label{class_ = "archive-transfer-dialog-label"}("Level"),
        div{class_ = "archive-transfer-dialog-level"}(
            span{class_ = "archive-transfer-dialog-level-hint"}("Fastest"),
            Snc::slider({
                .value = impl_->compressionLevel,
                .min = 1,
                .max = 9,
                .step = 1,
            }),
            span{class_ = "archive-transfer-dialog-level-hint"}("Smallest"),
            span{class_ = "archive-transfer-dialog-level-value"}(impl_->compressionLevel)
        )
    );
    // clang-format on
}

void ArchiveTransferDialog::open(OpenOptions const& options)
{
    namespace Snc = ScriptNuiComponents;

    impl_->onConfirm = options.onConfirm;
    impl_->fileStem = options.initialFileStem;
    impl_->codecLabelValue = codecLabel(options.initialCodec);
    impl_->compressionLevel = clampLevel(options.initialCompressionLevel);
    impl_->confirmOnClose = false;
    Nui::globalEventContext.executeActiveEventsImmediately();

    impl_->dialog->open({
        .headerText = options.headerText,
        .buttons = Snc::Dialog::Button::Ok | Snc::Dialog::Button::Cancel,
        .initialFocus = fmt::format("{}-filename", impl_->id),
        .onClose =
            [this](std::optional<Snc::Dialog::Button> button)
        {
            if (!impl_->onConfirm)
                return;

            const bool confirmed = (button && *button == Snc::Dialog::Button::Ok) || impl_->confirmOnClose;
            impl_->confirmOnClose = false;

            if (!confirmed)
            {
                impl_->onConfirm(std::nullopt);
                return;
            }

            ArchiveTransferResult result{
                .fileStem = impl_->fileStem.value(),
                .codec = codecFromLabel(impl_->codecLabelValue.value()),
                .compressionLevel = clampLevel(impl_->compressionLevel.value()),
            };
            impl_->onConfirm(result);
        },
        .modal = true,
        .mayCloseWithoutButton = false,
    });
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(ArchiveTransferDialog);

Nui::ElementRenderer ArchiveTransferDialog::operator()()
{
    return (*impl_->dialog)();
}
