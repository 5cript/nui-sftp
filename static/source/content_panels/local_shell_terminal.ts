import { NuiWidget } from './nui_widget';
import { ChannelId } from '../ids.tsx';

/**
 * A local-shell terminal panel, sibling to the SSH Terminal panel inside the
 * session's dock. Shares xterm.js plumbing with Terminal but:
 *  - identifies itself as `local-shell:<shellName>` in the layout so saved
 *    layouts round-trip to the correct shell config (bash, msys2, powershell…);
 *  - sets a dedicated title.className so `.lm-TabBar-tab.local-shell-widget`
 *    CSS can style the tab distinctly from SSH terminal tabs.
 */
class LocalShellTerminal extends NuiWidget {
    shellName: string;

    constructor(
        shellName: string,
        factory: () => HTMLElement | undefined,
        deleter: (_: ChannelId | undefined) => any
    ) {
        super(shellName, factory, () => {}, `local-shell:${shellName}`);
        this.shellName = shellName;

        this.deleter = () => {
            const channelElement = this.node.querySelector('.terminal-channel');
            if (channelElement) {
                const channelId = (channelElement as HTMLElement).dataset.channelid;
                if (channelId !== undefined) {
                    return deleter(channelId as ChannelId);
                }
                return;
            }
            deleter("INVALID_ID" as ChannelId);
        };

        this.title.label = shellName;
        this.title.closable = true;
        this.title.caption = shellName;
        this.title.className = 'local-shell-widget';
    }
}

export { LocalShellTerminal };