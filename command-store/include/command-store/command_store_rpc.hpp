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

      private:
        Store* store_;
    };
}
