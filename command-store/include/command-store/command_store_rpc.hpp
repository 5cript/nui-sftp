#pragma once

#include <backend/rpc_helper.hpp>
#include <command-store/command_store.hpp>

namespace CommandStore
{
    /**
     * @brief Exposes a Store over a Nui::RpcHub as "CommandStore::<method>" handlers.
     *
     * Shares the store's strand, so handlers and store operations form one serialization domain.
     * Replies follow the backend convention: {success: true, ...} or {error: "..."}.
     */
    class StoreRpc : public RpcHelper::StrandRpc
    {
      public:
        /**
         * @brief Registers all handlers on the hub; the store must outlive this object.
         */
        StoreRpc(boost::asio::any_io_executor executor, Nui::Window& wnd, Nui::RpcHub& hub, Store& store);

      private:
        void registerRecordExecution();
        void registerListHistory();
        void registerSetHistoryFlags();
        void registerDeleteHistory();
        void registerClearHistory();
        void registerListSnippets();
        void registerUpsertSnippet();
        void registerDeleteSnippet();
        void registerBumpSnippetUse();
        void registerListFolders();
        void registerUpsertFolder();
        void registerDeleteFolder();
        void registerImportSnippets();

      private:
        Store* store_;
    };

    /**
     * @brief Stands in for StoreRpc when the store could not be opened.
     *
     * Registers the same "CommandStore::<method>" handlers, each replying {error: reason}, so the
     * frontend can tell the user instead of waiting for a reply that never comes.
     */
    class UnavailableStoreRpc : public RpcHelper::StrandRpc
    {
      public:
        UnavailableStoreRpc(boost::asio::any_io_executor executor, Nui::Window& wnd, Nui::RpcHub& hub, std::string reason);
    };
}
