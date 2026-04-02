import { Message } from '@lumino/messaging';
import {
    NuiWidget
} from './nui_widget';
import { ChannelId } from '../ids.tsx';

class Terminal extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: (_: ChannelId | undefined) => any) {
        super(name, factory, () => {
            const channelElement = this.node.querySelector('.terminal-channel');
            if (channelElement) {
                const channelId = (channelElement as HTMLElement).dataset.channelid;
                if (channelId !== undefined) {
                    return deleter(channelId as ChannelId);
                }
                return; // No channel ID: channel was never successfully created, nothing to delete
            }
            deleter("INVALID_ID" as ChannelId);
        }, 'terminal');

        this.title.label = 'Terminal';
        this.title.closable = true;
        this.title.caption = 'Terminal';
    }


}

export { Terminal };
