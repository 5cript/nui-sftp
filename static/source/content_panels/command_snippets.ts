import {
    NuiWidget
} from './nui_widget';

class CommandSnippets extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'command-snippets');

        this.title.label = 'Command Snippets';
        this.title.closable = true;
        this.title.caption = 'Command Snippets';
    }
}

export { CommandSnippets };
