/**
 * Lightbox for carousel screenshots.
 *
 * Click a thumbnail and the image zooms from its in-grid position to a
 * near-viewport centered overlay; click anywhere on the overlay (or hit
 * Escape) to reverse the animation back into the slide.
 *
 * Implementation is FLIP: we clone the source <img>, position the clone
 * fixed at the source's bounding rect, hide the source with
 * `visibility:hidden` (keeps the carousel layout intact), then transition
 * the clone's left/top/width/height to the target rect.
 *
 * Exposed globally as `globalThis.nuiSftpLightboxOpen(imgElement)` so
 * Nui-side click handlers can fire it via emscripten::val.
 */
(function () {
    'use strict';

    // Tunables.
    var TRANSITION = 'left 380ms cubic-bezier(0.7, 0, 0.2, 1),' +
                     ' top 380ms cubic-bezier(0.7, 0, 0.2, 1),' +
                     ' width 380ms cubic-bezier(0.7, 0, 0.2, 1),' +
                     ' height 380ms cubic-bezier(0.7, 0, 0.2, 1),' +
                     ' border-radius 380ms ease,' +
                     ' box-shadow 380ms ease';
    var VIEWPORT_PADDING = 48;        // px breathing room around the enlarged image
    var TARGET_BORDER_RADIUS = '14px'; // when enlarged
    var TARGET_BOX_SHADOW = '0 40px 120px rgba(0, 0, 0, 0.65)';

    var active = null; // { clone, backdrop, original, originalRect, originalStyle, closing }

    function targetRect(srcRect) {
        var winW = window.innerWidth;
        var winH = window.innerHeight;
        var maxW = winW - VIEWPORT_PADDING * 2;
        var maxH = winH - VIEWPORT_PADDING * 2;
        var aspect = srcRect.width / srcRect.height;
        var w = maxW;
        var h = maxW / aspect;
        if (h > maxH) {
            h = maxH;
            w = maxH * aspect;
        }
        return {
            left: (winW - w) / 2,
            top: (winH - h) / 2,
            width: w,
            height: h,
        };
    }

    function close() {
        if (!active || active.closing) return;
        active.closing = true;

        var clone = active.clone;
        var backdrop = active.backdrop;
        var rect = active.originalRect;

        // Animate back to the source's original rect.
        clone.style.left = rect.left + 'px';
        clone.style.top = rect.top + 'px';
        clone.style.width = rect.width + 'px';
        clone.style.height = rect.height + 'px';
        clone.style.borderRadius = active.originalStyle.borderRadius;
        clone.style.boxShadow = active.originalStyle.boxShadow;
        backdrop.classList.remove('lightbox-backdrop--open');

        var captured = active;
        active = null;

        var done = function () {
            clone.removeEventListener('transitionend', done);
            if (clone.parentNode) clone.parentNode.removeChild(clone);
            if (backdrop.parentNode) backdrop.parentNode.removeChild(backdrop);
            captured.original.style.visibility = '';
        };
        clone.addEventListener('transitionend', done);
        // Fallback in case transitionend doesn't fire (e.g. tab hidden).
        setTimeout(done, 600);
    }

    function open(img) {
        if (!img || active) return;

        var rect = img.getBoundingClientRect();
        // Don't try to zoom invisible / unrendered images.
        if (rect.width === 0 || rect.height === 0) return;

        var cs = window.getComputedStyle(img);

        var backdrop = document.createElement('div');
        backdrop.className = 'lightbox-backdrop';
        backdrop.addEventListener('click', close);

        var clone = img.cloneNode(false);
        clone.className = 'lightbox-image';
        clone.draggable = false;
        clone.removeAttribute('id');
        clone.style.cssText = '';
        clone.style.position = 'fixed';
        clone.style.left = rect.left + 'px';
        clone.style.top = rect.top + 'px';
        clone.style.width = rect.width + 'px';
        clone.style.height = rect.height + 'px';
        clone.style.margin = '0';
        clone.style.borderRadius = cs.borderRadius;
        clone.style.boxShadow = cs.boxShadow;
        clone.style.objectFit = cs.objectFit;
        clone.style.zIndex = '1001';
        clone.style.cursor = 'zoom-out';
        clone.addEventListener('click', close);

        document.body.appendChild(backdrop);
        document.body.appendChild(clone);

        // Hide source while keeping its layout slot.
        img.style.visibility = 'hidden';

        active = {
            clone: clone,
            backdrop: backdrop,
            original: img,
            originalRect: rect,
            originalStyle: { borderRadius: cs.borderRadius, boxShadow: cs.boxShadow },
            closing: false,
        };

        // Force layout, then start the transition on the next frame so the
        // browser applies the initial position before we change it.
        void clone.offsetHeight;
        clone.style.transition = TRANSITION;

        var target = targetRect(rect);
        requestAnimationFrame(function () {
            clone.style.left = target.left + 'px';
            clone.style.top = target.top + 'px';
            clone.style.width = target.width + 'px';
            clone.style.height = target.height + 'px';
            clone.style.borderRadius = TARGET_BORDER_RADIUS;
            clone.style.boxShadow = TARGET_BOX_SHADOW;
            backdrop.classList.add('lightbox-backdrop--open');
        });
    }

    // Escape closes a currently open lightbox.
    document.addEventListener('keydown', function (e) {
        if (e.key === 'Escape') close();
    });

    globalThis.nuiSftpLightboxOpen = open;
    globalThis.nuiSftpLightboxClose = close;
})();
