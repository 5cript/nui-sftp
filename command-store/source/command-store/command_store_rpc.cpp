#include <command-store/command_store_rpc.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace CommandStore
{
    namespace
    {
        std::int64_t nowEpochSeconds()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()
            )
                .count();
        }

        SortOrder sortOrderFromString(std::string const& sort)
        {
            if (sort == "mostRun")
                return SortOrder::MostRun;
            if (sort == "name")
                return SortOrder::Name;
            return SortOrder::Recent;
        }

        nlohmann::json toJson(HistoryEntry const& entry)
        {
            return {
                {"id", entry.id},
                {"host", entry.host},
                {"command", entry.command},
                {"firstRun", entry.firstRun},
                {"lastRun", entry.lastRun},
                {"runs", entry.runs},
                {"pinned", entry.pinned},
                {"favorite", entry.favorite},
            };
        }

        nlohmann::json toJson(Snippet const& snippet)
        {
            return {
                {"id", snippet.id},
                {"name", snippet.name},
                {"command", snippet.command},
                {"folder", snippet.folder},
                {"tags", snippet.tags},
                {"favorite", snippet.favorite},
                {"uses", snippet.uses},
                {"lastUsed", snippet.lastUsed},
            };
        }

        nlohmann::json toJson(SnippetFolder const& folder)
        {
            return {
                {"id", folder.id},
                {"name", folder.name},
                {"icon", folder.icon},
                {"position", folder.position},
            };
        }

        Snippet snippetFromJson(nlohmann::json const& json)
        {
            Snippet snippet{};
            snippet.id = json.value("id", std::string{});
            snippet.name = json.value("name", std::string{});
            snippet.command = json.value("command", std::string{});
            snippet.folder = json.value("folder", std::string{});
            if (json.contains("tags") && json["tags"].is_array())
                snippet.tags = json["tags"].get<std::vector<std::string>>();
            snippet.favorite = json.value("favorite", false);
            snippet.uses = json.value("uses", std::int64_t{0});
            snippet.lastUsed = json.value("lastUsed", std::int64_t{0});
            return snippet;
        }

        SnippetFolder folderFromJson(nlohmann::json const& json)
        {
            SnippetFolder folder{};
            folder.id = json.value("id", std::string{});
            folder.name = json.value("name", std::string{});
            folder.icon = json.value("icon", std::string{});
            folder.position = json.value("position", std::int64_t{0});
            return folder;
        }

        /**
         * @brief Wraps the move-only RpcOnce for capture in the store's copyable callbacks.
         */
        std::shared_ptr<RpcHelper::RpcOnce> shareReply(RpcHelper::RpcOnce&& reply)
        {
            return std::make_shared<RpcHelper::RpcOnce>(std::move(reply));
        }

        /**
         * @brief Completion callback replying {success: true} or the error.
         */
        std::function<void(Result<void>)> replySuccess(std::shared_ptr<RpcHelper::RpcOnce> reply)
        {
            return [reply = std::move(reply)](Result<void> result)
            {
                if (!result)
                    return reply->error(result.error().message);
                (*reply)({{"success", true}});
            };
        }
    }

    StoreRpc::StoreRpc(boost::asio::any_io_executor executor, Nui::Window& wnd, Nui::RpcHub& hub, Store& store)
        : RpcHelper::StrandRpc{executor, store.strand(), wnd, hub}
        , store_{&store}
    {
        registerRecordExecution();
        registerListHistory();
        registerSetHistoryFlags();
        registerDeleteHistory();
        registerClearHistory();
        registerListSnippets();
        registerUpsertSnippet();
        registerDeleteSnippet();
        registerBumpSnippetUse();
        registerListFolders();
        registerUpsertFolder();
        registerDeleteFolder();
    }

    void StoreRpc::registerRecordExecution()
    {
        on("CommandStore::recordExecution")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::recordExecution", parameters}
                             .hasValueDeep("host"))
                        return;
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::recordExecution", parameters}
                             .hasValueDeep("command"))
                        return;

                    store_->recordExecution(
                        parameters["host"].get<std::string>(),
                        parameters["command"].get<std::string>(),
                        nowEpochSeconds(),
                        [reply = shareReply(std::move(reply))](Result<HistoryEntry> result)
                        {
                            if (!result)
                                return reply->error(result.error().message);
                            (*reply)({{"success", true}, {"entry", toJson(*result)}});
                        }
                    );
                }
            );
    }

    void StoreRpc::registerListHistory()
    {
        on("CommandStore::listHistory")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    HistoryQuery query{};
                    if (parameters.contains("hostFilter"))
                        query.hostFilter = parameters["hostFilter"].get<std::string>();
                    query.sort = sortOrderFromString(parameters.value("sort", std::string{"recent"}));
                    if (parameters.contains("limit"))
                        query.limit = parameters["limit"].get<std::int64_t>();

                    store_->listHistory(
                        std::move(query),
                        [reply = shareReply(std::move(reply))](Result<std::vector<HistoryEntry>> result)
                        {
                            if (!result)
                                return reply->error(result.error().message);
                            auto entries = nlohmann::json::array();
                            for (auto const& entry : *result)
                                entries.push_back(toJson(entry));
                            (*reply)({{"success", true}, {"entries", std::move(entries)}});
                        }
                    );
                }
            );
    }

    void StoreRpc::registerSetHistoryFlags()
    {
        on("CommandStore::setHistoryFlags")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::setHistoryFlags", parameters}
                             .hasValueDeep("id"))
                        return;

                    std::optional<bool> pinned{};
                    if (parameters.contains("pinned"))
                        pinned = parameters["pinned"].get<bool>();
                    std::optional<bool> favorite{};
                    if (parameters.contains("favorite"))
                        favorite = parameters["favorite"].get<bool>();

                    store_->setHistoryFlags(
                        parameters["id"].get<std::int64_t>(),
                        pinned,
                        favorite,
                        replySuccess(shareReply(std::move(reply)))
                    );
                }
            );
    }

    void StoreRpc::registerDeleteHistory()
    {
        on("CommandStore::deleteHistory")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::deleteHistory", parameters}.hasValueDeep(
                            "ids"
                        ))
                        return;

                    store_->deleteHistory(
                        parameters["ids"].get<std::vector<std::int64_t>>(),
                        replySuccess(shareReply(std::move(reply)))
                    );
                }
            );
    }

    void StoreRpc::registerClearHistory()
    {
        on("CommandStore::clearHistory")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const&)
                {
                    store_->clearHistory(replySuccess(shareReply(std::move(reply))));
                }
            );
    }

    void StoreRpc::registerListSnippets()
    {
        on("CommandStore::listSnippets")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const&)
                {
                    store_->listSnippets(
                        [reply = shareReply(std::move(reply))](Result<std::vector<Snippet>> result)
                        {
                            if (!result)
                                return reply->error(result.error().message);
                            auto snippets = nlohmann::json::array();
                            for (auto const& snippet : *result)
                                snippets.push_back(toJson(snippet));
                            (*reply)({{"success", true}, {"snippets", std::move(snippets)}});
                        }
                    );
                }
            );
    }

    void StoreRpc::registerUpsertSnippet()
    {
        on("CommandStore::upsertSnippet")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::upsertSnippet", parameters}.hasValueDeep(
                            "snippet"
                        ))
                        return;

                    store_->upsertSnippet(
                        snippetFromJson(parameters["snippet"]),
                        [reply = shareReply(std::move(reply))](Result<Snippet> result)
                        {
                            if (!result)
                                return reply->error(result.error().message);
                            (*reply)({{"success", true}, {"snippet", toJson(*result)}});
                        }
                    );
                }
            );
    }

    void StoreRpc::registerDeleteSnippet()
    {
        on("CommandStore::deleteSnippet")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::deleteSnippet", parameters}.hasValueDeep(
                            "id"
                        ))
                        return;

                    store_->deleteSnippet(
                        parameters["id"].get<std::string>(),
                        replySuccess(shareReply(std::move(reply)))
                    );
                }
            );
    }

    void StoreRpc::registerBumpSnippetUse()
    {
        on("CommandStore::bumpSnippetUse")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::bumpSnippetUse", parameters}
                             .hasValueDeep("id"))
                        return;

                    store_->bumpSnippetUse(
                        parameters["id"].get<std::string>(),
                        nowEpochSeconds(),
                        replySuccess(shareReply(std::move(reply)))
                    );
                }
            );
    }

    void StoreRpc::registerListFolders()
    {
        on("CommandStore::listFolders")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const&)
                {
                    store_->listFolders(
                        [reply = shareReply(std::move(reply))](Result<std::vector<SnippetFolder>> result)
                        {
                            if (!result)
                                return reply->error(result.error().message);
                            auto folders = nlohmann::json::array();
                            for (auto const& folder : *result)
                                folders.push_back(toJson(folder));
                            (*reply)({{"success", true}, {"folders", std::move(folders)}});
                        }
                    );
                }
            );
    }

    void StoreRpc::registerUpsertFolder()
    {
        on("CommandStore::upsertFolder")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::upsertFolder", parameters}.hasValueDeep(
                            "folder"
                        ))
                        return;

                    store_->upsertFolder(
                        folderFromJson(parameters["folder"]),
                        [reply = shareReply(std::move(reply))](Result<SnippetFolder> result)
                        {
                            if (!result)
                                return reply->error(result.error().message);
                            (*reply)({{"success", true}, {"folder", toJson(*result)}});
                        }
                    );
                }
            );
    }

    void StoreRpc::registerDeleteFolder()
    {
        on("CommandStore::deleteFolder")
            .perform(
                [this](RpcHelper::RpcOnce&& reply, nlohmann::json const& parameters)
                {
                    if (!RpcHelper::ParameterVerifyView{reply, "CommandStore::deleteFolder", parameters}.hasValueDeep(
                            "id"
                        ))
                        return;

                    store_->deleteFolder(
                        parameters["id"].get<std::string>(),
                        replySuccess(shareReply(std::move(reply)))
                    );
                }
            );
    }
}
