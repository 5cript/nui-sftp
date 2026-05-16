/**
 * Copy-to-clipboard for install command rows.
 *
 * On click, copies the given text and briefly swaps the row's `.copy`
 * label (e.g. "flathub") with a confirmation ("copied!"), restoring
 * after a short delay.
 *
 * Exposed globally as `globalThis.nuiSftpCopyToClipboard(rowEl, text)`
 * so Nui-side click handlers can fire it via emscripten::val.
 */
(function () {
    'use strict';

    var FEEDBACK_MS = 1200;

    function fallbackCopy(text) {
        var ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        ta.style.pointerEvents = 'none';
        document.body.appendChild(ta);
        ta.focus();
        ta.select();
        try { document.execCommand('copy'); } catch (_) { /* best-effort */ }
        document.body.removeChild(ta);
    }

    function showFeedback(rowEl) {
        if (!rowEl) return;
        var label = rowEl.querySelector('.copy');
        if (!label) return;
        // Don't stack feedbacks if the user mashes the row.
        if (label.dataset.original === undefined) {
            label.dataset.original = label.textContent;
        }
        label.textContent = 'copied!';
        rowEl.classList.add('copied');

        if (rowEl._copyTimer) clearTimeout(rowEl._copyTimer);
        rowEl._copyTimer = setTimeout(function () {
            label.textContent = label.dataset.original;
            delete label.dataset.original;
            rowEl.classList.remove('copied');
            rowEl._copyTimer = null;
        }, FEEDBACK_MS);
    }

    function copy(rowEl, text) {
        if (!text) return;
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).then(
                function () { showFeedback(rowEl); },
                function () { fallbackCopy(text); showFeedback(rowEl); }
            );
        } else {
            fallbackCopy(text);
            showFeedback(rowEl);
        }
    }

    globalThis.nuiSftpCopyToClipboard = copy;
})();
