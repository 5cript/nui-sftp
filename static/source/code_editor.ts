import * as monaco from "monaco-editor/esm/vs/editor/editor.api.js";
// Editor features (find, folding, formatting, context menu, ...), editor.api alone has none.
import "monaco-editor/esm/vs/features/register.all.js";
import "monaco-editor/esm/vs/languages/features/json/register.js";
// Tokenizers of the other languages, each loaded on first use.
import "monaco-editor/esm/vs/languages/definitions/register.all.js";

const themeName = "nui-sftp";

// Monaco's prebuilt, self-contained workers, copied next to the bundle by
// _cmake/copy_monaco_workers.cmake. Parcel's own bundling of the worker entries breaks them.
(globalThis as any).MonacoEnvironment = {
    getWorker(_workerId: string, label: string) {
        const worker = label === "json" ? "json" : "editor";
        return new Worker(new URL(`monaco/${worker}.worker.js`, document.baseURI), { name: label });
    },
};

/**
 * Resolves a CSS color expression (var(), color-mix(), oklch(), ...) to #rrggbbaa.
 */
const resolveColor = (() => {
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = 1;
    const context = canvas.getContext("2d", { willReadFrequently: true })!;

    return (expression: string, fallback: string): string => {
        const probe = document.createElement("span");
        probe.style.display = "none";
        probe.style.color = fallback;
        probe.style.color = expression;
        document.body.appendChild(probe);
        const computed = getComputedStyle(probe).color;
        probe.remove();

        context.clearRect(0, 0, 1, 1);
        context.fillStyle = fallback;
        context.fillStyle = computed;
        context.fillRect(0, 0, 1, 1);
        const [red, green, blue, alpha] = context.getImageData(0, 0, 1, 1).data;
        return "#" + [red, green, blue, alpha].map((channel) => channel.toString(16).padStart(2, "0")).join("");
    };
})();

const withAlpha = (color: string, alpha: number): string => {
    return color.substring(0, 7) + Math.round(alpha * 255).toString(16).padStart(2, "0");
};

const applyTheme = () => {
    const isLight = document.documentElement.dataset.theme === "light";
    const background = resolveColor("var(--background-color-fields)", isLight ? "#ffffff" : "#1e1e1e");
    const foreground = resolveColor("var(--color-fields)", isLight ? "#000000" : "#d4d4d4");
    const accent = resolveColor("var(--theme-color)", "#90d26d");
    const subdued = resolveColor("var(--subdued-text)", "#888888");
    const border = resolveColor("var(--border-color-fields)", "#505050");

    monaco.editor.defineTheme(themeName, {
        base: isLight ? "vs" : "vs-dark",
        inherit: true,
        rules: [],
        colors: {
            "editor.background": background,
            "editor.foreground": foreground,
            "editorGutter.background": background,
            "editorLineNumber.foreground": subdued,
            "editorLineNumber.activeForeground": foreground,
            "editorCursor.foreground": accent,
            "editor.selectionBackground": withAlpha(accent, 0.3),
            "editor.inactiveSelectionBackground": withAlpha(accent, 0.15),
            "editor.lineHighlightBorder": withAlpha(border, 0.5),
            "focusBorder": accent,
        },
    });
    monaco.editor.setTheme(themeName);
};

let themeRefreshScheduled = false;
const scheduleThemeRefresh = () => {
    if (themeRefreshScheduled)
        return;
    themeRefreshScheduled = true;
    requestAnimationFrame(() => {
        themeRefreshScheduled = false;
        applyTheme();
    });
};

new MutationObserver(scheduleThemeRefresh).observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["data-theme", "data-custom-theme"],
});

// A custom theme stylesheet applies its variables only once it loaded.
new MutationObserver((mutations) => {
    for (const mutation of mutations) {
        for (const node of mutation.addedNodes) {
            if (node instanceof HTMLLinkElement && node.rel === "stylesheet")
                node.addEventListener("load", scheduleThemeRefresh, { once: true });
        }
    }
}).observe(document.head, { childList: true });

let themeApplied = false;

(globalThis as any).codeEditor = {
    /**
     * Creates an editor inside host.
     */
    create(host: HTMLElement, options: { value?: string; language?: string; readOnly?: boolean }) {
        if (!themeApplied) {
            applyTheme();
            themeApplied = true;
        }
        return monaco.editor.create(host, {
            value: options.value ?? "",
            language: options.language ?? "plaintext",
            readOnly: options.readOnly ?? false,
            theme: themeName,
            automaticLayout: true,
            minimap: { enabled: false },
            scrollBeyondLastLine: false,
            // Overflow widgets on <body> would sit below a modal <dialog> in the top layer.
            fixedOverflowWidgets: false,
            formatOnPaste: true,
        });
    },

    setLanguage(editor: monaco.editor.IStandaloneCodeEditor, language: string) {
        const model = editor.getModel();
        if (model)
            monaco.editor.setModelLanguage(model, language);
    },

    /**
     * Disposes the editor and its model.
     */
    dispose(editor: monaco.editor.IStandaloneCodeEditor) {
        const model = editor.getModel();
        editor.dispose();
        model?.dispose();
    },

    applyTheme,
};
