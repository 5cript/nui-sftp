import { ContentPanel, PanelFactories } from './content_panel';
import {
    DockPanel,
    Widget,
    TabBar
} from '@lumino/widgets';
import { ChannelId } from './ids.ts';

interface addPanelArguments {
    host: HTMLElement;
    id: string;
    engineType: string;
    layoutString: string;
    terminalFactory: () => HTMLElement;
    terminalDelete: (channelId: ChannelId | undefined) => any;
    localShellFactory: (shellName: string) => HTMLElement;
    localShellDelete: (channelId: ChannelId | undefined) => any;
    fileExplorerFactory: () => HTMLElement;
    fileExplorerDelete: () => any;
    operationQueueFactory: () => HTMLElement;
    operationQueueDelete: () => any;
    sessionOptionsFactory: () => HTMLElement;
    sessionOptionsDelete: () => any;
    fileTrackingFactory: () => HTMLElement;
    fileTrackingDelete: () => any;
    openAddContextMenu: (id: string | undefined) => void;
}

/**
 * Registry of ContentPanel instances, one per Session. The manager holds no
 * per-session state of its own — everything (factories, dock, open-add
 * callback) lives on the ContentPanel so sessions never leak into each
 * other.
 *
 * The only cross-panel bookkeeping is `lastAddRequest`, a short-lived pair
 * recording "user just clicked the + button in panel X; when the popup menu
 * resolves, fabricate whatever they chose into X's dock". It's cleared on
 * fulfilment and only one such request can be pending at a time anyway.
 */
class ContentPanelManager {
    private panels: Map<string, ContentPanel> = new Map();
    private lastAddRequest: { panelId: string; widget: TabBar<Widget> } | undefined;

    addPanel(args: addPanelArguments): boolean {
        if (!args) {
            console.error("No arguments provided to addPanel");
            return false;
        }
        if (!args.host) {
            console.error("No host provided to addPanel");
            return false;
        }
        if (!args.id) {
            console.error("No id provided to addPanel");
            return false;
        }
        if (!args.terminalFactory || !args.terminalDelete ||
            !args.localShellFactory || !args.localShellDelete ||
            !args.fileExplorerFactory || !args.fileExplorerDelete ||
            !args.operationQueueFactory || !args.operationQueueDelete ||
            !args.sessionOptionsFactory || !args.sessionOptionsDelete ||
            !args.fileTrackingFactory || !args.fileTrackingDelete ||
            !args.openAddContextMenu) {
            console.error("Missing one function argument to addPanel");
            return false;
        }

        const factories: PanelFactories = {
            terminalFactory: args.terminalFactory,
            terminalDelete: args.terminalDelete,
            localShellFactory: args.localShellFactory,
            localShellDelete: args.localShellDelete,
            fileExplorerFactory: args.fileExplorerFactory,
            fileExplorerDelete: args.fileExplorerDelete,
            operationQueueFactory: args.operationQueueFactory,
            operationQueueDelete: args.operationQueueDelete,
            sessionOptionsFactory: args.sessionOptionsFactory,
            sessionOptionsDelete: args.sessionOptionsDelete,
            fileTrackingFactory: args.fileTrackingFactory,
            fileTrackingDelete: args.fileTrackingDelete,
        };

        const panelId = args.id;
        const onAddRequested = (_sender: DockPanel, widget: TabBar<Widget>) => {
            try {
                const addSelector = (TabBar as any).addButtonSelector || TabBar.addButtonSelector;
                const addButton = widget.node.querySelector(addSelector) as HTMLElement | null;
                if (!addButton) {
                    console.error("Could not find add button in tab bar");
                    return;
                }
                if (!addButton.id) {
                    const newId = (globalThis as any).generateId() as string;
                    addButton.id = `add_button_${newId}`;
                }
                this.lastAddRequest = { panelId, widget };
                args.openAddContextMenu(addButton.id);
            } catch (e) {
                console.error(e);
            }
        };

        const panel = new ContentPanel(
            panelId,
            args.engineType,
            args.layoutString,
            factories,
            args.openAddContextMenu,
            onAddRequested,
        );

        try {
            Widget.attach(panel.main, args.host);
            panel.main.update();
        }
        catch (e) {
            console.error(e);
            console.log(JSON.stringify(e));
            return false;
        }

        const resizeObserver = new ResizeObserver(() => {
            panel.main.update();
        });
        resizeObserver.observe(args.host);

        this.panels.set(panelId, panel);
        return true;
    }

    removePanel = (id: string) => {
        const panel = this.panels.get(id);
        if (!panel) return;
        panel.detach();
        this.panels.delete(id);
        if (this.lastAddRequest && this.lastAddRequest.panelId === id) {
            this.lastAddRequest = undefined;
        }
    }

    getPanelById(id: string): ContentPanel | undefined {
        return this.panels.get(id);
    }

    /**
     * Kept for backwards compatibility with older callers that expected the
     * Lumino DockPanel directly. New code should prefer getPanelById and go
     * through ContentPanel's typed API.
     */
    getDockPanelById(id: string): DockPanel | undefined {
        return this.panels.get(id)?.dock;
    }

    getPanelLayout(id: string): any | undefined {
        return this.panels.get(id)?.saveLayout();
    }

    /**
     * Completes a pending "+ click → context menu" request by fabricating the
     * chosen widget with the originating panel's factories and inserting it
     * into that panel's dock. The request is cleared even if fabrication
     * fails so subsequent clicks aren't blocked.
     */
    fullfillLastAddRequest = (whatToAdd: string) => {
        const request = this.lastAddRequest;
        this.lastAddRequest = undefined;
        if (!request) {
            console.error("No last add request to fulfill");
            return;
        }
        const panel = this.panels.get(request.panelId);
        if (!panel) {
            console.error(`fullfillLastAddRequest: panel "${request.panelId}" no longer exists`);
            return;
        }
        const component = panel.fabricateComponentFromId(whatToAdd);
        if (!component) {
            console.error(`Could not fabricate component of type ${whatToAdd}`);
            return;
        }
        panel.dock.addWidget(component, { ref: request.widget.titles[0].owner });
    }

    renameTerminalById = (panelId: string, channelId: ChannelId, title: string) => {
        const panel = this.panels.get(panelId);
        if (!panel) {
            console.error(`renameTerminalById: no panel found for id ${panelId}`);
            return;
        }
        if (!panel.renameTerminal(channelId, title)) {
            console.error(`renameTerminalById: could not find terminal widget with channel id ${channelId}`);
        }
    }

    closeTerminalByNode = (panelId: string, node: HTMLElement) => {
        const panel = this.panels.get(panelId);
        if (!panel) {
            console.error(`closeTerminalByNode: no panel found for id ${panelId}`);
            return;
        }
        if (!panel.closeTerminalByNode(node)) {
            console.error('closeTerminalByNode: could not find terminal widget to close');
        }
    }

    closeTerminalById = (panelId: string, channelId: ChannelId) => {
        const panel = this.panels.get(panelId);
        if (!panel) {
            console.error(`closeTerminalById: no panel found for id ${panelId}`);
            return;
        }
        if (!panel.closeTerminalById(channelId)) {
            console.error(`closeTerminalById: could not find terminal widget with channel id ${channelId}`);
        }
    }
};
export { ContentPanelManager };