// import './livereload.ts';

import { ContentPanelManager } from "./content_panel_manager.ts";

(() => {
    globalThis.generateId = () => {
        return crypto.randomUUID();
    }
    globalThis.contentPanelManager = new ContentPanelManager();
    globalThis.colorStringToRGBAObject = (colorString) => {
        const rgb = getRGBColor(colorString);
        const alpha = getAlpha(colorString);
        return { r: rgb.r, g: rgb.g, b: rgb.b, a: alpha * 255 };
    };

    globalThis.decodeUtf8Base64 = (base64) => {
        return new TextDecoder().decode(Uint8Array.fromBase64(base64));
    }
})();
