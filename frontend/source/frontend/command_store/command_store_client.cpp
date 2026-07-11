#include <frontend/command_store/command_store_client.hpp>

#include <log/log.hpp>

#include <nui/rpc.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string_view>
#include <utility>

using namespace CommandStore;

namespace
{
    std::int64_t nowEpochSeconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string sortOrderToString(SortOrder const sort)
    {
        switch (sort)
        {
            case SortOrder::MostRun:
                return "mostRun";
            case SortOrder::Name:
                return "name";
            case SortOrder::Recent:
                break;
        }
        return "recent";
    }

    /**
     * @brief The reason of a failed reply, or nothing when the reply reports success.
     */
    std::optional<std::string> failureReason(Nui::val const& response)
    {
        if (response.hasOwnProperty("error"))
            return response["error"].as<std::string>();
        if (!response.hasOwnProperty("success") || !response["success"].as<bool>())
            return std::string{"malformed reply"};
        return std::nullopt;
    }

    /**
     * @brief Position of the first element satisfying the predicate, searched on the raw container so
     *        that no modification is recorded.
     */
    std::optional<std::size_t> indexOf(auto const& container, auto const& predicate)
    {
        const auto found = std::ranges::find_if(container, predicate);
        if (found == container.end())
            return std::nullopt;
        return static_cast<std::size_t>(std::ranges::distance(container.begin(), found));
    }

    auto sameCommand(std::string const& host, std::string const& command)
    {
        return [&host, &command](HistoryEntry const& entry)
        {
            return entry.host == host && entry.command == command;
        };
    }

    auto hasId(std::int64_t const id)
    {
        return [id](HistoryEntry const& entry)
        {
            return entry.id == id;
        };
    }

    auto hasId(std::string const& id)
    {
        return [&id](auto const& element)
        {
            return element.id == id;
        };
    }

    std::int64_t asInt64(Nui::val const& value)
    {
        return static_cast<std::int64_t>(value.as<long long>());
    }

    HistoryEntry historyEntryFromVal(Nui::val const& value)
    {
        return HistoryEntry{
            .id = asInt64(value["id"]),
            .host = value["host"].as<std::string>(),
            .command = value["command"].as<std::string>(),
            .firstRun = asInt64(value["firstRun"]),
            .lastRun = asInt64(value["lastRun"]),
            .runs = asInt64(value["runs"]),
            .pinned = value["pinned"].as<bool>(),
            .favorite = value["favorite"].as<bool>(),
        };
    }

    Snippet snippetFromVal(Nui::val const& value)
    {
        Snippet snippet{
            .id = value["id"].as<std::string>(),
            .name = value["name"].as<std::string>(),
            .command = value["command"].as<std::string>(),
            .folder = value["folder"].as<std::string>(),
            .tags = {},
            .favorite = value["favorite"].as<bool>(),
            .uses = asInt64(value["uses"]),
            .lastUsed = asInt64(value["lastUsed"]),
        };

        const auto tags = value["tags"];
        const auto tagCount = tags["length"].as<long long>();
        snippet.tags.reserve(static_cast<std::size_t>(tagCount));
        for (long long index = 0; index < tagCount; ++index)
            snippet.tags.push_back(tags[static_cast<int>(index)].as<std::string>());

        return snippet;
    }

    SnippetFolder folderFromVal(Nui::val const& value)
    {
        return SnippetFolder{
            .id = value["id"].as<std::string>(),
            .name = value["name"].as<std::string>(),
            .icon = value["icon"].as<std::string>(),
            .position = asInt64(value["position"]),
        };
    }

    template <typename ElementT, typename ReaderT>
    std::vector<ElementT> readArray(Nui::val const& array, ReaderT const& reader)
    {
        const auto length = array["length"].as<long long>();
        std::vector<ElementT> elements{};
        elements.reserve(static_cast<std::size_t>(length));
        for (long long index = 0; index < length; ++index)
            elements.push_back(reader(array[static_cast<int>(index)]));
        return elements;
    }

    Nui::val toVal(Snippet const& snippet)
    {
        auto tags = Nui::val::array();
        for (std::size_t index = 0; index != snippet.tags.size(); ++index)
            tags.set(index, snippet.tags[index]);

        auto value = Nui::val::object();
        value.set("id", snippet.id);
        value.set("name", snippet.name);
        value.set("command", snippet.command);
        value.set("folder", snippet.folder);
        value.set("tags", tags);
        value.set("favorite", snippet.favorite);
        return value;
    }

    Nui::val toVal(SnippetFolder const& folder)
    {
        auto value = Nui::val::object();
        value.set("id", folder.id);
        value.set("name", folder.name);
        value.set("icon", folder.icon);
        value.set("position", static_cast<double>(folder.position));
        return value;
    }
}

struct CommandStoreClient::Implementation
{
    Nui::Observed<std::vector<HistoryEntry>> history{};
    Nui::Observed<std::vector<Snippet>> snippets{};
    Nui::Observed<std::vector<SnippetFolder>> folders{};
    HistoryQuery historyQuery{};
    std::function<void(std::string const&)> onError{};

    /**
     * @brief Whether the reply reports success; a failure is logged and reported to the user.
     */
    bool succeeded(Nui::val const& response, std::string_view const function)
    {
        const auto reason = failureReason(response);
        if (!reason)
            return true;

        Log::error("CommandStore: {} failed: {}", function, *reason);
        if (onError)
            onError(fmt::format("Command store: {} failed: {}", function, *reason));

        return false;
    }
};

CommandStoreClient::CommandStoreClient()
    : impl_{std::make_unique<Implementation>()}
{}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(CommandStoreClient);

void CommandStoreClient::setOnError(std::function<void(std::string const&)> onError)
{
    impl_->onError = std::move(onError);
}

Nui::Observed<std::vector<HistoryEntry>>& CommandStoreClient::history()
{
    return impl_->history;
}

Nui::Observed<std::vector<Snippet>>& CommandStoreClient::snippets()
{
    return impl_->snippets;
}

Nui::Observed<std::vector<SnippetFolder>>& CommandStoreClient::folders()
{
    return impl_->folders;
}

HistoryQuery const& CommandStoreClient::historyQuery() const
{
    return impl_->historyQuery;
}

void CommandStoreClient::recordExecution(std::string host, std::string command)
{
    // Optimistic: bump the local entry (or prepend a fresh one) so the panel reacts without a round
    // trip. The reply reconciles id, runs and firstRun with what the database actually stored; a
    // failed reply undoes the local change again.
    const auto existing = indexOf(impl_->history.value(), sameCommand(host, command));
    const auto previous = existing ? std::optional<HistoryEntry>{impl_->history.value()[*existing]} : std::nullopt;
    const auto now = nowEpochSeconds();

    if (existing)
    {
        auto entry = impl_->history[*existing];
        entry->runs += 1;
        entry->lastRun = now;
    }
    else
    {
        impl_->history.insert(
            impl_->history.begin(),
            HistoryEntry{
                .id = 0,
                .host = host,
                .command = command,
                .firstRun = now,
                .lastRun = now,
                .runs = 1,
                .pinned = false,
                .favorite = false,
            }
        );
    }
    impl_->history.eventContext().sync();

    auto parameters = Nui::val::object();
    parameters.set("host", host);
    parameters.set("command", command);

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::recordExecution",
        [this, host = std::move(host), command = std::move(command), previous](Nui::val response) {
            const auto current = indexOf(impl_->history.value(), sameCommand(host, command));
            if (!current)
                return;

            if (!impl_->succeeded(response, "recordExecution"))
            {
                if (previous)
                    impl_->history[*current] = *previous;
                else
                    impl_->history.erase(impl_->history.cbegin() + static_cast<std::ptrdiff_t>(*current));
            }
            else
            {
                impl_->history[*current] = historyEntryFromVal(response["entry"]);
            }
            impl_->history.eventContext().sync();
        },
        parameters
    );
}

void CommandStoreClient::reloadHistory(HistoryQuery query, std::function<void()> onLoaded)
{
    impl_->historyQuery = query;

    auto parameters = Nui::val::object();
    if (query.hostFilter)
        parameters.set("hostFilter", *query.hostFilter);
    parameters.set("sort", sortOrderToString(query.sort));
    if (query.limit)
        parameters.set("limit", static_cast<double>(*query.limit));

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::listHistory",
        [this, onLoaded = std::move(onLoaded)](Nui::val response) {
            if (!impl_->succeeded(response, "listHistory"))
                return;

            impl_->history = readArray<HistoryEntry>(response["entries"], historyEntryFromVal);
            impl_->history.eventContext().sync();

            if (onLoaded)
                onLoaded();
        },
        parameters
    );
}

void CommandStoreClient::reloadSnippets(std::function<void()> onLoaded)
{
    Nui::RpcClient::callWithBackChannel(
        "CommandStore::listSnippets",
        [this, onLoaded = std::move(onLoaded)](Nui::val response) {
            if (!impl_->succeeded(response, "listSnippets"))
                return;

            impl_->snippets = readArray<Snippet>(response["snippets"], snippetFromVal);
            impl_->snippets.eventContext().sync();

            if (onLoaded)
                onLoaded();
        },
        Nui::val::object()
    );
}

void CommandStoreClient::reloadFolders(std::function<void()> onLoaded)
{
    Nui::RpcClient::callWithBackChannel(
        "CommandStore::listFolders",
        [this, onLoaded = std::move(onLoaded)](Nui::val response) {
            if (!impl_->succeeded(response, "listFolders"))
                return;

            impl_->folders = readArray<SnippetFolder>(response["folders"], folderFromVal);
            impl_->folders.eventContext().sync();

            if (onLoaded)
                onLoaded();
        },
        Nui::val::object()
    );
}

void CommandStoreClient::setHistoryFlags(std::int64_t id, std::optional<bool> pinned, std::optional<bool> favorite)
{
    const auto existing = indexOf(impl_->history.value(), hasId(id));
    if (!existing)
    {
        Log::error("CommandStore: setHistoryFlags for unknown entry {}", id);
        return;
    }

    const auto previous = impl_->history.value()[*existing];
    auto entry = impl_->history[*existing];
    if (pinned)
        entry->pinned = *pinned;
    if (favorite)
        entry->favorite = *favorite;
    impl_->history.eventContext().sync();

    auto parameters = Nui::val::object();
    parameters.set("id", static_cast<double>(id));
    if (pinned)
        parameters.set("pinned", *pinned);
    if (favorite)
        parameters.set("favorite", *favorite);

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::setHistoryFlags",
        [this, id, previous](Nui::val response) {
            if (impl_->succeeded(response, "setHistoryFlags"))
                return;

            const auto current = indexOf(impl_->history.value(), hasId(id));
            if (!current)
                return;

            impl_->history[*current] = previous;
            impl_->history.eventContext().sync();
        },
        parameters
    );
}

void CommandStoreClient::deleteHistory(std::vector<std::int64_t> ids)
{
    // Back to front, so that the erased positions stay valid.
    for (std::size_t index = impl_->history.value().size(); index != 0; --index)
    {
        if (std::ranges::find(ids, impl_->history.value()[index - 1].id) == ids.end())
            continue;

        impl_->history.erase(impl_->history.cbegin() + static_cast<std::ptrdiff_t>(index - 1));
    }
    impl_->history.eventContext().sync();

    auto identifiers = Nui::val::array();
    for (std::size_t index = 0; index != ids.size(); ++index)
        identifiers.set(index, static_cast<double>(ids[index]));

    auto parameters = Nui::val::object();
    parameters.set("ids", identifiers);

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::deleteHistory",
        [this](Nui::val response) {
            // The removed entries are gone locally; only the database knows what actually survived.
            if (!impl_->succeeded(response, "deleteHistory"))
                reloadHistory(impl_->historyQuery);
        },
        parameters
    );
}

void CommandStoreClient::clearHistory()
{
    impl_->history.clear();
    impl_->history.eventContext().sync();

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::clearHistory",
        [this](Nui::val response) {
            if (!impl_->succeeded(response, "clearHistory"))
                reloadHistory(impl_->historyQuery);
        },
        Nui::val::object()
    );
}

void CommandStoreClient::upsertSnippet(Snippet snippet, std::function<void(Snippet const&)> onSaved)
{
    // No optimistic insert: a new snippet has no id yet, the database assigns it.
    auto parameters = Nui::val::object();
    parameters.set("snippet", toVal(snippet));

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::upsertSnippet",
        [this, onSaved = std::move(onSaved)](Nui::val response) {
            if (!impl_->succeeded(response, "upsertSnippet"))
                return;

            const auto stored = snippetFromVal(response["snippet"]);
            const auto existing = indexOf(impl_->snippets.value(), hasId(stored.id));
            if (existing)
                impl_->snippets[*existing] = stored;
            else
                impl_->snippets.push_back(stored);
            impl_->snippets.eventContext().sync();

            if (onSaved)
                onSaved(stored);
        },
        parameters
    );
}

void CommandStoreClient::deleteSnippet(std::string id)
{
    const auto existing = indexOf(impl_->snippets.value(), hasId(id));
    if (existing)
    {
        impl_->snippets.erase(impl_->snippets.cbegin() + static_cast<std::ptrdiff_t>(*existing));
        impl_->snippets.eventContext().sync();
    }

    auto parameters = Nui::val::object();
    parameters.set("id", id);

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::deleteSnippet",
        [this](Nui::val response) {
            if (!impl_->succeeded(response, "deleteSnippet"))
                reloadSnippets();
        },
        parameters
    );
}

void CommandStoreClient::bumpSnippetUse(std::string id)
{
    const auto existing = indexOf(impl_->snippets.value(), hasId(id));
    std::optional<Snippet> previous{};
    if (existing)
    {
        previous = impl_->snippets.value()[*existing];

        auto snippet = impl_->snippets[*existing];
        snippet->uses += 1;
        snippet->lastUsed = nowEpochSeconds();
        impl_->snippets.eventContext().sync();
    }

    auto parameters = Nui::val::object();
    parameters.set("id", id);

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::bumpSnippetUse",
        [this, id = std::move(id), previous = std::move(previous)](Nui::val response) {
            if (impl_->succeeded(response, "bumpSnippetUse") || !previous)
                return;

            const auto current = indexOf(impl_->snippets.value(), hasId(id));
            if (!current)
                return;

            impl_->snippets[*current] = *previous;
            impl_->snippets.eventContext().sync();
        },
        parameters
    );
}

void CommandStoreClient::upsertFolder(SnippetFolder folder, std::function<void(SnippetFolder const&)> onSaved)
{
    // No optimistic insert: a new folder has no id yet, the database assigns it.
    auto parameters = Nui::val::object();
    parameters.set("folder", toVal(folder));

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::upsertFolder",
        [this, onSaved = std::move(onSaved)](Nui::val response) {
            if (!impl_->succeeded(response, "upsertFolder"))
                return;

            const auto stored = folderFromVal(response["folder"]);
            const auto existing = indexOf(impl_->folders.value(), hasId(stored.id));
            if (existing)
                impl_->folders[*existing] = stored;
            else
                impl_->folders.push_back(stored);
            impl_->folders.eventContext().sync();

            if (onSaved)
                onSaved(stored);
        },
        parameters
    );
}

void CommandStoreClient::deleteFolder(std::string id)
{
    const auto existing = indexOf(impl_->folders.value(), hasId(id));
    if (existing)
    {
        impl_->folders.erase(impl_->folders.cbegin() + static_cast<std::ptrdiff_t>(*existing));
        impl_->folders.eventContext().sync();
    }

    // The backend moves the folder's snippets to the root instead of deleting them.
    for (std::size_t index = 0; index != impl_->snippets.value().size(); ++index)
    {
        if (impl_->snippets.value()[index].folder != id)
            continue;

        impl_->snippets[index]->folder.clear();
    }
    impl_->snippets.eventContext().sync();

    auto parameters = Nui::val::object();
    parameters.set("id", id);

    Nui::RpcClient::callWithBackChannel(
        "CommandStore::deleteFolder",
        [this](Nui::val response) {
            if (!impl_->succeeded(response, "deleteFolder"))
            {
                reloadFolders();
                reloadSnippets();
            }
        },
        parameters
    );
}
