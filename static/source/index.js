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
})();
