// Page-side helpers for drive.mjs. Evaluated once in the nui-sftp page through the DevTools protocol, then called as
// globalThis.screenshotDriver.<function>(...). Selectors follow .claude/skills/nui-sftp-dev/references/ui-map.md and
// match the English UI strings.
globalThis.screenshotDriver = (() => {
    const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

    const frames = () => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));

    const textOf = (element) => element?.textContent.trim() ?? '';

    const isShown = (element) =>
        !!element && getComputedStyle(element).display !== 'none' && element.getClientRects().length > 0;

    const waitFor = async (description, predicate, timeoutMilliseconds = 20000) => {
        const deadline = performance.now() + timeoutMilliseconds;
        for (;;) {
            const value = predicate();
            if (value)
                return value;
            const failure = openDialogText();
            if (failure)
                throw new Error(`waiting for ${description}, a dialog opened instead: ${failure}`);
            if (performance.now() > deadline)
                throw new Error(`timed out waiting for ${description}`);
            await sleep(100);
        }
    };

    const expectedDialogs = new Set(['ArchiveTransferDialog', 'SnippetPresetsDialog']);

    const openDialogText = () => {
        const dialog = [...document.querySelectorAll('dialog[open]')].find((each) => !expectedDialogs.has(each.id));
        return dialog ? `${dialog.id}: ${textOf(dialog).slice(0, 400)}` : null;
    };

    const findByText = (selector, text, root = document) =>
        [...root.querySelectorAll(selector)].find((element) => textOf(element) === text);

    const clickCenter = (element, type, extra = {}) => {
        const rectangle = element.getBoundingClientRect();
        element.dispatchEvent(
            new MouseEvent(type, {
                bubbles: true,
                cancelable: true,
                button: type === 'contextmenu' ? 2 : 0,
                clientX: rectangle.left + rectangle.width / 2,
                clientY: rectangle.top + rectangle.height / 2,
                ...extra,
            })
        );
    };

    const closePopovers = () => {
        for (const popover of document.querySelectorAll(':popover-open'))
            popover.hidePopover();
    };

    const closeToasts = () => {
        for (const close of document.querySelectorAll('.snc-toast-close'))
            close.click();
    };

    /**
     * @brief Removes transient UI (toasts, open popovers) and waits until the page has painted.
     */
    const prepareCapture = async () => {
        const failure = openDialogText();
        if (failure)
            throw new Error(`not capturing, a dialog is open: ${failure}`);
        closePopovers();
        closeToasts();
        await sleep(400);
        closeToasts();
        await frames();
        return true;
    };

    const toolbarSelect = (label) =>
        [...document.querySelectorAll('.toolbar > button.script-nui-select')].find(
            (button) => textOf(button.querySelector('.script-nui-select-label')) === label
        );

    const pickOption = async (label, optionText) => {
        const select = toolbarSelect(label);
        if (!select)
            throw new Error(`no toolbar select labelled ${label}`);
        select.click();
        await frames();
        const options = [...select.querySelectorAll(':scope > .script-nui-select-popover > div[data-index]')];
        const option = options.find((each) => textOf(each) === optionText);
        if (!option)
            throw new Error(`${label} has no option ${optionText}, it has: ${options.map(textOf).join(', ')}`);
        option.click();
        await frames();
        closePopovers();
    };

    const sessionTabs = () =>
        [...document.querySelectorAll('.script-nui-tab-bar[data-class="session-area-tabs"] .script-nui-tab')];

    const tabTitle = (tab) => textOf(tab.querySelector('span'));

    const visibleSession = () => document.querySelector('.session-area-content > .terminal-session.terminal-session-visible');

    const visibleDock = () => {
        const element = visibleSession()?.querySelector('[id^="dock_"]');
        return element ? contentPanelManager.getDockPanelById(element.id.slice(5)) : undefined;
    };

    const dockWidget = (layoutId) => {
        const dock = visibleDock();
        if (!dock)
            throw new Error('the visible session has no dock');
        const widget = Array.from(dock.widgets()).find((each) => each.layoutId === layoutId);
        if (!widget)
            throw new Error(`the dock has no ${layoutId} panel`);
        return widget;
    };

    /**
     * @brief Brings a dock panel (terminal, file-explorer, command-snippets, ...) to the front of its tab area.
     */
    const activatePanel = async (layoutId) => {
        visibleDock().activateWidget(dockWidget(layoutId));
        await frames();
        return true;
    };

    const selectSessionTab = async (title) => {
        const tab = sessionTabs().find((each) => tabTitle(each) === title);
        if (!tab)
            return false;
        if (!tab.classList.contains('selected'))
            tab.click();
        await waitFor(`session tab ${title}`, () => tab.classList.contains('selected') && visibleDock());
        return true;
    };

    const fileSides = () => [...(visibleSession()?.querySelectorAll('.nui-file-grid-side') ?? [])];

    const fileSide = (index) => {
        const side = fileSides()[index];
        if (!side)
            throw new Error(`the visible session has no file explorer side ${index}`);
        return side;
    };

    const pathInput = (side) => side.querySelector('.nui-file-grid-path-bar input.script-nui-text-input');

    const sideItems = (side) => [
        ...side.querySelectorAll('.nui-file-grid-item-icons[data-index], .nui-file-grid-table-row[data-index]'),
    ];

    const itemName = (item) =>
        item.classList.contains('nui-file-grid-table-row')
            ? textOf(item.querySelector('.nui-file-grid-table-cell span'))
            : textOf(item.lastElementChild);

    const findItem = (index, name) => sideItems(fileSide(index)).find((item) => itemName(item) === name);

    const sideIdle = (side) => !side.querySelector('.nui-file-grid-loading-placeholder.visible');

    const terminalOf = (layoutId) => {
        const channel = dockWidget(layoutId).node.querySelector('.terminal-channel[data-channelid]');
        if (!channel)
            return null;
        return (
            [...terminalUtility.terminals.values()].find((each) => each.terminal.element?.parentElement === channel)
                ?.terminal ?? null
        );
    };

    const terminalText = (terminal) => {
        const buffer = terminal.buffer.active;
        const lines = [];
        for (let index = 0; index < buffer.length; ++index)
            lines.push(buffer.getLine(index)?.translateToString(true) ?? '');
        return lines.join('\n').trimEnd();
    };

    /**
     * @brief Waits until the terminal shows a prompt and its text stopped changing.
     */
    const waitTerminalQuiet = async (layoutId, quietMilliseconds = 900, timeoutMilliseconds = 30000) => {
        const terminal = await waitFor(`the ${layoutId} terminal`, () => terminalOf(layoutId));
        const deadline = performance.now() + timeoutMilliseconds;
        let last = terminalText(terminal);
        let changedAt = performance.now();
        for (;;) {
            await sleep(100);
            const now = terminalText(terminal);
            if (now !== last) {
                last = now;
                changedAt = performance.now();
            }
            const promptShown = /[$#>%] ?$/.test(now);
            if (promptShown && performance.now() - changedAt >= quietMilliseconds)
                return true;
            if (performance.now() > deadline)
                throw new Error(`the ${layoutId} terminal did not settle, it shows: ${now.slice(-300)}`);
        }
    };

    return {
        prepareCapture,

        /**
         * @brief Throws when the app shows a dialog nobody asked for (load warnings, failed sessions).
         */
        assertNoDialog: () => {
            const failure = openDialogText();
            if (failure)
                throw new Error(`unexpected dialog: ${failure}`);
            return true;
        },

        viewport: () => ({ width: innerWidth, height: innerHeight, scale: devicePixelRatio }),

        /**
         * @brief Selects the tab of a session or starts the session from the toolbar.
         * @param server The session name in the toolbar's Server select.
         * @param layout The layout name in the Layout select.
         * @param expectedTitle The tab title once connected (user@host:port for SSH, the name for shells).
         * @param remote Whether the session has a remote side to wait for.
         */
        openSession: async (server, layout, expectedTitle, remote) => {
            if (await selectSessionTab(expectedTitle))
                return 'selected';
            const tabCount = sessionTabs().length;
            await pickOption('Server', server);
            await pickOption('Layout', layout);
            const connect = findByText('.toolbar > button.script-nui-button', 'Connect');
            if (!connect)
                throw new Error('no Connect button in the toolbar');
            connect.click();
            await waitFor(`a new session tab for ${server}`, () => sessionTabs().length > tabCount);
            await waitFor(
                `the session tab to read ${expectedTitle}`,
                () => {
                    const selected = sessionTabs().find((each) => each.classList.contains('selected'));
                    return selected && tabTitle(selected) === expectedTitle;
                },
                45000
            );
            await waitFor('the session dock', () => visibleDock(), 30000);
            await waitFor('the terminal channel', () => terminalOf('terminal'), 30000);
            if (remote) {
                await waitFor(
                    'the remote file listing',
                    () => {
                        const side = fileSides()[1];
                        return side && pathInput(side)?.value && sideItems(side).length > 0 && sideIdle(side);
                    },
                    30000
                );
            }
            return 'opened';
        },

        activatePanel,

        /**
         * @brief Navigates a file explorer side (0 = local, 1 = remote) and waits for the listing.
         * @param expectedName An entry that has to show up, proves the new directory is listed.
         */
        navigate: async (index, path, expectedName) => {
            const side = fileSide(index);
            const input = pathInput(side);
            input.value = path;
            input.dispatchEvent(new KeyboardEvent('keyup', { key: 'Enter', bubbles: true }));
            await sleep(300);
            await waitFor(
                `${path} to be listed`,
                () => sideIdle(side) && (!expectedName || findItem(index, expectedName)),
                20000
            );
            await sleep(200);
            return true;
        },

        /**
         * @brief Selects an entry, with add = true it joins the selection like a Ctrl+click.
         */
        select: async (index, name, add = false) => {
            const item = findItem(index, name);
            if (!item)
                throw new Error(`no entry ${name} on side ${index}`);
            clickCenter(item, 'click', { ctrlKey: add });
            await frames();
            return true;
        },

        /**
         * @brief Right-clicks an entry and clicks a context menu item by its label.
         * @param confirm Answers Yes when the action asks for confirmation (downloads, uploads).
         */
        contextAction: async (index, name, label, confirm = false) => {
            const item = findItem(index, name);
            if (!item)
                throw new Error(`no entry ${name} on side ${index}`);
            const target = item.querySelector('.nui-file-grid-table-cell') ?? item;
            clickCenter(target, 'contextmenu');
            const menu = await waitFor('the context menu', () =>
                document.querySelector('.script-nui-popup-menu:not(.script-nui-popup-menu--measuring)')
            );
            const entries = [...menu.querySelectorAll('.script-nui-popup-menu__item')];
            const entry = entries.find((each) => textOf(each.querySelector('.script-nui-popup-menu__label')) === label);
            if (!entry || entry.className.includes('--disabled')) {
                const labels = entries.map((each) => textOf(each.querySelector('.script-nui-popup-menu__label')));
                throw new Error(`context menu item ${label} is missing or disabled, menu: ${labels.join(', ')}`);
            }
            entry.click();
            await frames();
            if (confirm) {
                const deadline = performance.now() + 3000;
                while (performance.now() < deadline) {
                    const yes = document.querySelector('dialog#ConfirmDialog[open] #ConfirmDialog_yes');
                    if (yes) {
                        yes.click();
                        await frames();
                        break;
                    }
                    await sleep(100);
                }
            }
            return true;
        },

        /**
         * @brief Closes dialogs, menus and overlays a failed scene left open, so the next scene starts clean.
         */
        resetUi: async () => {
            for (const dialog of document.querySelectorAll('dialog[open]')) {
                const button = dialog.querySelector('[id$="_cancel"], [id$="_no"], [id$="_ok"]');
                if (button)
                    button.click();
                else
                    dialog.close();
            }
            closePopovers();
            visibleSession()?.querySelector('.sync-dialog button[title="Close"]')?.click();
            if (isShown(document.querySelector('.settings-page-background-blocker')))
                document.querySelector('.settings-page-header > button.script-nui-button')?.click();
            closeToasts();
            await sleep(300);
            return openDialogText();
        },

        /**
         * @brief Waits until the queue has operations and all of them completed, throws when one did not succeed.
         */
        waitQueueDone: () =>
            waitFor(
                'the queued operations to complete',
                () => {
                    const cards = [...(visibleSession()?.querySelectorAll('.operation-queue .opq-card[data-op-id]') ?? [])];
                    const unsuccessful = cards.find((card) =>
                        ['opq-failed', 'opq-canceled', 'opq-partial-success'].some((state) => card.classList.contains(state))
                    );
                    if (unsuccessful)
                        throw new Error(`an operation did not succeed: ${textOf(unsuccessful).slice(0, 300)}`);
                    return cards.length > 0 && cards.every((card) => card.classList.contains('opq-completed'));
                },
                60000
            ),

        clearTerminal: async (layoutId) => {
            await waitTerminalQuiet(layoutId, 1500);
            terminalOf(layoutId).clear();
            await frames();
            return true;
        },

        /**
         * @brief Types a command line into a terminal like a user and waits for its output to settle.
         */
        typeCommand: async (layoutId, command) => {
            await waitTerminalQuiet(layoutId);
            terminalOf(layoutId).input(`${command}\r`, true);
            await sleep(300);
            await waitTerminalQuiet(layoutId);
            return true;
        },

        setColorMode: async (mode) => {
            const root = document.documentElement;
            if (root.dataset.theme !== mode) {
                document.querySelector('.toolbar-notifications').previousElementSibling.click();
                await waitFor(`the ${mode} theme`, () => root.dataset.theme === mode);
            }
            await sleep(300);
            return true;
        },

        waitSyncResult: async () => {
            const result = await waitFor(
                'the synchronize dialog result',
                () => {
                    const progress = document.querySelector('.sync-progress-dialog-blocker');
                    if (isShown(progress))
                        return null;
                    const blocker = visibleSession()?.querySelector('.sync-dialog-blocker:not(.sync-progress-dialog-blocker)');
                    if (!isShown(blocker))
                        return null;
                    const synchronize = [...blocker.querySelectorAll('.sync-dialog button')].find(
                        (button) => textOf(button) === 'Synchronize'
                    );
                    return synchronize && !synchronize.disabled ? blocker : null;
                },
                60000
            );
            return textOf(result.querySelector('.sync-footer-summary'));
        },

        closeSyncDialog: async () => {
            visibleSession().querySelector('.sync-dialog button[title="Close"]')?.click();
            await frames();
            return true;
        },

        waitArchiveDialog: async () => {
            await waitFor('the archive dialog', () => document.querySelector('dialog#ArchiveTransferDialog[open]'));
            document.activeElement?.blur();
            await frames();
            return true;
        },

        cancelArchiveDialog: async () => {
            document.querySelector('#ArchiveTransferDialog_cancel').click();
            await waitFor('the archive dialog to close', () => !document.querySelector('dialog#ArchiveTransferDialog[open]'));
            return true;
        },

        /**
         * @brief Adds ready-made snippet folders through the "More folders" dialog, one card at a time.
         */
        addPresetFolders: async (names) => {
            const more = dockWidget('command-snippets').node.querySelector('.cmds-more-folders');
            if (!more)
                throw new Error('no "More folders" button in the snippets panel');
            more.click();
            await waitFor('the preset folders dialog', () => document.querySelector('dialog#SnippetPresetsDialog[open]'));
            const cardOf = (name) =>
                [...document.querySelectorAll('dialog#SnippetPresetsDialog .snippet-presets-dialog-card')].find(
                    (card) => textOf(card.querySelector('.snippet-presets-dialog-name')) === name
                );
            for (const name of names) {
                const card = cardOf(name);
                if (!card)
                    throw new Error(`no preset folder named ${name}`);
                if (card.classList.contains('existing'))
                    continue;
                card.querySelector('.snippet-presets-dialog-card-foot button').click();
                await waitFor(`the ${name} preset to be added`, () => cardOf(name)?.classList.contains('existing'), 20000);
            }
            document.querySelector('#SnippetPresetsDialog_ok').click();
            await waitFor('the preset folders dialog to close', () => !document.querySelector('dialog#SnippetPresetsDialog[open]'));
            return true;
        },

        selectSnippetFolder: async (name) => {
            const panel = dockWidget('command-snippets').node;
            const folderOf = () =>
                [...panel.querySelectorAll('.cmds-folder')].find(
                    (each) => textOf(each.querySelector('.cmds-folder-name')) === name
                );
            const folder = folderOf();
            if (!folder)
                throw new Error(`no snippet folder ${name}`);
            folder.click();
            await waitFor(`the ${name} folder to be selected`, () => folderOf()?.classList.contains('cmds-folder-selected'));
            await frames();
            return true;
        },

        waitPanelText: (layoutId, texts) =>
            waitFor(
                `${texts.join(', ')} in the ${layoutId} panel`,
                () => {
                    const content = dockWidget(layoutId).node.textContent;
                    return texts.every((each) => content.includes(each));
                },
                30000
            ),

        openSettings: async () => {
            document.querySelector('#toolbar-settings-button').click();
            await waitFor(
                'the settings page',
                () => {
                    const search = document.querySelector('.settings-search-input');
                    return isShown(document.querySelector('.settings-page-background-blocker')) && search && !search.disabled;
                },
                30000
            );
            return true;
        },

        selectSettingsSection: async (name) => {
            const section = [...document.querySelectorAll('.settings-page-section-selector')].find((each) =>
                textOf(each).endsWith(name)
            );
            if (!section)
                throw new Error(`no settings section ${name}`);
            section.click();
            await waitFor(`the ${name} settings section`, () => section.classList.contains('active'));
            await sleep(500);
            return true;
        },

        searchSettings: async (query, selectedRow) => {
            const input = document.querySelector('.settings-search-input');
            input.focus();
            input.value = query;
            input.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertText', data: query }));
            await waitFor(`settings results for ${query}`, () =>
                document.querySelector('.settings-search-pane.open .settings-search-row')
            );
            for (let row = 0; row < selectedRow; ++row)
                input.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true }));
            await frames();
            return true;
        },

        closeSettings: async () => {
            const input = document.querySelector('.settings-search-input');
            if (document.querySelector('.settings-search-pane.open'))
                input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
            document.querySelector('.settings-page-header > button.script-nui-button').click();
            await waitFor('the settings page to close', () => !isShown(document.querySelector('.settings-page-background-blocker')));
            return true;
        },
    };
})();
true;
