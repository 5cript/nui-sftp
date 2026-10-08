#include <frontend/dialog/snippet_presets_dialog.hpp>
#include <frontend/dialog/localized_button_labels.hpp>
#include <frontend/dialog/snippet_import_dialog.hpp>
#include <frontend/command_store/command_store_client.hpp>
#include <frontend/notifications.hpp>

#include <command-store/snippet_import_plan.hpp>
#include <command-store/snippet_presets.hpp>
#include <command-store/snippet_transfer.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <ui5-sap-icons/icons/attachment-zip-file.hpp>
#include <ui5-sap-icons/icons/cloud.hpp>
#include <ui5-sap-icons/icons/database.hpp>
#include <ui5-sap-icons/icons/date-time.hpp>
#include <ui5-sap-icons/icons/display.hpp>
#include <ui5-sap-icons/icons/document-text.hpp>
#include <ui5-sap-icons/icons/documents.hpp>
#include <ui5-sap-icons/icons/folder.hpp>
#include <ui5-sap-icons/icons/internet-browser.hpp>
#include <ui5-sap-icons/icons/it-host.hpp>
#include <ui5-sap-icons/icons/it-instance.hpp>
#include <ui5-sap-icons/icons/key.hpp>
#include <ui5-sap-icons/icons/locked.hpp>
#include <ui5-sap-icons/icons/org-chart.hpp>
#include <ui5-sap-icons/icons/process.hpp>
#include <ui5-sap-icons/icons/product.hpp>
#include <ui5-sap-icons/icons/screen-split-two.hpp>
#include <ui5-sap-icons/icons/shield.hpp>
#include <ui5-sap-icons/icons/shipping-status.hpp>
#include <ui5-sap-icons/icons/source-code.hpp>
#include <ui5-sap-icons/icons/sys-monitor.hpp>
#include <ui5-sap-icons/icons/upload.hpp>
#include <ui5-sap-icons/icons/user-settings.hpp>
#include <ui5-sap-icons/icons/world.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using CommandStore::SnippetPresets::Preset;
    using Button = ScriptNuiComponents::Dialog::Button;

    // clang-format off
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char apkPreset[] = {
#embed "../../../../command-store/presets/apk.json"
        ,
        '\0',
    };
    constexpr char aptPreset[] = {
#embed "../../../../command-store/presets/apt.json"
        ,
        '\0',
    };
    constexpr char archivesPreset[] = {
#embed "../../../../command-store/presets/archives.json"
        ,
        '\0',
    };
    constexpr char certificatesPreset[] = {
#embed "../../../../command-store/presets/certificates.json"
        ,
        '\0',
    };
    constexpr char cronPreset[] = {
#embed "../../../../command-store/presets/cron.json"
        ,
        '\0',
    };
    constexpr char databasesPreset[] = {
#embed "../../../../command-store/presets/databases.json"
        ,
        '\0',
    };
    constexpr char disksAndFilesPreset[] = {
#embed "../../../../command-store/presets/disks-and-files.json"
        ,
        '\0',
    };
    constexpr char dnfPreset[] = {
#embed "../../../../command-store/presets/dnf.json"
        ,
        '\0',
    };
    constexpr char dockerPreset[] = {
#embed "../../../../command-store/presets/docker.json"
        ,
        '\0',
    };
    constexpr char dockerComposePreset[] = {
#embed "../../../../command-store/presets/docker-compose.json"
        ,
        '\0',
    };
    constexpr char fileTransferPreset[] = {
#embed "../../../../command-store/presets/file-transfer.json"
        ,
        '\0',
    };
    constexpr char firewallPreset[] = {
#embed "../../../../command-store/presets/firewall.json"
        ,
        '\0',
    };
    constexpr char gitPreset[] = {
#embed "../../../../command-store/presets/git.json"
        ,
        '\0',
    };
    constexpr char kubernetesPreset[] = {
#embed "../../../../command-store/presets/kubernetes.json"
        ,
        '\0',
    };
    constexpr char logsPreset[] = {
#embed "../../../../command-store/presets/logs.json"
        ,
        '\0',
    };
    constexpr char networkingPreset[] = {
#embed "../../../../command-store/presets/networking.json"
        ,
        '\0',
    };
    constexpr char nginxPreset[] = {
#embed "../../../../command-store/presets/nginx.json"
        ,
        '\0',
    };
    constexpr char pacmanPreset[] = {
#embed "../../../../command-store/presets/pacman.json"
        ,
        '\0',
    };
    constexpr char podmanPreset[] = {
#embed "../../../../command-store/presets/podman.json"
        ,
        '\0',
    };
    constexpr char processesPreset[] = {
#embed "../../../../command-store/presets/processes.json"
        ,
        '\0',
    };
    constexpr char screenPreset[] = {
#embed "../../../../command-store/presets/screen.json"
        ,
        '\0',
    };
    constexpr char systemPreset[] = {
#embed "../../../../command-store/presets/system.json"
        ,
        '\0',
    };
    constexpr char sshPreset[] = {
#embed "../../../../command-store/presets/ssh.json"
        ,
        '\0',
    };
    constexpr char systemdPreset[] = {
#embed "../../../../command-store/presets/systemd.json"
        ,
        '\0',
    };
    constexpr char tmuxPreset[] = {
#embed "../../../../command-store/presets/tmux.json"
        ,
        '\0',
    };
    constexpr char usersAndPermissionsPreset[] = {
#embed "../../../../command-store/presets/users-and-permissions.json"
        ,
        '\0',
    };
    constexpr char zypperPreset[] = {
#embed "../../../../command-store/presets/zypper.json"
        ,
        '\0',
    };
#pragma clang diagnostic pop
    // clang-format on

    /**
     * @brief The shipped presets in the order the cards show them.
     */
    std::vector<Preset> loadPresets()
    {
        constexpr std::string_view documents[] = {
            dockerPreset,
            dockerComposePreset,
            podmanPreset,
            kubernetesPreset,
            sshPreset,
            tmuxPreset,
            screenPreset,
            gitPreset,
            systemdPreset,
            cronPreset,
            logsPreset,
            processesPreset,
            systemPreset,
            networkingPreset,
            firewallPreset,
            disksAndFilesPreset,
            fileTransferPreset,
            archivesPreset,
            aptPreset,
            dnfPreset,
            pacmanPreset,
            zypperPreset,
            apkPreset,
            usersAndPermissionsPreset,
            certificatesPreset,
            nginxPreset,
            databasesPreset,
        };

        std::vector<Preset> presets{};
        presets.reserve(std::size(documents));
        for (auto const& document : documents)
        {
            auto preset = CommandStore::SnippetPresets::parse(document);
            if (!preset)
            {
                Log::error("Skipping an unreadable snippet preset: {}", document.substr(0, 80));
                continue;
            }
            presets.push_back(std::move(*preset));
        }
        return presets;
    }

    Nui::ElementRenderer presetIcon(std::string const& icon)
    {
        static const std::map<std::string_view, Nui::ElementRenderer (*)()> icons{
            {"attachment-zip-file", &Ui5Icons::attachment_zip_file},
            {"cloud", &Ui5Icons::cloud},
            {"database", &Ui5Icons::database},
            {"date-time", &Ui5Icons::date_time},
            {"display", &Ui5Icons::display},
            {"document-text", &Ui5Icons::document_text},
            {"documents", &Ui5Icons::documents},
            {"internet-browser", &Ui5Icons::internet_browser},
            {"it-host", &Ui5Icons::it_host},
            {"it-instance", &Ui5Icons::it_instance},
            {"key", &Ui5Icons::key},
            {"locked", &Ui5Icons::locked},
            {"org-chart", &Ui5Icons::org_chart},
            {"process", &Ui5Icons::process},
            {"product", &Ui5Icons::product},
            {"screen-split-two", &Ui5Icons::screen_split_two},
            {"shield", &Ui5Icons::shield},
            {"shipping-status", &Ui5Icons::shipping_status},
            {"source-code", &Ui5Icons::source_code},
            {"sys-monitor", &Ui5Icons::sys_monitor},
            {"upload", &Ui5Icons::upload},
            {"user-settings", &Ui5Icons::user_settings},
            {"world", &Ui5Icons::world},
        };
        const auto found = icons.find(icon);
        return found != icons.end() ? found->second() : Ui5Icons::folder();
    }

    std::string firstLine(std::string const& command)
    {
        const auto end = command.find('\n');
        return end == std::string::npos ? command : command.substr(0, end) + " …";
    }
}

struct SnippetPresetsDialog::Implementation
{
    std::string id;
    CommandStoreClient* client;
    SnippetImportDialog* importDialog;
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog{};

    std::vector<Preset> presets{};
    /** @brief Presets being added; their buttons stay disabled until the folders reload. */
    std::set<std::string> adding{};
    /** @brief Bumped on open and while adding; redraws the cards. */
    Nui::Observed<int> revision{0};
    /** @brief The cards are only built while open, store changes cost nothing otherwise. */
    bool isOpen{false};

    Implementation(std::string id, CommandStoreClient* client, SnippetImportDialog* importDialog)
        : id{std::move(id)}
        , client{client}
        , importDialog{importDialog}
    {}

    void redraw()
    {
        revision = revision.value() + 1;
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    std::optional<std::string> existingFolderId(Preset const& preset) const
    {
        auto const& folders = client->folders().value();
        const auto folder = std::ranges::find_if(
            folders,
            [&preset](CommandStore::SnippetFolder const& candidate)
            {
                return candidate.name == preset.name;
            }
        );
        if (folder == folders.end())
            return std::nullopt;
        return folder->id;
    }

    /**
     * @brief How many preset snippets an import into the existing folder would add or change.
     */
    std::size_t differences(Preset const& preset, std::string const& folderId) const
    {
        const auto plan = CommandStore::SnippetImport::plan(
            preset.snippets,
            client->snippets().value(),
            client->folders().value(),
            CommandStore::SnippetImport::Target{.folderId = folderId}
        );
        return static_cast<std::size_t>(std::ranges::count_if(
            plan.snippets,
            [](CommandStore::SnippetImport::PlannedSnippet const& planned)
            {
                return planned.status != CommandStore::SnippetImport::Status::Identical;
            }
        ));
    }

    void add(Preset const& preset)
    {
        if (adding.contains(preset.name))
            return;
        const auto plan = CommandStore::SnippetImport::plan(
            preset.snippets,
            client->snippets().value(),
            client->folders().value(),
            CommandStore::SnippetImport::Target{}
        );
        auto decided = CommandStore::SnippetImport::decide(
            plan,
            [](std::size_t)
            {
                return CommandStore::SnippetImport::Resolution::Skip;
            }
        );
        adding.insert(preset.name);
        redraw();
        client->importSnippets(
            std::move(decided.entries),
            [this, name = preset.name](CommandStore::ImportSummary const& summary)
            {
                adding.erase(name);
                redraw();
                Notifications::success(
                    fmt::format(fmt::runtime(language->get("snippetPresetsDialog", "added")), name, summary.added)
                );
            },
            [this, name = preset.name]()
            {
                adding.erase(name);
                redraw();
            }
        );
    }

    void review(Preset const& preset, std::string const& folderId)
    {
        dialog->close();
        importDialog->open({
            .target = CommandStore::SnippetImport::Target{.folderId = folderId},
            .text = CommandStore::SnippetTransfer::toJson(preset.snippets).dump(4),
            .startAtReview = true,
            .source = SnippetImportDialog::Source::Preset,
        });
    }

    Nui::ElementRenderer card(Preset const& preset);
    Nui::ElementRenderer body();
};

Nui::ElementRenderer SnippetPresetsDialog::Implementation::card(Preset const& preset)
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Attributes::title;

    const auto folderId = existingFolderId(preset);
    const auto count = preset.snippets.size();

    std::vector<Nui::ElementRenderer> samples{};
    for (std::size_t index = 0; index < std::min<std::size_t>(3, count); ++index)
        samples.push_back(div{class_ = "snippet-presets-dialog-sample"}(firstLine(preset.snippets[index].command)));

    std::string status{};
    if (folderId)
    {
        const auto changes = differences(preset, *folderId);
        status = changes == 0 ? language->get("snippetPresetsDialog", "upToDate")
            : changes == 1 ? language->get("snippetPresetsDialog", "changesOne")
                           : fmt::format(fmt::runtime(language->get("snippetPresetsDialog", "changesMany")), changes);
    }

    auto action = folderId ? ScriptNuiComponents::button({
                                 .text = language->get("snippetPresetsDialog", "reviewButton"),
                                 .attributes =
                                     {
                                         title = language->get("snippetPresetsDialog", "reviewTooltip"),
                                         onClick =
                                             [this, &preset, id = *folderId](Nui::val)
                                         {
                                             review(preset, id);
                                         },
                                     },
                             })
                           : ScriptNuiComponents::button({
                                 .text = language->get("snippetPresetsDialog", "addButton"),
                                 .attributes =
                                     {
                                         "disabled"_prop = adding.contains(preset.name),
                                         onClick =
                                             [this, &preset](Nui::val)
                                         {
                                             add(preset);
                                         },
                                     },
                                 .styleVariant = ScriptNuiComponents::StyleVariant::Primary,
                             });

    // clang-format off
    return div{class_ = folderId ? "snippet-presets-dialog-card existing" : "snippet-presets-dialog-card"}(
        div{class_ = "snippet-presets-dialog-card-head"}(
            span{class_ = "snippet-presets-dialog-icon"}(presetIcon(preset.icon)),
            span{class_ = "snippet-presets-dialog-name"}(preset.name),
            folderId
                ? Nui::ElementRenderer{span{class_ = "snippet-presets-dialog-badge"}(language->get("snippetPresetsDialog", "inCollection"))}
                : Nui::nil()
        ),
        div{class_ = "snippet-presets-dialog-description"}(preset.description(language->currentLanguage())),
        div{class_ = "snippet-presets-dialog-samples"}(
            Nui::range(std::move(samples)),
            [](long long, auto const& sample) -> Nui::ElementRenderer {
                return sample;
            }
        ),
        div{class_ = "snippet-presets-dialog-card-foot"}(
            span{class_ = "snippet-presets-dialog-count"}(
                count == 1 ? language->get("snippetPresetsDialog", "snippetsOne")
                           : fmt::format(fmt::runtime(language->get("snippetPresetsDialog", "snippetsMany")), count)
            ),
            span{class_ = "snippet-presets-dialog-status"}(status),
            std::move(action)
        )
    );
    // clang-format on
}

Nui::ElementRenderer SnippetPresetsDialog::Implementation::body()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return section{class_ = "snippet-presets-dialog"}(
        div{class_ = "snippet-presets-dialog-intro"}(
            Nui::observe(revision).generate([]() -> std::string {
                return language->get("snippetPresetsDialog", "intro");
            })
        ),
        div{class_ = "snippet-presets-dialog-grid-host"}(
            Nui::observe(revision, client->folders(), client->snippets()).generate([this]() -> Nui::ElementRenderer {
                if (!isOpen)
                    return Nui::nil();
                std::vector<Nui::ElementRenderer> cards{};
                cards.reserve(presets.size());
                for (auto const& preset : presets)
                    cards.push_back(card(preset));
                return div{class_ = "snippet-presets-dialog-grid"}(
                    Nui::range(std::move(cards)),
                    [](long long, auto const& card) -> Nui::ElementRenderer {
                        return card;
                    }
                );
            })
        )
    );
    // clang-format on
}

SnippetPresetsDialog::SnippetPresetsDialog(
    std::string id,
    CommandStoreClient* client,
    SnippetImportDialog* importDialog
)
    : impl_{std::make_unique<Implementation>(std::move(id), client, importDialog)}
{
    impl_->presets = loadPresets();
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(impl_->id, impl_->body());
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(SnippetPresetsDialog);

Nui::ElementRenderer SnippetPresetsDialog::operator()()
{
    return (*impl_->dialog)();
}

void SnippetPresetsDialog::open()
{
    auto labels = localizedButtonLabels();
    labels.ok = language->get("snippetPresetsDialog", "closeButton");
    impl_->dialog->setButtonLabels(labels);
    impl_->adding.clear();
    impl_->isOpen = true;
    impl_->dialog->open({
        .headerText = language->get("snippetPresetsDialog", "title"),
        .buttons = Button::Ok,
        .onClose =
            [this](std::optional<Button>)
        {
            impl_->isOpen = false;
        },
        .modal = true,
        .mayCloseWithoutButton = true,
    });
    impl_->redraw();
}
