import { Message } from '@lumino/messaging';
import {
    NuiWidget
} from './nui_widget';

class SessionOptions extends NuiWidget {
    constructor(name: string, factory: () => HTMLElement | undefined, deleter: () => any) {
        super(name, factory, deleter, 'session-options', 'sessionOptions');

        this.title.closable = true;
    }
}

export { SessionOptions };
