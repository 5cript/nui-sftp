// import './livereload.ts';

import "@ui5/webcomponents/dist/Assets.js";
import "@ui5/webcomponents-fiori/dist/Assets.js";
import "@ui5/webcomponents-base/dist/Device.js";
import "@ui5/webcomponents/dist/features/InputSuggestions.js";
import { setTheme } from "@ui5/webcomponents-base/dist/config/Theme.js";
import { setThemeRoot } from "@ui5/webcomponents-base/dist/config/ThemeRoot.js";
import { getRGBColor, getAlpha } from "@ui5/webcomponents-base/dist/util/ColorConversion.js";

import { ContentPanelManager } from "./content_panel_manager.ts";
import "./icons.js";

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

    // app.example is intentional, because of windows dns timeouts
    setThemeRoot("nui://app.example/");
    setTheme("dark");
})();
