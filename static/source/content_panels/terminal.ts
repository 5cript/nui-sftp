import {
    NuiWidget
} from './nui_widget';
import { ChannelId } from '../ids.tsx';
import { Message } from '@lumino/messaging';

/// Dispatched on a terminal's channel element when its tab is clicked; marks it as last interacted.
const terminalTabActivatedEvent = 'terminal-tab-activated';

/**
 * Lumino activates a widget when the user clicks its tab, also when the tab is already current.
 */
function announceTerminalTabActivation(node: HTMLElement) {
    node.querySelector('.terminal-channel')?.dispatchEvent(new CustomEvent(terminalTabActivatedEvent));
}

class Terminal extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: (_: ChannelId | undefined) => any) {
        super(name, factory, () => {}, 'terminal', 'terminal');
        this.deleter = () => {
            const channelElement = this.node.querySelector('.terminal-channel');
            if (channelElement) {
                const channelId = (channelElement as HTMLElement).dataset.channelid;
                if (channelId !== undefined) {
                    return deleter(channelId as ChannelId);
                }
                return; // No channel ID: channel was never successfully created, nothing to delete
            }
            deleter("INVALID_ID" as ChannelId);
        };

        this.title.closable = true;
    }

    protected onActivateRequest(msg: Message): void {
        super.onActivateRequest(msg);
        announceTerminalTabActivation(this.node);
    }
}

export { Terminal, announceTerminalTabActivation };
