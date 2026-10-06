import {
    NuiWidget
} from './nui_widget';

class CommandHistory extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'command-history', 'commandHistory');

        this.title.closable = true;
    }
}

export { CommandHistory };
