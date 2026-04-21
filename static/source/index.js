// import './livereload.ts';

import { ContentPanelManager } from "./content_panel_manager.ts";
import "./addressable_setting.ts";

(() => {
    globalThis.generateId = () => {
        return crypto.randomUUID();
    }
    globalThis.contentPanelManager = new ContentPanelManager();
    const parseColorToRGBA = (colorString) => {
        const canvas = document.createElement('canvas');
        canvas.width = canvas.height = 1;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = colorString;
        ctx.fillRect(0, 0, 1, 1);
        const [red, green, blue, alpha] = ctx.getImageData(0, 0, 1, 1).data;
        return { r: red, g: green, b: blue, a: alpha };
    };
    globalThis.colorStringToRGBAObject = (colorString) => {
        return parseColorToRGBA(colorString);
    };

    globalThis.decodeUtf8Base64 = (base64) => {
        return new TextDecoder().decode(Uint8Array.fromBase64(base64));
    }
    globalThis.encodeUtf8Base64 = (str) => {
        return new TextEncoder().encode(str).toBase64();
    }

    const writeClipboard = (text, label) => {
        if (navigator && navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).catch((err) => {
                console.error(label + " clipboard write failed", err);
            });
            return;
        }
        console.error("navigator.clipboard unavailable; " + label + " not copied");
    };

    globalThis.writeTerminalDumpToClipboard = (text) => {
        writeClipboard(text, "terminal dump");
    };

    /* Strip ANSI escapes and control characters, preserve whitespace (\t \n \r).
     * Order matters: OSC with BEL terminator, OSC with ST terminator, CSI, then
     * the remaining two-char escapes, then C0/DEL. */
    globalThis.writeTerminalPlainToClipboard = (text) => {
        const stripped = text
            .replace(/\x1b\][^\x07\x1b]*\x07/g, "")
            .replace(/\x1b\][^\x1b]*\x1b\\/g, "")
            .replace(/\x1b\[[0-9;?]*[ -/]*[@-~]/g, "")
            .replace(/\x1b[@-Z\\-_]/g, "")
            .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g, "");
        writeClipboard(stripped, "terminal plain text");
    };
})();
