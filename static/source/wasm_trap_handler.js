/* Installed by debug frontends only. A WASM trap, or any uncaught error that unwound through WASM frames, leaves the
 * C++ state untrustworthy, so it is logged to the backend straight over RPC and the app quits. */
(() => {
    const errorLevel = 4;
    let handled = false;

    // Chromium and WebKit both name WASM frames "wasm-function[index]".
    const comesFromWasm = (error) =>
        error instanceof WebAssembly.RuntimeError ||
        (typeof error?.stack === "string" && error.stack.includes("wasm-function["));

    // WebKit stacks leave out the message.
    const describe = (error) => {
        const message = String(error);
        if (typeof error.stack !== "string")
            return message;
        return error.stack.startsWith(message) ? error.stack : `${message}\n${error.stack}`;
    };

    const callBackend = (name, ...args) => {
        const callable = globalThis.nui_rpc?.backend?.[name];
        if (typeof callable === "function")
            callable(...args);
    };

    const onUncaught = (error) => {
        if (handled || !comesFromWasm(error))
            return;
        handled = true;
        callBackend("log", errorLevel, `Uncaught WebAssembly error, quitting:\n${describe(error)}`);
        callBackend("Nui::terminate");
    };

    globalThis.installWasmTrapHandler = () => {
        // Emscripten glue takes several of the default 10 frames.
        Error.stackTraceLimit = 64;
        window.addEventListener("error", (event) => onUncaught(event.error));
        window.addEventListener("unhandledrejection", (event) => onUncaught(event.reason));
    };
})();
