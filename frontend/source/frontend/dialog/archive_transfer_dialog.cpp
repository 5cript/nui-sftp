#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/dialog/dialog_buttons_keyboard_support.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/checkbox.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/select.hpp>
#include <script-nui-components/slider.hpp>
#include <script-nui-components/text_input.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/dom/basic_element.hpp>
#include <nui/event_system/observed_value_combinator.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <optional>

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

int backendCompressionCode(ArchiveCodec codec)
{
    switch (codec)
    {
        case ArchiveCodec::None:
            return 1;
        case ArchiveCodec::Gzip:
            return 2;
        case ArchiveCodec::Bzip2:
            return 3;
        case ArchiveCodec::Zstd:
            return 4;
        case ArchiveCodec::Xz:
            return 5;
    }
    return 2;
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

    /**
     * @brief The level range a codec's library accepts, the same ranges TarArchive::compressionOptionsForLevel
     * clamps to.
     */
    struct LevelScale
    {
        int minimum;
        int maximum;
        int defaultLevel;
        /** @brief The first zstd ultra level, it needs much more memory to pack and to unpack. */
        std::optional<int> firstUltraLevel{};
    };

    LevelScale levelScale(ArchiveCodec codec)
    {
        switch (codec)
        {
            case ArchiveCodec::Bzip2:
                return {.minimum = 1, .maximum = 9, .defaultLevel = 9};
            case ArchiveCodec::Zstd:
                return {.minimum = 1, .maximum = 22, .defaultLevel = 3, .firstUltraLevel = 20};
            case ArchiveCodec::Xz:
                return {.minimum = 0, .maximum = 9, .defaultLevel = 6};
            case ArchiveCodec::None:
            case ArchiveCodec::Gzip:
                break;
        }
        return {.minimum = 1, .maximum = 9, .defaultLevel = 6};
    }

    std::size_t codecIndex(ArchiveCodec codec)
    {
        return static_cast<std::size_t>(codec);
    }

    bool isUltraLevel(ArchiveCodec codec, int level)
    {
        const auto firstUltraLevel = levelScale(codec).firstUltraLevel;
        return firstUltraLevel && level >= *firstUltraLevel;
    }

    /**
     * @brief Memory in MiB xz needs for each preset, from the xz(1) manual.
     */
    constexpr std::array<int, 10u> xzPackMemory{3, 9, 17, 32, 48, 94, 94, 186, 370, 674};
    constexpr std::array<int, 10u> xzUnpackMemory{1, 2, 3, 5, 5, 9, 9, 17, 33, 65};

    std::string kilobytesText(int kilobytes)
    {
        if (kilobytes < 1000)
            return fmt::format("{} kB", kilobytes);
        return fmt::format("{:.1f} MB", kilobytes / 1000.0);
    }

    std::string levelText(ArchiveCodec codec, int level)
    {
        if (codec == ArchiveCodec::Bzip2)
            return fmt::format("{} kB", level * 100);
        return std::to_string(level);
    }

    char const* levelLabelKey(ArchiveCodec codec)
    {
        switch (codec)
        {
            case ArchiveCodec::Bzip2:
                return "blockSize";
            case ArchiveCodec::Xz:
                return "preset";
            case ArchiveCodec::None:
            case ArchiveCodec::Gzip:
            case ArchiveCodec::Zstd:
                break;
        }
        return "level";
    }

    std::string memoryNote(std::string const& pack, std::string const& unpack)
    {
        return fmt::format(fmt::runtime(language->get("archiveTransferDialog", "memoryUse")), pack, unpack);
    }

    std::string levelNote(ArchiveCodec codec, int level)
    {
        const auto defaultNote = fmt::format(
            fmt::runtime(language->get("archiveTransferDialog", "defaultLevel")),
            levelText(codec, levelScale(codec).defaultLevel)
        );
        switch (codec)
        {
            case ArchiveCodec::Bzip2:
                return fmt::format(
                    "{} {}", memoryNote(kilobytesText(400 + 800 * level), kilobytesText(100 + 400 * level)), defaultNote
                );
            case ArchiveCodec::Xz:
            {
                const auto preset = static_cast<std::size_t>(std::clamp(level, 0, 9));
                return fmt::format(
                    "{} {}",
                    memoryNote(fmt::format("{} MiB", xzPackMemory[preset]), fmt::format("{} MiB", xzUnpackMemory[preset])),
                    defaultNote
                );
            }
            case ArchiveCodec::Zstd:
                if (isUltraLevel(codec, level))
                    return language->get("archiveTransferDialog", "ultraLevelPicked");
                return fmt::format("{} {}", defaultNote, language->get("archiveTransferDialog", "ultraLevels"));
            case ArchiveCodec::None:
            case ArchiveCodec::Gzip:
                break;
        }
        return defaultNote;
    }

    double trackFraction(LevelScale const& scale, double level)
    {
        return (level - scale.minimum) / (scale.maximum - scale.minimum);
    }

    /**
     * @brief Where @p level sits on the slider track, from 0 to 1, for a marker below the track.
     */
    std::string trackPosition(LevelScale const& scale, double level)
    {
        return fmt::format("--archive-transfer-dialog-position: {:.4f}", trackFraction(scale, level));
    }

    /**
     * @brief How far the track is filled for the current @p level, from 0 to 1.
     */
    std::string trackValue(LevelScale const& scale, int level)
    {
        return fmt::format("--archive-transfer-dialog-value: {:.4f}", trackFraction(scale, level));
    }
}

struct ArchiveTransferDialog::Implementation
{
    std::string id;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;
    Nui::Observed<std::string> fileStem{};
    Nui::Observed<std::string> codecLabelValue{};
    /** @brief One level per codec, indexed by ArchiveCodec, so switching codecs keeps each choice. */
    std::array<Nui::Observed<int>, 5u> compressionLevels{};
    Nui::Observed<bool> xzExtreme{false};
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
    using Nui::Elements::label;

    // clang-format off
    return section{class_ = "archive-transfer-dialog"}(
        div{class_ = "archive-transfer-dialog-field"}(
            label{
                class_ = "archive-transfer-dialog-label",
                "for"_attr = fmt::format("{}-filename", impl_->id)
            }(language->getObserved("archiveTransferDialog", "fileName")),
            div{class_ = "archive-transfer-dialog-filename"}(
                Snc::textInput({
                    .value = impl_->fileStem,
                    .attributes = {
                        id = fmt::format("{}-filename", impl_->id),
                        "spellcheck"_attr = "false",
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
                Snc::select(Snc::SelectOptions<decltype(impl_->codecLabelValue), std::vector<std::string>>{
                    .activeOption = impl_->codecLabelValue,
                    .options = impl_->codecLabelOptions,
                    .attributes = {
                        "aria-label"_attr = language->get("archiveTransferDialog", "format"),
                    },
                })
            )
        ),
        div{class_ = "archive-transfer-dialog-compression"}(
            observe(impl_->codecLabelValue),
            [this]() -> Nui::ElementRenderer {
                return compressionSection(codecFromLabel(impl_->codecLabelValue.value()));
            }
        )
    );
    // clang-format on
}

Nui::ElementRenderer ArchiveTransferDialog::compressionSection(ArchiveCodec codec)
{
    namespace Snc = ScriptNuiComponents;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::label;
    using Nui::Elements::span;

    if (codec == ArchiveCodec::None)
    {
        return p{class_ = "archive-transfer-dialog-note"}(
            language->getObserved("archiveTransferDialog", "noCompression")
        );
    }

    const auto scale = levelScale(codec);
    auto& level = impl_->compressionLevels[codecIndex(codec)];
    const auto levelId = fmt::format("{}-level", impl_->id);

    // clang-format off
    return div{class_ = "archive-transfer-dialog-field"}(
        div{class_ = "archive-transfer-dialog-level-header"}(
            label{
                class_ = "archive-transfer-dialog-label",
                "for"_attr = levelId
            }(language->getObserved("archiveTransferDialog", levelLabelKey(codec))),
            span{
                class_ = observe(level).generate([codec](int value) {
                    return isUltraLevel(codec, value)
                        ? "archive-transfer-dialog-level-value archive-transfer-dialog-ultra"
                        : "archive-transfer-dialog-level-value";
                })
            }(observe(level).generate([codec](int value) {
                return levelText(codec, value);
            }))
        ),
        div{class_ = "archive-transfer-dialog-track"}(
            scale.firstUltraLevel
                ? span{
                    class_ = "archive-transfer-dialog-ultra-band",
                    style = trackPosition(scale, *scale.firstUltraLevel - 0.5)
                }()
                : Nui::nil(),
            span{
                class_ = "archive-transfer-dialog-default-tick",
                style = trackPosition(scale, scale.defaultLevel)
            }(),
            Snc::slider({
                .value = level,
                .min = scale.minimum,
                .max = scale.maximum,
                .step = 1,
                .attributes = {
                    id = levelId,
                    style = observe(level).generate([scale](int value) {
                        return trackValue(scale, value);
                    }),
                },
            })
        ),
        div{class_ = "archive-transfer-dialog-level-ends"}(
            span{}(fmt::format("{} · {}", levelText(codec, scale.minimum), language->get("archiveTransferDialog", codec == ArchiveCodec::Bzip2 ? "lessMemory" : "fastest"))),
            span{}(fmt::format("{} · {}", language->get("archiveTransferDialog", codec == ArchiveCodec::Bzip2 ? "betterRatio" : "smallest"), levelText(codec, scale.maximum)))
        ),
        codec == ArchiveCodec::Xz
            ? Snc::checkbox({
                .isChecked = impl_->xzExtreme,
                .label = language->getObserved("archiveTransferDialog", "extreme"),
            })
            : Nui::nil(),
        p{
            class_ = observe(level).generate([codec](int value) {
                return isUltraLevel(codec, value)
                    ? "archive-transfer-dialog-note archive-transfer-dialog-ultra"
                    : "archive-transfer-dialog-note";
            })
        }(observe(level).generate([codec](int value) {
            return levelNote(codec, value);
        }))
    );
    // clang-format on
}

void ArchiveTransferDialog::open(OpenOptions const& options)
{
    namespace Snc = ScriptNuiComponents;

    impl_->onConfirm = options.onConfirm;
    impl_->fileStem = options.initialFileStem;
    impl_->codecLabelValue = codecLabel(options.initialCodec);
    for (auto const& option : codecOptionsList())
        impl_->compressionLevels[codecIndex(option.codec)] = levelScale(option.codec).defaultLevel;
    impl_->xzExtreme = false;
    impl_->confirmOnClose = false;
    Nui::globalEventContext.executeActiveEventsImmediately();

    impl_->dialog->setButtonLabels(localizedButtonLabels());
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

            const auto codec = codecFromLabel(impl_->codecLabelValue.value());
            const auto scale = levelScale(codec);
            ArchiveTransferResult result{
                .fileStem = impl_->fileStem.value(),
                .codec = codec,
                .compressionLevel =
                    std::clamp(impl_->compressionLevels[codecIndex(codec)].value(), scale.minimum, scale.maximum),
                .xzExtreme = codec == ArchiveCodec::Xz && impl_->xzExtreme.value(),
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
