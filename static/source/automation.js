/* Hooks for driving the app over CDP or the WebKit inspector. The frontend registers
 * nui_rpc.frontend["Automation::connect"] only when dev tools are enabled. */
(() => {
    const pending = new Map();

    globalThis.nuiAutomation = {
        /**
         * Opens the saved session `name` in a new tab.
         * Resolves {success, info} once it opened or failed, also for an unknown name.
         * Rejects when the app was not started with dev tools.
         */
        connect(name, userIndex = 0) {
            const connectFunction = globalThis.nui_rpc?.frontend?.["Automation::connect"];
            if (typeof connectFunction !== "function") {
                return Promise.reject(
                    new Error("nuiAutomation is disabled, start nui-sftp with --enable-dev-tools")
                );
            }
            const id = crypto.randomUUID();
            return new Promise((resolve) => {
                pending.set(id, resolve);
                connectFunction({ id, name, userIndex });
            });
        },

        settle(id, success, info) {
            const resolve = pending.get(id);
            if (!resolve)
                return;
            pending.delete(id);
            resolve({ success, info });
        },
    };
})();
