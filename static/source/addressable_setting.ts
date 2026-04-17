/*
 * Support for `addressableSetting(htmlId, ...)` — C++ side calls these
 * helpers when resolving a `FrontendEvents::requestOpenSettingsAtId` request.
 *
 * Keep this file pure DOM / browser-API — no framework imports. Attached to
 * `globalThis.addressableSettings` so C++ can invoke via Nui::val::global.
 */

interface AddressableSettingsApi {
    /**
     * Walk up from a starting element and click the `.settings-group-header`
     * of any ancestor `.settings-group-content` that is in the `.collapsed`
     * state. The click handler on the header flips the C++-side observed
     * boolean, which animates the group open.
     */
    expandCollapsedGroupsContaining(startId: string): void;

    /**
     * Scroll the addressable setting identified by `htmlId` into view.
     * Because `.addressable-setting` is `display: contents` (box-less), the
     * scroll target is its first element child — the real setting row.
     * Highlight class is applied to the wrapper for ~1.5s so the selector
     * `.addressable-setting--highlight > div` can paint the pulse.
     */
    scrollToAndHighlight(htmlId: string): void;
}

(() => {
    const api: AddressableSettingsApi = {
        expandCollapsedGroupsContaining(startId: string) {
            const start = document.getElementById(startId);
            if (!start) return;

            let cursor: HTMLElement | null = start;
            while (cursor && cursor !== document.body) {
                if (cursor.classList
                    && cursor.classList.contains('settings-group-content')
                    && cursor.classList.contains('collapsed'))
                {
                    const header = cursor.previousElementSibling as HTMLElement | null;
                    if (header && header.classList.contains('settings-group-header')) {
                        header.click();
                    }
                }
                cursor = cursor.parentElement;
            }
        },

        scrollToAndHighlight(htmlId: string) {
            const wrapper = document.getElementById(htmlId);
            if (!wrapper) return;

            const scrollTarget = (wrapper.firstElementChild as HTMLElement) ?? wrapper;
            scrollTarget.scrollIntoView({ behavior: 'smooth', block: 'center' });

            wrapper.classList.add('addressable-setting--highlight');
            setTimeout(() => {
                const stillThere = document.getElementById(htmlId);
                if (stillThere) stillThere.classList.remove('addressable-setting--highlight');
            }, 1500);
        },
    };

    (globalThis as any).addressableSettings = api;
})();