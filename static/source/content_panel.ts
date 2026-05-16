import { Terminal } from './content_panels/terminal';
import { LocalShellTerminal } from './content_panels/local_shell_terminal';
import { FileExplorer } from './content_panels/file_explorer';
import { OperationQueue } from './content_panels/operation_queue';
import { SessionOptions } from './content_panels/session_options';
import { FileTracking } from './content_panels/file_tracking';
import {
    BoxPanel,
    DockPanel,
    Widget,
    TabBar,
    DockLayout
} from '@lumino/widgets';
import { MessageLoop } from '@lumino/messaging';
import { ChannelId } from './ids.ts';

/**
 * Factory / deleter callbacks that come from one specific Session's C++
 * Nui::bind lambdas. These capture that Session's `this`, its
 * FrontendSessionManager, its channelElements vector and layout id — so they
 * are inherently per-session and must NEVER bleed across sessions.
 */
export interface PanelFactories {
    terminalFactory: () => HTMLElement | undefined;
    terminalDelete: (channelId: ChannelId | undefined) => any;
    localShellFactory: (shellName: string) => HTMLElement | undefined;
    localShellDelete: (channelId: ChannelId | undefined) => any;
    fileExplorerFactory: () => HTMLElement | undefined;
    fileExplorerDelete: () => any;
    operationQueueFactory: () => HTMLElement | undefined;
    operationQueueDelete: () => any;
    sessionOptionsFactory: () => HTMLElement | undefined;
    sessionOptionsDelete: () => any;
    fileTrackingFactory: () => HTMLElement | undefined;
    fileTrackingDelete: () => any;
}

/**
 * A single session's Lumino dock panel tree plus the C++ factories that
 * build its widgets. One ContentPanel per Session — owning all the
 * per-session state that used to live (incorrectly) on the singleton
 * ContentPanelManager.
 *
 * The manager keeps a map of these keyed by layout id; per-session state
 * therefore can't be overwritten when a new session is added.
 */
export class ContentPanel {
    readonly id: string;
    readonly main: BoxPanel;
    readonly dock: DockPanel;
    readonly engineType: string;
    readonly factories: PanelFactories;
    readonly openAddContextMenu: (anchorId: string | undefined) => void;
    /// Observes the host element for size changes so Lumino re-lays-out the
    /// panel. Held on the panel so detach() can disconnect it — otherwise the
    /// observer pins `this.main` alive and keeps stale Nui::bind references
    /// to the owning Session around after close.
    private resizeObserver: ResizeObserver | undefined;

    constructor(
        id: string,
        engineType: string,
        layoutString: string | undefined,
        factories: PanelFactories,
        openAddContextMenu: (anchorId: string | undefined) => void,
        onAddRequested: (sender: DockPanel, widget: TabBar<Widget>) => void,
    ) {
        this.id = id;
        this.engineType = engineType;
        this.factories = factories;
        this.openAddContextMenu = openAddContextMenu;

        this.main = new BoxPanel({ spacing: 0 });
        this.main.id = 'main_' + id;

        if (layoutString === undefined || layoutString === null || layoutString === "") {
            this.dock = this.makeDefaultDock();
        } else {
            this.dock = this.makeDockFromLayout(layoutString);
        }
        this.dock.id = 'dock_' + id;

        this.dock.addRequested.connect(onAddRequested);
        this.main.addWidget(this.dock);
    }

    /**
     * Attach the Lumino tree to @p host and start observing its size. Called
     * from ContentPanelManager.addPanel after construction so attach errors
     * propagate cleanly back to C++.
     */
    attach(host: HTMLElement) {
        Widget.attach(this.main, host);
        this.main.update();
        this.resizeObserver = new ResizeObserver(() => {
            this.main.update();
        });
        this.resizeObserver.observe(host);
    }

    /**
     * Builds the widget for a layout id string. Local-shell tabs use the
     * prefixed form `local-shell:<shellName>`; plain strings pick the matching
     * generic panel type. Returns undefined if the factory can't service the
     * request (e.g. the named shell has been removed from settings) — callers
     * drop the widget rather than placing a blank tab.
     */
    fabricateComponentFromId(id: string): Widget | undefined {
        if (id.startsWith('local-shell:')) {
            const shellName = id.slice('local-shell:'.length);
            const node = this.factories.localShellFactory(shellName);
            if (!node) {
                console.warn(`Dropping local-shell tab for missing shell config "${shellName}"`);
                return undefined;
            }
            return new LocalShellTerminal(
                shellName,
                () => node,
                this.factories.localShellDelete
            );
        }
        switch (id) {
            case 'terminal':
                return new Terminal('Terminal', this.factories.terminalFactory, this.factories.terminalDelete);
            case 'file-explorer':
                return new FileExplorer('FileExplorer', this.factories.fileExplorerFactory, this.factories.fileExplorerDelete);
            case 'operation-queue':
                return new OperationQueue('OperationQueue', this.factories.operationQueueFactory, this.factories.operationQueueDelete);
            case 'session-options':
                return new SessionOptions('SessionOptions', this.factories.sessionOptionsFactory, this.factories.sessionOptionsDelete);
            case 'file-tracking':
                return new FileTracking('File Tracking', this.factories.fileTrackingFactory, this.factories.fileTrackingDelete);
            default:
                return undefined;
        }
    }

    /** Updates a channel's tab title by channelId. Returns false if not found. */
    renameTerminal(channelId: ChannelId, title: string): boolean {
        for (const widget of this.dock.widgets()) {
            const el = widget.node.querySelector(`.terminal-channel[data-channelid="${channelId}"]`);
            if (el) {
                widget.title.label = title;
                widget.title.caption = title;
                return true;
            }
        }
        return false;
    }

    /** Closes a channel widget by channelId. Returns false if not found. */
    closeTerminalById(channelId: ChannelId): boolean {
        for (const widget of this.dock.widgets()) {
            const el = widget.node.querySelector(`.terminal-channel[data-channelid="${channelId}"]`);
            if (el) {
                widget.close();
                return true;
            }
        }
        return false;
    }

    /** Closes a channel widget identified by its root DOM node. */
    closeTerminalByNode(node: HTMLElement): boolean {
        for (const widget of this.dock.widgets()) {
            if (widget.node === node) {
                widget.close();
                return true;
            }
        }
        return false;
    }

    /**
     * Returns the current Lumino layout as a serialisable JSON object where
     * widget references are replaced by their layoutId string (so a saved
     * layout can later be rehydrated through fabricateComponentFromId).
     */
    saveLayout(): any {
        return JSON.parse(JSON.stringify(this.dock.saveLayout(), (key, value) => {
            if (key === 'widgets') {
                return value.map((e: any) => e.layoutId);
            }
            return value;
        }));
    }

    /**
     * Tears down the Lumino tree on session close.
     *
     * Dispose is cascading: it detaches every widget from the DOM (firing
     * onAfterDetach → each widget's C++ deleter WHILE the Session is still
     * alive), disconnects all signal handlers, and drops Lumino's internal
     * references. That is critical — just removing the DOM element leaves
     * the widgets alive in JS memory still holding Nui::bind handles for the
     * Session that's about to be destroyed, and any later invocation of
     * those stale handles crashes the WASM runtime with
     * `getWasmTableEntry(index) is not a function`.
     */
    detach() {
        if (this.resizeObserver) {
            this.resizeObserver.disconnect();
            this.resizeObserver = undefined;
        }
        if (!this.main.isDisposed) {
            // Nui's reactive renderer can remove the Session's host div from
            // the DOM before contentPanelManager.removePanel runs.  At that
            // point main thinks it's still attached (isAttached === true) but
            // its node is no longer connected, so Widget.dispose → Widget.detach
            // would throw "Widget is not attached".  Send the detach messages
            // ourselves so child widgets get their onAfterDetach cascade
            // (NuiWidget's deleter relies on it) and the IsAttached flag is
            // cleared, then dispose proceeds without trying to remove an
            // already-removed node.
            if (this.main.isAttached && !this.main.node.isConnected) {
                MessageLoop.sendMessage(this.main, Widget.Msg.BeforeDetach);
                MessageLoop.sendMessage(this.main, Widget.Msg.AfterDetach);
            }
            try {
                this.main.dispose();
            } catch (e) {
                console.error(`[ContentPanel ${this.id}] detach: main.dispose() threw`, e);
                throw e;
            }
        }
    }

    // ── Dock construction ─────────────────────────────────────────────────

    private makeDefaultDock(): DockPanel {
        const f = this.factories;
        const term = new Terminal('Terminal', f.terminalFactory, f.terminalDelete);
        const explorer = new FileExplorer('FileExplorer', f.fileExplorerFactory, f.fileExplorerDelete);

        const dock = new DockPanel({
            addButtonEnabled: true,
        });
        dock.addWidget(term);
        dock.addWidget(explorer, { mode: 'split-right', ref: term });
        if (this.engineType === 'ssh') {
            const queue = new OperationQueue('OperationQueue', f.operationQueueFactory, f.operationQueueDelete);
            const fileTracking = new FileTracking('File Tracking', f.fileTrackingFactory, f.fileTrackingDelete);
            dock.addWidget(queue, { mode: 'split-bottom', ref: term });
            dock.addWidget(fileTracking, { mode: 'tab-after', ref: queue });
        }
        this.applyDefaultSplit(dock);
        return dock;
    }

    private makeDockFromLayout(layoutString: string): DockPanel {
        const dehydrated = JSON.parse(layoutString);

        const deserializeArea = (area: any): any => {
            if (!area) return null;

            const type = ((area as any).type as string) || 'unknown';
            if (type === 'unknown' || (type !== 'tab-area' && type !== 'split-area')) {
                console.error(`Attempted to deserialize unknown type: ${type}`);
                return null;
            }

            if (type === 'tab-area') {
                const { currentIndex, widgets } = area;
                const hydrated = {
                    type: 'tab-area',
                    currentIndex: currentIndex || 0,
                    widgets:
                        (widgets &&
                            (widgets.map((widget: any) => this.fabricateComponentFromId(widget))
                                    .filter((widget: any) => !!widget))) || [],
                };

                if (hydrated.currentIndex > hydrated.widgets.length - 1) {
                    hydrated.currentIndex = 0;
                }
                return hydrated;
            }

            if (type === 'split-area') {
                const { orientation, sizes, children } = area;
                const hydrated = {
                    type: 'split-area',
                    orientation,
                    sizes: sizes || [],
                    children:
                        (children &&
                            (children.map((child: any) => deserializeArea(child))
                                     .filter((child: any) => !!child))) || [],
                };
                return hydrated;
            }
        };

        const dock = new DockPanel({
            addButtonEnabled: true,
        });
        const area = { main: deserializeArea(dehydrated.main) };
        if (area) {
            const dockLayout = dock.layout as DockLayout;
            dockLayout.restoreLayout(area as DockPanel.ILayoutConfig);
        }
        return dock;
    }

    private applyDefaultSplit(dock: DockPanel) {
        const saved = dock.saveLayout();
        const main = saved.main as DockLayout.ISplitAreaConfig;
        if (main) {
            const children = main.children;
            if (children) {
                const first = children[0] as DockLayout.ISplitAreaConfig;
                if (first) {
                    first.sizes = [0.5, 0.5];
                }
            }
            main.sizes = [0.42, 0.58];
        }
        dock.restoreLayout(saved);
    }
}