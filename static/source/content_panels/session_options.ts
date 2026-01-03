import { Message } from '@lumino/messaging';
import {
    NuiWidget
} from './nui_widget';

class SessionOptions extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'session-options');

        this.title.label = 'Session Options';
        this.title.closable = true;
        this.title.caption = 'Session Options';
    }
}

export { SessionOptions };
