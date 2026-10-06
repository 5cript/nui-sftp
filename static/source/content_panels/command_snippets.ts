import {
    NuiWidget
} from './nui_widget';

class CommandSnippets extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'command-snippets', 'commandSnippets');

        this.title.closable = true;
    }
}

export { CommandSnippets };
