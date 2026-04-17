// Emscripten's generated JS is loaded as a module (<script type="module">), so `var` bindings here
// are module-scoped and don't automatically see a Module object set on globalThis. Pull in any hooks
// pre-configured by the host page (e.g. the progress-bar-aware instantiateWasm in static/index.html)
// and chain onRuntimeInitialized so both the host hook and the required Module.main() run.
var Module = Object.assign({}, globalThis.Module || {});
var __prevOnRuntimeInitialized = Module.onRuntimeInitialized;
Module.onRuntimeInitialized = () => {
    if (typeof __prevOnRuntimeInitialized === "function")
        __prevOnRuntimeInitialized();
    Module.main();
};