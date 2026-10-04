import {
    NuiWidget
} from './nui_widget';

class CommandHistory extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'command-history');

        this.title.label = 'Command History';
        this.title.closable = true;
        this.title.caption = 'Command History';
    }
}

export { CommandHistory };
